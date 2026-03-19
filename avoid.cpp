// avoid.cpp
// Favorite-user tracking (vision only for now) + FOLLOW-ONLY controller.
// Initial behavior:
//   - Lock onto the closest visible person as favorite_id
//   - Keep tracking that same ID until lost
//   - If lost too long, reacquire the closest visible person again
//
// Build:
//   g++ -O2 -std=c++17 avoid.cpp -I . -o avoid
//
// Run:
//   ./avoid
//   ./avoid --replay inputs.jsonl
//
// Expected input JSON per line:
// {
//   "time_s": 12.3,
//   "ble_dist_m": null,
//   "vision": { "tracks":[
//       {"id":7,"theta_rad":0.12,"dist_m":2.0,"conf":0.88},
//       ...
//   ]}
// }
//
// Output JSON per line:
// {
//   "time_s": ...,
//   "mode": 0|1|4,
//   "favorite_id": ...,
//   "theta_t_rad": ...,
//   "dist_t_m": ...,
//   "v_ms": ...,
//   "wheel_L_ms": ...,
//   "wheel_R_ms": ...
// }

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#if __has_include(<nlohmann/json.hpp>)
  #include <nlohmann/json.hpp>
  using json = nlohmann::json;
#elif __has_include("json.hpp")
  #include "json.hpp"
  using json = nlohmann::json;
#else
  #error "Missing JSON header. Install nlohmann-json-dev or place json.hpp next to this file."
#endif

static inline double clamp(double x, double lo, double hi) {
  return std::max(lo, std::min(hi, x));
}

static inline double clampAbs(double x, double lim) {
  return clamp(x, -lim, lim);
}

struct TrackObs {
  int id = -1;
  double theta = 0.0;  // rad
  double dist  = 0.0;  // m
  double conf  = 0.0;
};

struct FrameIn {
  double time_s = 0.0;
  double ble_dist_m = std::numeric_limits<double>::quiet_NaN(); // optional
  std::vector<TrackObs> tracks;
};

struct CtrlOut {
  double v_ms = 0.0;
  double wheel_L_ms = 0.0;
  double wheel_R_ms = 0.0;
  int mode = 0;               // 0 acquire, 1 follow, 4 lost
  int favorite_id = -1;
  double theta_t = 0.0;
  double dist_t  = 0.0;
};

struct Params {
  // Rover geometry / caps
  double wheel_base_m = 0.40;
  double Vmax = 1.2;   // m/s
  double Wmax = 1.2;   // rad/s

  // Follow distance goal
  double d_target_m = 6.0 * 0.3048;  // 6 ft

  // Follow controller
  double Kp_d = 0.9;
  double Kp_theta = 1.6;

  // Favorite selection
  double conf_min = 0.45;
  int    lock_frames = 3;
  double lost_timeout_s = 1.0;

  // Speed ramp
  double dv_per_tick = 0.08; // m/s per frame
};

struct State {
  int favorite_id = -1;

  // acquisition state
  int candidate_id = -1;
  int candidate_win_count = 0;

  // target memory
  double last_theta = 0.0;
  double last_dist  = 0.0;
  double last_seen_t = -1e9;

  // BLE filter kept for future use
  bool ble_init = false;
  double ble_filt = 0.0;

  // speed ramp memory
  double last_v = 0.0;
};

static bool parseFrameJson(const std::string& line, FrameIn& in) {
  if (line.empty()) return false;

  json j = json::parse(line);

  in.time_s = j.value("time_s", 0.0);

  if (j.contains("ble_dist_m") && !j["ble_dist_m"].is_null()) {
    in.ble_dist_m = j["ble_dist_m"].get<double>();
  } else {
    in.ble_dist_m = std::numeric_limits<double>::quiet_NaN();
  }

  in.tracks.clear();
  if (j.contains("vision") && j["vision"].contains("tracks")) {
    for (const auto& t : j["vision"]["tracks"]) {
      TrackObs o;
      o.id    = t.value("id", -1);
      o.theta = t.value("theta_rad", 0.0);
      o.dist  = t.value("dist_m", 0.0);
      o.conf  = t.value("conf", 0.0);
      in.tracks.push_back(o);
    }
  }

  return true;
}

static const TrackObs* findTrack(const std::vector<TrackObs>& tracks, int id) {
  for (const auto& t : tracks) {
    if (t.id == id) return &t;
  }
  return nullptr;
}

static int pickClosestVisiblePerson(const FrameIn& in, const Params& P) {
  double bestDist = std::numeric_limits<double>::infinity();
  double bestAbsTheta = std::numeric_limits<double>::infinity();
  double bestConf = -1.0;
  int bestId = -1;

  for (const auto& t : in.tracks) {
    if (t.id < 0) continue;
    if (t.conf < P.conf_min) continue;
    if (!std::isfinite(t.dist) || t.dist <= 0.0) continue;

    const double absTheta = std::fabs(t.theta);
    bool better = false;

    // Primary: closest person
    if (t.dist < bestDist - 1e-6) {
      better = true;
    }
    // Tie-break 1: more centered
    else if (std::fabs(t.dist - bestDist) <= 1e-6 && absTheta < bestAbsTheta - 1e-6) {
      better = true;
    }
    // Tie-break 2: higher confidence
    else if (std::fabs(t.dist - bestDist) <= 1e-6 &&
             std::fabs(absTheta - bestAbsTheta) <= 1e-6 &&
             t.conf > bestConf) {
      better = true;
    }

    if (better) {
      bestDist = t.dist;
      bestAbsTheta = absTheta;
      bestConf = t.conf;
      bestId = t.id;
    }
  }

  return bestId;
}

static void updateFavorite(const FrameIn& in, const Params& P, State& S) {
  const double t = in.time_s;

  // Keep current favorite if still visible
  if (S.favorite_id >= 0) {
    const TrackObs* fav = findTrack(in.tracks, S.favorite_id);
    if (fav && fav->conf >= P.conf_min && std::isfinite(fav->dist) && fav->dist > 0.0) {
      S.last_theta = fav->theta;
      S.last_dist  = fav->dist;
      S.last_seen_t = t;
      return;
    }

    // Drop favorite if gone too long
    if ((t - S.last_seen_t) > P.lost_timeout_s) {
      S.favorite_id = -1;
      S.candidate_id = -1;
      S.candidate_win_count = 0;
    }

    return;
  }

  // No favorite yet: choose closest visible person
  int best = pickClosestVisiblePerson(in, P);

  if (best < 0) {
    S.candidate_id = -1;
    S.candidate_win_count = 0;
    return;
  }

  if (best == S.candidate_id) {
    S.candidate_win_count++;
  } else {
    S.candidate_id = best;
    S.candidate_win_count = 1;
  }

  if (S.candidate_win_count >= P.lock_frames) {
    S.favorite_id = S.candidate_id;
    S.candidate_id = -1;
    S.candidate_win_count = 0;

    const TrackObs* fav = findTrack(in.tracks, S.favorite_id);
    if (fav) {
      S.last_theta = fav->theta;
      S.last_dist  = fav->dist;
      S.last_seen_t = t;
    }
  }
}

static CtrlOut controlStep(const FrameIn& in, const Params& P, State& S) {
  CtrlOut out;

  // BLE filter retained for future use
  if (std::isfinite(in.ble_dist_m)) {
    if (!S.ble_init) {
      S.ble_init = true;
      S.ble_filt = in.ble_dist_m;
    }
    S.ble_filt = 0.25 * in.ble_dist_m + 0.75 * S.ble_filt;
  }

  updateFavorite(in, P, S);
  out.favorite_id = S.favorite_id;

  bool have_target = false;
  double theta_t = 0.0;
  double dist_t = 0.0;

  if (S.favorite_id >= 0) {
    const TrackObs* fav = findTrack(in.tracks, S.favorite_id);
    if (fav && fav->conf >= P.conf_min && std::isfinite(fav->dist) && fav->dist > 0.0) {
      theta_t = fav->theta;
      dist_t  = fav->dist;
      have_target = true;
    }
  }

  out.theta_t = theta_t;
  out.dist_t = dist_t;

  double v_cmd = 0.0;
  double w_cmd = 0.0;

  if (have_target) {
    // forward-only follow control
    double err_d = dist_t - P.d_target_m;
    v_cmd = clamp(P.Kp_d * err_d, 0.0, P.Vmax);
    w_cmd = clampAbs(P.Kp_theta * theta_t, P.Wmax);
    out.mode = 1; // FOLLOW
  } else {
    // acquire / lost behavior
    v_cmd = 0.10;
    w_cmd = 0.55;
    out.mode = (S.favorite_id < 0) ? 0 : 4; // ACQUIRE or LOST
  }

  // smooth forward-speed ramp
  double dv = v_cmd - S.last_v;
  dv = clamp(dv, -P.dv_per_tick, +P.dv_per_tick);
  v_cmd = clamp(S.last_v + dv, 0.0, P.Vmax);
  S.last_v = v_cmd;

  out.v_ms = v_cmd;
  out.wheel_L_ms = clamp(v_cmd - w_cmd * (P.wheel_base_m * 0.5), 0.0, P.Vmax);
  out.wheel_R_ms = clamp(v_cmd + w_cmd * (P.wheel_base_m * 0.5), 0.0, P.Vmax);

  return out;
}

static void printOutJson(const FrameIn& in, const CtrlOut& o) {
  json j;
  j["time_s"] = in.time_s;
  j["mode"] = o.mode;
  j["favorite_id"] = o.favorite_id;
  j["theta_t_rad"] = o.theta_t;
  j["dist_t_m"] = o.dist_t;
  j["v_ms"] = o.v_ms;
  j["wheel_L_ms"] = o.wheel_L_ms;
  j["wheel_R_ms"] = o.wheel_R_ms;

  std::cout << j.dump() << "\n" << std::flush;
}

int main(int argc, char** argv) {
  Params P;
  State S;

  auto runStream = [&](std::istream& is) {
    std::string line;
    while (std::getline(is, line)) {
      if (line.empty()) continue;

      FrameIn in;
      try {
        if (!parseFrameJson(line, in)) continue;
      } catch (const std::exception& e) {
        std::cerr << "[parse] " << e.what() << "\n";
        continue;
      }

      CtrlOut o = controlStep(in, P, S);
      printOutJson(in, o);
    }
    return 0;
  };

  if (argc == 3 && std::string(argv[1]) == "--replay") {
    std::ifstream fin(argv[2]);
    if (!fin) {
      std::cerr << "Could not open " << argv[2] << "\n";
      return 1;
    }
    return runStream(fin);
  }

  return runStream(std::cin);
}