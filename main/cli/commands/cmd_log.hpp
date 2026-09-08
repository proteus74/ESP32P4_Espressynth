#pragma once

#include "esp_console.h"
#include "esp_log.h"

#include <cstdio>
#include <cstring>

namespace Feature::CLI::Commands {

namespace {

inline bool parse_level(const char* str, esp_log_level_t& out) {
    struct { const char* name; esp_log_level_t level; } levels[] = {
        {"none", ESP_LOG_NONE},   {"error", ESP_LOG_ERROR}, {"warn", ESP_LOG_WARN},
        {"info", ESP_LOG_INFO},   {"debug", ESP_LOG_DEBUG}, {"verbose", ESP_LOG_VERBOSE},
    };
    for (const auto& entry : levels) {
        if (std::strcmp(str, entry.name) == 0) {
            out = entry.level;
            return true;
        }
    }
    if (std::strlen(str) == 1 && str[0] >= '0' && str[0] <= '5') {
        out = static_cast<esp_log_level_t>(str[0] - '0');
        return true;
    }
    return false;
}

inline const char* level_name(esp_log_level_t level) {
    switch (level) {
        case ESP_LOG_NONE:    return "none";
        case ESP_LOG_ERROR:   return "error";
        case ESP_LOG_WARN:    return "warn";
        case ESP_LOG_INFO:    return "info";
        case ESP_LOG_DEBUG:   return "debug";
        case ESP_LOG_VERBOSE: return "verbose";
        default:              return "?";
    }
}

inline int cli_cmd_log(int argc, char** argv) {
    if (argc == 2) {
        printf("%s: %s\n", argv[1], level_name(esp_log_level_get(argv[1])));
        return 0;
    }
    if (argc == 3) {
        esp_log_level_t level;
        if (!parse_level(argv[2], level)) {
            printf("Unknown level '%s'. Use none|error|warn|info|debug|verbose or 0-5.\n", argv[2]);
            return 1;
        }
        esp_log_level_set(argv[1], level);
        printf("%s: %s\n", argv[1], level_name(level));
        return 0;
    }
    printf("Usage: log <tag> [level]   (tag \"*\" affects every tag without its own level)\n");
    return 1;
}

} // namespace

inline void register_log() {
    const esp_console_cmd_t cmd = {
        .command = "log",
        .help = "Show or set a tag's log level (none|error|warn|info|debug|verbose or 0-5)",
        .hint = "<tag> [level]",
        .func = &cli_cmd_log,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    register_command(cmd);
}

} // namespace Feature::CLI::Commands
