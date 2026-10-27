#pragma once

// LVGL refresh control, for measuring how much the display rendering disturbs the audio render
// task (compare p50/p99/peak in the WebSocket "cpuLoad" status with and without it).
//
//   lvgl pause    stop the LVGL timer: no more redraws/flushes (the panel keeps showing the last frame)
//   lvgl resume   restart it

#include "esp_console.h"
#include "esp_err.h"
#include "esp_lvgl_port.h"

#include <cstdio>
#include <cstring>

namespace Feature::CLI::Commands {

namespace {

inline int cli_cmd_lvgl(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "pause") == 0) {
        const esp_err_t err = lvgl_port_stop();
        printf("LVGL refresh paused: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "resume") == 0) {
        const esp_err_t err = lvgl_port_resume();
        printf("LVGL refresh resumed: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    printf("Usage: lvgl pause | resume\n");
    return 1;
}

} // namespace

inline void register_lvgl() {
    const esp_console_cmd_t cmd = {
        .command = "lvgl",
        .help = "Pause or resume the LVGL display refresh (pause|resume)",
        .hint = "pause|resume",
        .func = &cli_cmd_lvgl,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    register_command(cmd);
}

} // namespace Feature::CLI::Commands
