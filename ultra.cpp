#include <gpiod.h>
#include <chrono>
#include <thread>
#include <iostream>

#define CHIPNAME "gpiochip0"
#define TRIG 23
#define ECHO 24

int main() {
    gpiod_chip *chip = gpiod_chip_open_by_name(CHIPNAME);
    gpiod_line *trig = gpiod_chip_get_line(chip, TRIG);
    gpiod_line *echo = gpiod_chip_get_line(chip, ECHO);

    gpiod_line_request_output(trig, "trig", 0);
    gpiod_line_request_input(echo, "echo");

    while (true) {
        // Send 10us pulse
        gpiod_line_set_value(trig, 1);
        std::this_thread::sleep_for(std::chrono::microseconds(10));
        gpiod_line_set_value(trig, 0);

        // Wait for echo HIGH
        while (gpiod_line_get_value(echo) == 0);

        auto start = std::chrono::high_resolution_clock::now();

        // Wait for echo LOW
        while (gpiod_line_get_value(echo) == 1);

        auto end = std::chrono::high_resolution_clock::now();

        std::chrono::duration<double> elapsed = end - start;

        double distance_cm = elapsed.count() * 34300 / 2;

        std::cout << "Distance: " << distance_cm << " cm" << std::endl;

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    gpiod_chip_close(chip);
    return 0;
}
