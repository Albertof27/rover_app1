#include <pigpio.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

struct Sonar {
    int trig;
    int echo;
    std::string name;
};

struct DistSnapshot {
    std::array<double, 4> cm{};
    uint64_t t_us = 0;   // pigpio tick when last updated
    uint64_t seq  = 0;   // increments every full 4-sensor scan
};

static inline uint64_t now_us_steady() {
    // pigpio tick is 32-bit and wraps; for our purpose as "recent timestamp", it’s OK.
    return static_cast<uint64_t>(gpioTick());
}

// Measure one HC-SR04 distance in cm. Returns -1 on timeout.
double read_distance_cm(int trig, int echo, unsigned timeout_us = 30000) {
    gpioWrite(trig, 0);
    gpioDelay(2);

    // 10 us trigger pulse
    gpioWrite(trig, 1);
    gpioDelay(10);
    gpioWrite(trig, 0);

    // wait for echo high
    uint32_t t_wait = gpioTick();
    while (gpioRead(echo) == 0) {
        if ((gpioTick() - t_wait) > timeout_us) return -1.0;
    }
    uint32_t t0 = gpioTick();

    // wait for echo low
    while (gpioRead(echo) == 1) {
        if ((gpioTick() - t0) > timeout_us) return -1.0;
    }
    uint32_t t1 = gpioTick();

    uint32_t dt_us = t1 - t0;

    // HC-SR04 approximation: cm = us / 58
    return static_cast<double>(dt_us) / 58.0;
}

bool file_exists_and_big(const std::string& path, std::streamoff min_bytes = 2000) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    return f.tellg() >= min_bytes;
}

// Snapshot with libcamera-still (simple + low processing, but spawns a process).
bool capture_jpeg(const std::string& out_path, int width, int height, int quality) {
    std::ostringstream cmd;
    cmd << "libcamera-still -n -t 1 "
        << "--width " << width << " --height " << height << " "
        << "--quality " << quality << " "
        << "-o " << out_path
        << " >/dev/null 2>&1";

    int rc = std::system(cmd.str().c_str());
    if (rc != 0) return false;
    return file_exists_and_big(out_path);
}

int main() {
    // ---- Tune these ---------------------------------------------------------
    const std::string out_dir = "/home/pi/frames"; // change if you want
    const int cam_w = 640;
    const int cam_h = 480;
    const int cam_quality = 75;

    // Snapshot rate (frames per second). Use 1 or 2 to stay lightweight.
    const double snapshot_fps = 2.0; // e.g., 1.0 for 1 fps, 2.0 for 2 fps

    // Ultrasonic scan pacing to avoid crosstalk.
    // This is the delay between sensors; total scan time ~ 4*(ping time + gap).
    const int inter_sensor_gap_ms = 35;
    // ------------------------------------------------------------------------

    // Init pigpio
    if (gpioInitialise() < 0) {
        std::cerr << "ERROR: pigpio init failed. Try:\n"
                  << "  sudo systemctl start pigpiod\n";
        return 1;
    }

    // Define your 4 sensors (BCM numbering)
    std::array<Sonar,4> s = {{
        {23, 24, "front"},
        {17, 27, "left"},
        { 5,  6, "right"},
        {16, 20, "rear"}
    }};

    // Setup pins
    for (auto &x : s) {
        gpioSetMode(x.trig, PI_OUTPUT);
        gpioSetMode(x.echo, PI_INPUT);
        gpioWrite(x.trig, 0);
        gpioSetPullUpDown(x.echo, PI_PUD_DOWN);
    }

    // Shared distance state (protected by a mutex)
    DistSnapshot latest;
    std::mutex latest_mtx;

    std::atomic<bool> running{true};

    // ---- Thread A: continuous ultrasonic scanning ---------------------------
    std::thread sonar_thread([&](){
        uint64_t seq = 0;

        while (running.load()) {
            DistSnapshot snap;
            snap.seq = ++seq;

            for (size_t i = 0; i < s.size(); i++) {
                snap.cm[i] = read_distance_cm(s[i].trig, s[i].echo);
                std::this_thread::sleep_for(std::chrono::milliseconds(inter_sensor_gap_ms));
            }

            snap.t_us = now_us_steady();

            {
                std::lock_guard<std::mutex> lk(latest_mtx);
                latest = snap;
            }
            // No extra sleep here; scan rate is governed by ping time + inter_sensor_gap_ms.
        }
    });

    // ---- Thread B: constant-rate camera snapshots ---------------------------
    std::thread cam_thread([&](){
        using clock = std::chrono::steady_clock;
        const auto period = std::chrono::duration<double>(1.0 / snapshot_fps);

        // Align to "now" and then tick forward by exactly one period each time
        auto next = clock::now() + std::chrono::duration_cast<clock::duration>(period);

        int frame = 0;

        while (running.load()) {
            std::this_thread::sleep_until(next);
            next += std::chrono::duration_cast<clock::duration>(period);

            frame++;

            // Build filename
            std::ostringstream path;
            path << out_dir << "/frame_" << std::setw(6) << std::setfill('0') << frame << ".jpg";

            // Capture snapshot (this can take some time, but sonar thread keeps running)
            bool ok = capture_jpeg(path.str(), cam_w, cam_h, cam_quality);

            // Grab latest distances (non-blocking w.r.t. sonar; just a quick mutex lock)
            DistSnapshot snap;
            {
                std::lock_guard<std::mutex> lk(latest_mtx);
                snap = latest;
            }

            // Print log line (you can also write CSV here)
            std::cout << "frame: " << frame
                      << " jpeg_ok: " << (ok ? 1 : 0)
                      << " file: " << path.str()
                      << " sonar_seq: " << snap.seq
                      << " sonar_t_us: " << snap.t_us
                      << " front: " << snap.cm[0]
                      << " left: "  << snap.cm[1]
                      << " right: " << snap.cm[2]
                      << " rear: "  << snap.cm[3]
                      << "\n";
        }
    });

    // ---- Main thread: run until Ctrl+C (simple) -----------------------------
    std::cout << "Running. Frames -> " << out_dir << "\n";
    std::cout << "Press Ctrl+C to stop.\n";

    // Basic idle loop
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // (Unreachable in this minimal version; if you want clean shutdown on Ctrl+C,
    // I can add signal handlers.)
    running.store(false);
    sonar_thread.join();
    cam_thread.join();

    gpioTerminate();
    return 0;
}
