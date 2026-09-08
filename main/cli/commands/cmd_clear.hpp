#pragma once

#include "esp_console.h"

#include <cstdio>

namespace Feature::CLI::Commands {

/**
 * @brief ANSI: clear the whole screen and move the cursor home. Shared with "htop"
 *        (cmd_htop.hpp), which redraws the same way every refresh.
 */
inline void clear_screen() {
    printf("\033[2J\033[H");
    fflush(stdout);
}

namespace {

inline int cli_cmd_clear(int argc, char** argv) {
    (void)argc;
    (void)argv;
    clear_screen();
    return 0;
}

} // namespace

inline void register_clear() {
    const esp_console_cmd_t cmd = {
        .command = "clear",
        .help = "Clear the console screen",
        .hint = nullptr,
        .func = &cli_cmd_clear,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    register_command(cmd);
}

} // namespace Feature::CLI::Commands
