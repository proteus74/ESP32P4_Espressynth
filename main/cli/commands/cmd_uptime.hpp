#pragma once

#include "esp_console.h"
#include "esp_timer.h"

#include <cstdio>

namespace Feature::CLI::Commands {

namespace {

inline int cli_cmd_uptime(int argc, char** argv) {
    (void)argc;
    (void)argv;

    uint64_t seconds = static_cast<uint64_t>(esp_timer_get_time() / 1000000);
    const uint64_t days = seconds / 86400;
    seconds %= 86400;
    const uint64_t hours = seconds / 3600;
    seconds %= 3600;
    const uint64_t minutes = seconds / 60;
    seconds %= 60;

    printf("up ");
    if (days > 0) printf("%llu day%s, ", (unsigned long long)days, days == 1 ? "" : "s");
    printf("%02llu:%02llu:%02llu\n", (unsigned long long)hours, (unsigned long long)minutes,
           (unsigned long long)seconds);
    return 0;
}

} // namespace

inline void register_uptime() {
    const esp_console_cmd_t cmd = {
        .command = "uptime",
        .help = "Show time since boot",
        .hint = nullptr,
        .func = &cli_cmd_uptime,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    register_command(cmd);
}

} // namespace Feature::CLI::Commands
