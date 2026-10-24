#pragma once

// LCD backlight test command for the CrowPanel (the backlight sits behind the STC8 co-MCU, see
// main/boards/crowpanel_5inch.hpp). Registered only for that board.
//
//   backlight               show usage
//   backlight <0-100>       percent, same path as the firmware (Board::set_backlight_percent)
//   backlight raw <0-255>   write the STC8 PWM register directly (rail switched on)
//   backlight sweep [ms]    ramp the raw PWM value 0..255 in steps of 5 (default 100 ms per step)
//                           and print it, to see where the panel stops getting brighter

#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Feature::CLI::Commands {

namespace {

inline int cli_cmd_backlight(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "sweep") != 0) {
        char* end = nullptr;
        const long percent = std::strtol(argv[1], &end, 10);
        if (*end != '\0' || percent < 0 || percent > 100) {
            printf("Usage: backlight <0-100> | raw <0-255> | sweep [ms]\n");
            return 1;
        }
        const esp_err_t err = Board::set_backlight_percent(static_cast<int>(percent));
        printf("Backlight %ld%% (PWM %ld/%d): %s\n", percent,
               percent * CONFIG_CROWPANEL_BACKLIGHT_PWM_MAX / 100, CONFIG_CROWPANEL_BACKLIGHT_PWM_MAX,
               err == ESP_OK ? "OK" : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (argc == 3 && std::strcmp(argv[1], "raw") == 0) {
        char* end = nullptr;
        const long value = std::strtol(argv[2], &end, 10);
        if (*end != '\0' || value < 0 || value > 255) {
            printf("Usage: backlight raw <0-255>\n");
            return 1;
        }
        esp_err_t err = Board::stc8_set_gpio(CurrentBoard::STC8_GPIO_OUT_LCD_BL_POWER, value > 0);
        if (err == ESP_OK) {
            err = Board::stc8_write_reg(CurrentBoard::STC8_REG_SET_PWM + CurrentBoard::STC8_PWM_LCD_BL,
                                        static_cast<uint8_t>(value));
        }
        printf("Backlight PWM %ld/255: %s\n", value, err == ESP_OK ? "OK" : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if ((argc == 2 || argc == 3) && std::strcmp(argv[1], "sweep") == 0) {
        const long step_ms = argc == 3 ? std::strtol(argv[2], nullptr, 10) : 100;
        if (step_ms < 10 || step_ms > 5000) {
            printf("Usage: backlight sweep [10-5000 ms per step]\n");
            return 1;
        }
        esp_err_t err = Board::stc8_set_gpio(CurrentBoard::STC8_GPIO_OUT_LCD_BL_POWER, true);
        for (int value = 0; value <= 255 && err == ESP_OK; value += 5) {
            err = Board::stc8_write_reg(CurrentBoard::STC8_REG_SET_PWM + CurrentBoard::STC8_PWM_LCD_BL,
                                        static_cast<uint8_t>(value));
            printf("PWM %3d/255\n", value);
            vTaskDelay(pdMS_TO_TICKS(step_ms));
        }
        if (err != ESP_OK) printf("Failed: %s\n", esp_err_to_name(err));
        Board::set_backlight_percent(100); // back to the firmware's own full brightness
        printf("Sweep done, backlight back to 100%% (PWM %d).\n", CONFIG_CROWPANEL_BACKLIGHT_PWM_MAX);
        return err == ESP_OK ? 0 : 1;
    }
    printf("Usage: backlight <0-100> | raw <0-255> | sweep [ms]\n");
    return 1;
}

} // namespace

inline void register_backlight() {
    const esp_console_cmd_t cmd = {
        .command = "backlight",
        .help = "LCD backlight: <0-100> percent, raw <0-255> PWM register, sweep [ms] to find the range",
        .hint = "<0-100>|raw <0-255>|sweep [ms]",
        .func = &cli_cmd_backlight,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    register_command(cmd);
}

} // namespace Feature::CLI::Commands
