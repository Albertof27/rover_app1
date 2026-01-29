#include <gpiod.hpp>
#include <chrono>
#include <thread>
#include <iostream>

static constexpr const char* CHIP = "/dev/gpiochip0";
static constexpr int TRIG = 23; // BCM GPIO23
static constexpr int ECHO = 24; // BCM GPIO24

// Busy-wait helper (simple + works fine for testing)
static inline void sleep_us(int us) {
    std::this_thread::sleep_for(std::chrono::microseconds(us));
}

int main() {
    try {
        gpiod::chip chip(CHIP);

        // Get GPIO lines
        gpiod::line trig = chip.get_line(TRIG);
        gpiod::line echo = chip.get_line(ECHO);

        // Request TRIG as output, start low
        trig.request(
            {"hcsr04-trig", gpiod::line_request::DIRECTION_OUTPUT, 0},
            0
        );

        // Request ECHO as input
        echo.request(
            {"hcsr04-echo", gpiod::line_request::DIRECTION_INPUT, 0}
        );

        while (true) {
            // Ensure trig low
            trig.set_value(0);
            sleep_us(2);

            // 10us trigger pulse
            trig.set_value(1);
            sleep_us(10);
            trig.set_value(0);

            // Wait for echo HIGH (timeout)
            auto t0 = std::chrono::high_resolution_clock::now();
            while (echo.get_value() == 0) {
                auto now = std::chrono::high_resolution_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count() > 30) {
                    std::cout << "Timeout waiting for echo HIGH\n";
                    goto pause_and_continue;
                }
            }

            // Measure HIGH pulse width
            auto start = std::chrono::high_resolution_clock::now();
            while (echo.get_value() == 1) {
                auto now = std::chrono::high_resolution_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() > 30) {
                    std::cout << "Timeout waiting for echo LOW\n";
                    goto pause_and_continue;
                }
            }
            auto end = std::chrono::high_resolution_clock::now();

            auto pulse_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

            // distance(cm) = (time_us * 0.0343) / 2
            double distance_cm = (pulse_us * 0.0343) / 2.0;

            std::cout << "Distance: " << distance_cm << " cm\n";

        pause_and_continue:
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

    } catch (const std::exception& e) {
        std::cerr << "GPIO error: " << e.what() << "\n";
        return 1;
    }
}

