#pragma once

#include "esp_console.h"
#include "esp_system.h"

#include <cstdio>

namespace Feature::CLI::Commands {

namespace {

inline int cli_cmd_reset(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf("Restarting...\n");
    fflush(stdout);
    esp_restart();
    return 0; // unreachable
}

} // namespace

inline void register_reset() {
    const esp_console_cmd_t cmd = {
        .command = "reset",
        .help = "Restart the ESP32-P4",
        .hint = nullptr,
        .func = &cli_cmd_reset,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    register_command(cmd);
}

} // namespace Feature::CLI::Commands
