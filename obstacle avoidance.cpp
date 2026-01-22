/* ./app and ctrl c */
/**************************************************************
 * Luggage Rover Control – M0..M4 Simulation (C++) + Replay
 *
 * M0: Fixed-rate loop + logging (JSON + CSV), finite run
 * M1: Distance P-control for ~6 ft following, walking-speed caps
 * M2: Near-field STOP override from occupancy bubble
 * M3: Smoothing (IIR) + acceleration rate limit
 * M4: Reactive avoidance – when LoS is blocked (but not near), bias
 *     left/right and apply a turning + forward command until clear.
 *
 * cd "/Users/davidelem/Documents/capstone/capstone"
 * build: g++ -std=c++17 -O2 "obstacle avoidance.cpp" -I . -o rover_m4
 * run (sim):      ./rover_m4
 * run (replay):   ./rover_m4 --replay rover_inputs_15s.jsonl
 **************************************************************/

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>   // for setprecision in pretty prints
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <sstream>

// JSON single-header (we dropped json.hpp into ./nlohmann/)
#include <nlohmann/json.hpp>
using json = nlohmann::json;

using namespace std;
using namespace std::chrono;

/* ================== Parameters ================== */
// grid size HxW; each cell is CELL_M meters. Row 0 is "near" the rover.
static const int    H = 30, W = 50;
static const double CELL_M = 0.10;     // 10 cm per cell

// target gap to phone (~6 ft)
static const double TARGET_FT = 6.0;
static const double TARGET_M  = TARGET_FT * 0.3048;

// Safety + line-of-sight (LoS) checks
static const double NEAR_BUBBLE_M = 0.30;   // STOP if obstacle inside this radius
static const double LOS_CHECK_M   = 2.0;    // look this far ahead for LoS
static const int    LOS_HALF_COLS = 2;      // half-width (in cols) of the LoS corridor

// Loop rate + speed caps (keep to walking speed)
static const double LOOP_HZ      = 10.0;    // run at 10 Hz
static const int    LOOP_MS      = int(1000.0 / LOOP_HZ);
static const double WALK_V_MAX   = 1.35;    // m/s cap (~avg walking)
static const double AVOID_V_FWD  = 0.50;    // forward speed during avoidance
static const double AVOID_W      = 0.60;    // yaw rate during avoidance (rad/s)
static const double WHEEL_BASE_M = 0.40;    // wheel spacing (for v,w -> wheel speeds)

// Control tuning
static const double KP             = 0.8;   // P-gain on distance error
static const double RATE_LIMIT     = 0.05;  // accel limit per tick (~1 m/s^2)
static const double DIST_IIR_ALPHA = 0.25;  // smoothing for Bluetooth distance

// how long the built-in sim runs
static const double SIM_DURATION_S = 35.0;

/* ================= Types ================= */
// occupancy matrix is H*W (row-major). 0 = free, 1 = obstacle
struct ObstacleInfo { vector<uint8_t> matrix; };

struct InputPacket {
    double       time_s;               // tick time
    double       distance_to_phone_m;  // BLE distance (meters)
    ObstacleInfo obstacle_info;        // occupancy grid snapshot
};

struct OutputPacket {
    uint32_t seq;                      // sequence id
    double   time_s;                   // time
    double   FL, FR, RL, RR;           // wheel linear speeds (m/s)
    uint8_t  STOP;                     // 1 if near-bubble hit
    double   v_cmd, w_cmd;             // body forward/yaw commands
    int      mode;                     // 0 follow, 1 stop, 2 avoid_left, 3 avoid_right
    double   dist_raw_m, dist_filt_m, err_m, fwd_cmd_mps; // debug values
};

/* ================= Helpers ================= */
// basic clamp-by-magnitude
static inline double clampAbs(double x, double lim){ return x>lim?lim:(x<-lim?-lim:x); }
// (row, col) -> flat index
static inline size_t idx(int r,int c){ return size_t(r)*W + size_t(c); }
// accel limiter so v ramps cleanly
static inline double rateLimit(double cmd,double last,double step,double maxMag){
    double d = cmd - last;
    if (fabs(d) > step) d = (d>0 ? step : -step);
    return clampAbs(last + d, maxMag);
}
// meters → feet 
static inline double m2ft(double m){ return m / 0.3048; }

/* ================= LoS & Side-Freedom ================= */
bool losBlocked(const vector<uint8_t>& M){
    int rows = min(H-1, int(round(LOS_CHECK_M / CELL_M)));
    int c0 = max(0, W/2 - LOS_HALF_COLS), c1 = min(W-1, W/2 + LOS_HALF_COLS);
    for(int r=0;r<=rows;++r) for(int c=c0;c<=c1;++c) if(M[idx(r,c)]==1) return true;
    return false;
}
// count obstacles left vs right out to LOS_CHECK_M; pick freer side (tie → right)
int freerSide(const vector<uint8_t>& M){
    int rows = min(H-1, int(round(LOS_CHECK_M / CELL_M)));
    int mid = W/2, L=0,R=0;
    for(int r=0;r<=rows;++r){
        for(int c=0;c<mid;++c) if(M[idx(r,c)]==1) ++L;
        for(int c=mid+1;c<W;++c) if(M[idx(r,c)]==1) ++R;
    }
    if (L<R) return 2; if (R<L) return 3; return 3; // tie → right
}

/* ================= Simulated Input (demo obstacles) ================= */
// fake inputs so the sim path can run on its own (M5 uses --replay instead)
InputPacket fakeInput(double t_s){
    // distance ~6 ft with small wobble + noise (pretend Bluetooth)
    double d_ft = 6.0 + 0.6 * sin(2.0*M_PI*0.08*t_s);
    double d_m  = d_ft * 0.3048;
    static std::mt19937 rng(42);
    static std::normal_distribution<double> n(0.0,0.03);
    d_m += n(rng);

    vector<uint8_t> M(size_t(H)*W, 0);

    // Near-field obstacle toggles 4s on/4s off (exercise STOP behavior)
    bool near_on = ( (int)floor(t_s) % 8 >= 4 );
    if (near_on){
        int rows = max(1, int(round(NEAR_BUBBLE_M/CELL_M)));
        int r0=0,r1=min(H-1,rows-1), c0=max(0,W/2-2), c1=min(W-1,W/2+2);
        for(int r=r0;r<=r1;++r) for(int c=c0;c<=c1;++c) M[idx(r,c)]=1;
    }

    // Mid-field "wall" 10s..20s to trigger avoidance
    if (t_s>=10.0 && t_s<=20.0){
        int r0 = max(0, int(round(1.0/CELL_M)));   // ~1 m ahead
        int r1 = min(H-1, int(round(1.8/CELL_M))); // ~1.8 m
        int c0 = max(0, W/2-LOS_HALF_COLS), c1 = min(W-1, W/2+LOS_HALF_COLS);
        for(int r=r0;r<=r1;++r) for(int c=c0;c<=c1;++c) M[idx(r,c)]=1;
        // open up the right side a bit so we consistently choose avoid_right
        for(int r=r0;r<=r1;++r) for(int c=W/2+5;c<min(W, W/2+10);++c) M[idx(r,c)]=0;
    }

    InputPacket in;
    in.time_s=t_s; in.distance_to_phone_m=d_m; in.obstacle_info.matrix=std::move(M);
    return in;
}

/* ================= Safety Bubble ================= */
// if any occupied cell inside the near "bubble" → STOP
uint8_t nearFieldStop(const vector<uint8_t>& M){
    int rows = max(1, int(round(NEAR_BUBBLE_M / CELL_M)));
    int r0=0,r1=min(H-1,rows-1), c0=max(0,W/2-3), c1=min(W-1,W/2+3);
    for(int r=r0;r<=r1;++r) for(int c=c0;c<=c1;++c) if(M[idx(r,c)]==1) return 1;
    return 0;
}

/* ================= JSON (sim print) ================= */
// original per-tick JSON line (useful if you pipe to a file / plotting later)
void printJson(const OutputPacket& p){
    cout.setf(std::ios::fixed); cout.precision(6);
    cout<<"{"
        <<"\"seq\":"<<p.seq<<",\"time\":"<<p.time_s
        <<",\"FL\":"<<p.FL<<",\"FR\":"<<p.FR<<",\"RL\":"<<p.RL<<",\"RR\":"<<p.RR
        <<",\"STOP\":"<<(int)p.STOP<<",\"v\":"<<p.v_cmd<<",\"w\":"<<p.w_cmd
        <<",\"mode\":"<<p.mode
        <<",\"dist_raw_m\":"<<p.dist_raw_m<<",\"dist_filt_m\":"<<p.dist_filt_m
        <<",\"err_m\":"<<p.err_m<<",\"fwd_cmd\":"<<p.fwd_cmd_mps<<"}\n";
}

/* ================= Replay helpers (pretty prints) ================= */
// map mode id → label
static inline const char* modeName(int m){
    switch(m){ case 0:return "FOLLOW"; case 1:return "STOP";
               case 2:return "AVOID_LEFT"; case 3:return "AVOID_RIGHT"; }
    return "UNKNOWN";
}



// one JSONL line → InputPacket (time, distance, occupancy)
bool parseJsonLine(const std::string& line, InputPacket& out){
    if(line.empty()) return false;
    json j = json::parse(line);

    out.time_s = j.at("time_s").get<double>();
    out.distance_to_phone_m = j.at("distance_to_phone_m").get<double>();

    const auto& occ = j.at("occupancy");
    int h = occ.at("H").get<int>();
    int w = occ.at("W").get<int>();
    const auto& cells = occ.at("cells");

    out.obstacle_info.matrix.assign((size_t)h*(size_t)w, 0u);
    for(size_t k=0;k<out.obstacle_info.matrix.size();++k){
        out.obstacle_info.matrix[k] = (uint8_t)cells.at(k).get<int>();
    }

    // warn if the file grid size doesn’t match our compile-time HxW (we still proceed)
    if (h!=H || w!=W){
        std::cerr << "[warn] JSON grid is " << h << "x" << w
                  << " but code expects " << H << "x" << W << ".\n";
    }
    return true;
}

/* ================= Replay mode (M5 validation path) ================= */
// Reads JSONL frames → runs the SAME control as sim → prints human-readable decisions + ASCII
int runReplayFromJsonl(const std::string& path){
    std::ifstream fin(path);
    if(!fin){ std::cerr<<"Could not open "<<path<<"\n"; return 1; }

    // NEW: write replay CSV with wheel speeds for plotting
    std::ofstream replay("replay_log.csv");
    replay.setf(std::ios::fixed); replay.precision(6);
    replay << "seq,time_s,dist_raw_m,dist_filt_m,v_cmd,w_cmd,FL,FR,RL,RR,STOP,mode\n";

    double dist_filt = TARGET_M; // start filter at target gap
    double lastV = 0.0;          // for accel limiting
    std::string line; int tick=0;

    while(std::getline(fin, line)){
        InputPacket in;
        try{
            if(!parseJsonLine(line, in)) continue; // skip blanks
        }catch(const std::exception& e){
            std::cerr<<"JSON parse error: "<<e.what()<<"\n"; continue;
        }
        tick++;

        const auto& M = in.obstacle_info.matrix;

        // M3: smooth Bluetooth distance (IIR)
        double dist_raw = in.distance_to_phone_m;
        dist_filt = DIST_IIR_ALPHA*dist_raw + (1.0-DIST_IIR_ALPHA)*dist_filt;

        // M2 + M4: safety bubble + LoS check
        uint8_t STOP = nearFieldStop(M);
        bool blocked = losBlocked(M);

        // M1/M4: choose mode + commands
        int mode=0; double v_cmd=0.0, w_cmd=0.0;
        double err = dist_filt - TARGET_M;
        double fwd_cmd = clampAbs(KP*err, WALK_V_MAX);
        std::string reason;

        if (STOP){
            mode=1; v_cmd=0.0; w_cmd=0.0; reason="near bubble hit";
        } else if (blocked){
            int side = freerSide(M); mode=side; // 2=LEFT, 3=RIGHT
            v_cmd = std::min(AVOID_V_FWD, WALK_V_MAX);
            w_cmd = (side==2)? +AVOID_W : -AVOID_W;
            reason = std::string("LoS blocked → ") + (side==2?"avoid_left":"avoid_right");
        } else {
            mode=0; v_cmd=fwd_cmd; w_cmd=0.0; reason="LoS clear; P-follow";
        }

        // M3: rate-limit forward v so ramps are smooth
        v_cmd = rateLimit(v_cmd, lastV, RATE_LIMIT, WALK_V_MAX);
        if (STOP) v_cmd = 0.0; // STOP overrides all motion
        lastV = v_cmd;

        // convert (v,w) to left/right wheel speeds
        double vL = clampAbs(v_cmd - w_cmd*(WHEEL_BASE_M*0.5), WALK_V_MAX);
        double vR = clampAbs(v_cmd + w_cmd*(WHEEL_BASE_M*0.5), WALK_V_MAX);

        // Pretty print one line per tick + ASCII grid (near band)
        std::cout.setf(std::ios::fixed); std::cout.precision(2);
        std::cout<<"Tick "<<tick<<"  t="<<in.time_s<<"s  "<<modeName(mode)
         <<"  v="<<v_cmd<<"  w="<<w_cmd
         <<"  (vL="<<vL<<", vR="<<vR<<")  "
         <<"reason: "<<reason
         <<"  |  dist_raw="<<dist_raw<<" m ("<<m2ft(dist_raw)<<" ft)"
         <<"  dist_smooth="<<dist_filt<<" m ("<<m2ft(dist_filt)<<" ft)"
         <<"\n";
       

        // --- NEW: append one CSV line per frame ---
        replay << tick << "," << in.time_s << ","
               << dist_raw << "," << dist_filt << ","
               << v_cmd << "," << w_cmd << ","
               << vL << "," << vR << ","   // FL, FR
               << vL << "," << vR << ","   // RL, RR (same for diff-drive)
               << int(STOP) << "," << mode << "\n";
    }

    // NEW: close replay CSV
    replay.close();
    return 0;
}

/* ================= Main ================= */
int main(int argc, char** argv){
    // If I pass --replay, run the JSONL reader instead of the built-in sim
    if (argc==3 && std::string(argv[1])=="--replay"){
        return runReplayFromJsonl(argv[2]);
    }

    // ===== your existing simulation path (unchanged) =====
    ofstream csv("rover_log_m4.csv"); // CSV for plotting
    csv<<"seq,time_s,dist_raw_m,dist_filt_m,err_m,fwd_cmd_mps,v_cmd,w_cmd,FL,FR,RL,RR,STOP,mode\n";
    csv.setf(std::ios::fixed); csv.precision(6);

    uint32_t seq=0;
    double lastV=0.0;            // accel limiter memory
    double dist_filt = TARGET_M; // start filter at target

    auto t0=steady_clock::now(), next=t0;
    while(true){
        auto now=steady_clock::now();
        double tS = duration<double>(now - t0).count();
        if (tS>=SIM_DURATION_S) break; // stop after SIM_DURATION_S

        // M0: synthesize one input tick (distance + occupancy)
        InputPacket in = fakeInput(tS);
        const auto& M = in.obstacle_info.matrix;

        // M3: smooth Bluetooth distance
        double dist_raw = in.distance_to_phone_m;
        dist_filt = DIST_IIR_ALPHA*dist_raw + (1.0-DIST_IIR_ALPHA)*dist_filt;

        // M2 + M4: safety + LoS
        uint8_t STOP = nearFieldStop(M);
        bool blocked = losBlocked(M);

        // M1/M4: pick mode + compute (v,w)
        int mode=0; double v_cmd=0.0, w_cmd=0.0;
        double err = dist_filt - TARGET_M;
        double fwd_cmd = clampAbs(KP*err, WALK_V_MAX);
        if (STOP){
            mode=1; v_cmd=0.0; w_cmd=0.0;
        } else if (blocked){
            int side = freerSide(M); mode=side; // 2=LEFT, 3=RIGHT
            v_cmd = min(AVOID_V_FWD, WALK_V_MAX);
            w_cmd = (side==2)? +AVOID_W : -AVOID_W;
        } else {
            mode=0; v_cmd=fwd_cmd; w_cmd=0.0;
        }

        // M3: rate-limit forward speed
        v_cmd = rateLimit(v_cmd, lastV, RATE_LIMIT, WALK_V_MAX);
        if (STOP) v_cmd = 0.0;
        lastV = v_cmd;

        // (v,w) -> wheel speeds
        double vL = clampAbs(v_cmd - w_cmd*(WHEEL_BASE_M*0.5), WALK_V_MAX);
        double vR = clampAbs(v_cmd + w_cmd*(WHEEL_BASE_M*0.5), WALK_V_MAX);

        // pack + print one JSON line (original behavior)
        OutputPacket out; out.seq=++seq; out.time_s=in.time_s;
        out.STOP=STOP; out.v_cmd=v_cmd; out.w_cmd=w_cmd; out.mode=mode;
        out.dist_filt_m=dist_filt; out.err_m=err; out.fwd_cmd_mps=fwd_cmd;
        out.FL=vL; out.RL=vL; out.FR=vR; out.RR=vR;

        printJson(out);

        // write one CSV row
        csv<<out.seq<<","<<out.time_s<<","<<out.dist_filt_m<<","<<out.err_m<<","<<out.fwd_cmd_mps<<","
           <<out.v_cmd<<","<<out.w_cmd<<","<<out.FL<<","<<out.FR<<","<<out.RL<<","<<out.RR<<","<<(int)out.STOP<<","<<out.mode<<"\n";

        // hold 10 Hz loop timing
        next += milliseconds(LOOP_MS);
        this_thread::sleep_until(next);
    }
    csv.close();
    return 0;
}
