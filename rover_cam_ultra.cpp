#include <pigpio.h>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <array>

struct Sonar {
    int trig;
    int echo;
    std::string name;
};

// Measure one HC-SR04 distance in cm. Returns -1 on timeout.
double read_distance_cm(int trig, int echo, unsigned timeout_us = 30000) {
    gpioWrite(trig, 0);
    gpioDelay(2);

    // 10 us trigger
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
    return static_cast<double>(dt_us) / 58.0; // HC-SR04 conversion
}

bool file_exists_and_big(const std::string& path, std::streamoff min_bytes = 2000) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    return f.tellg() >= min_bytes;
}

// Capture a JPEG using libcamera-still into out_path.
bool capture_jpeg(const std::string& out_path, int width, int height, int quality = 85) {
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
    // Init pigpio
    if (gpioInitialise() < 0) {
        std::cerr << "ERROR: pigpio init failed. Try:\n"
                  << "  sudo systemctl start pigpiod\n";
        return 1;
    }

    // Sonar pin map (BCM numbers) — adjust as needed
    std::array<Sonar,4> s = {{
        {23, 24, "front"},
        {17, 27, "left"},
        { 5,  6, "right"},
        {16, 20, "rear"}
    }};

    for (auto &x : s) {
        gpioSetMode(x.trig, PI_OUTPUT);
        gpioSetMode(x.echo, PI_INPUT);
        gpioWrite(x.trig, 0);
        gpioSetPullUpDown(x.echo, PI_PUD_DOWN);
    }

    // Make an output folder (optional). If you want a folder, set it here:
    const std::string out_dir = "."; // change to "/home/pi/frames" if you want

    int frame = 0;

    while (true) {
        frame++;

        // 1) Read all 4 sensors sequentially to avoid crosstalk
        std::array<double,4> d{};
        for (size_t i = 0; i < s.size(); i++) {
            d[i] = read_distance_cm(s[i].trig, s[i].echo);
            std::this_thread::sleep_for(std::chrono::milliseconds(35));
        }

        // 2) Capture a JPEG snapshot
        std::ostringstream path;
        path << out_dir << "/frame_" << std::setw(4) << std::setfill('0') << frame << ".jpg";

        bool ok = capture_jpeg(path.str(), 1280, 720, 85);

        // 3) Log results
        std::cout << "frame: " << frame << "\n";
        std::cout << "jpeg_ok: " << (ok ? 1 : 0) << " file: " << path.str() << "\n";
        for (size_t i = 0; i < s.size(); i++) {
            std::cout << s[i].name << "_cm: " << d[i] << "\n";
        }
        std::cout << "---\n";

        // pacing
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    gpioTerminate();
    return 0;
}
