#include <gpiod.h>
#include <chrono>
#include <thread>
#include <iostream>

#define CHIP "/dev/gpiochip0"
#define TRIG 23
#define ECHO 24

static inline void sleep_us(int us) {
    std::this_thread::sleep_for(std::chrono::microseconds(us));
}

int main() {
    gpiod_chip *chip = gpiod_chip_open(CHIP);
    if (!chip) {
        std::cerr << "Failed to open gpiochip\n";
        return 1;
    }

    gpiod_line *trig = gpiod_chip_get_line(chip, TRIG);
    gpiod_line *echo = gpiod_chip_get_line(chip, ECHO);

    if (!trig || !echo) {
        std::cerr << "Failed to get GPIO lines\n";
        return 1;
    }

    // Request TRIG as output
    if (gpiod_line_request_output(trig, "trig", 0) < 0) {
        std::cerr << "Failed to request TRIG as output\n";
        return 1;
    }

    // Request ECHO as input
    if (gpiod_line_request_input(echo, "echo") < 0) {
        std::cerr << "Failed to request ECHO as input\n";
        return 1;
    }

    while (true) {
        // Trigger pulse
        gpiod_line_set_value(trig, 1);
        sleep_us(10);
        gpiod_line_set_value(trig, 0);

        // Wait for echo HIGH
        auto timeout_start = std::chrono::high_resolution_clock::now();
        while (gpiod_line_get_value(echo) == 0) {
            auto now = std::chrono::high_resolution_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - timeout_start).count() > 30) {
                std::cout << "Timeout waiting for echo HIGH\n";
                goto wait_and_repeat;
            }
        }

        auto start = std::chrono::high_resolution_clock::now();

        // Wait for echo LOW
        while (gpiod_line_get_value(echo) == 1);

        auto end = std::chrono::high_resolution_clock::now();

        auto pulse_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        double distance_cm = (pulse_us * 0.0343) / 2.0;

        std::cout << "Distance: " << distance_cm << " cm\n";

    wait_and_repeat:
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    gpiod_chip_close(chip);
    return 0;
}
