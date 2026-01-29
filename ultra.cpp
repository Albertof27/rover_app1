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
    // Open chip
    gpiod_chip *chip = gpiod_chip_open(CHIP);

    // Configure TRIG as output
    gpiod_line_settings *trig_settings = gpiod_line_settings_new();
    gpiod_line_settings_set_direction(trig_settings, GPIOD_LINE_DIRECTION_OUTPUT);

    gpiod_line_config *trig_config = gpiod_line_config_new();
    gpiod_line_config_add_line_settings(trig_config, (unsigned int[]){TRIG}, 1, trig_settings);

    gpiod_request_config *req_cfg = gpiod_request_config_new();
    gpiod_request_config_set_consumer(req_cfg, "hcsr04");

    gpiod_line_request *trig_req =
        gpiod_chip_request_lines(chip, req_cfg, trig_config);

    // Configure ECHO as input
    gpiod_line_settings *echo_settings = gpiod_line_settings_new();
    gpiod_line_settings_set_direction(echo_settings, GPIOD_LINE_DIRECTION_INPUT);

    gpiod_line_config *echo_config = gpiod_line_config_new();
    gpiod_line_config_add_line_settings(echo_config, (unsigned int[]){ECHO}, 1, echo_settings);

    gpiod_line_request *echo_req =
        gpiod_chip_request_lines(chip, req_cfg, echo_config);

    while (true) {
        // 10us trigger pulse
        gpiod_line_request_set_value(trig_req, TRIG, 1);
        sleep_us(10);
        gpiod_line_request_set_value(trig_req, TRIG, 0);

        // Wait for echo HIGH
        int val = 0;
        auto t0 = std::chrono::high_resolution_clock::now();
        while (val == 0) {
            gpiod_line_request_get_value(echo_req, ECHO, &val);
            auto now = std::chrono::high_resolution_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count() > 30) {
                std::cout << "Timeout\n";
                goto delay;
            }
        }

        auto start = std::chrono::high_resolution_clock::now();

        // Wait for echo LOW
        while (val == 1) {
            gpiod_line_request_get_value(echo_req, ECHO, &val);
        }

        auto end = std::chrono::high_resolution_clock::now();

        auto pulse_us =
            std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

        double distance_cm = (pulse_us * 0.0343) / 2.0;
        std::cout << "Distance: " << distance_cm << " cm\n";

    delay:
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    return 0;
}


    gpiod_chip_close(chip);
    return 0;
}
