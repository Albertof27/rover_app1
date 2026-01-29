#include <cstdlib>
#include <chrono>
#include <thread>
#include <iostream>
#include <sstream>
#include <iomanip>

int main() {
    const int capture_interval_ms = 500;   // 2 frames per second
    int frame_count = 0;

    std::cout << "Starting camera capture loop...\n";

    while (true) {
        // Create filename like frame_0001.jpg
        std::stringstream filename;
        filename << "frames/frame_"
                 << std::setw(4) << std::setfill('0')
                 << frame_count++ << ".jpg";

        // Build libcamera command
        std::stringstream cmd;
        cmd << "libcamera-still "
            << "--nopreview "
            << "--immediate "
            << "--width 640 --height 480 "
            << "-o " << filename.str();

        std::cout << "Capturing: " << filename.str() << std::endl;

        system(cmd.str().c_str());

        // Wait before next frame (lets CPU breathe for your processing)
        std::this_thread::sleep_for(std::chrono::milliseconds(capture_interval_ms));
    }

    return 0;
}
