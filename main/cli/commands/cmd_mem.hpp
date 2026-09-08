#pragma once

#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

#include <cstdio>

namespace Feature::CLI::Commands {

namespace {

inline int cli_cmd_mem(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf("Free heap:       %u bytes\n", (unsigned)esp_get_free_heap_size());
    printf("Free internal:   %u bytes\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    printf("Free PSRAM:      %u bytes\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    printf("Min free (ever): %u bytes\n", (unsigned)esp_get_minimum_free_heap_size());
    return 0;
}

} // namespace

inline void register_mem() {
    const esp_console_cmd_t cmd = {
        .command = "mem",
        .help = "Show free heap memory (internal RAM + PSRAM)",
        .hint = nullptr,
        .func = &cli_cmd_mem,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    register_command(cmd);
}

} // namespace Feature::CLI::Commands
