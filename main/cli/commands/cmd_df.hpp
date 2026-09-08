#pragma once

// "df" command: shows LittleFS space usage, Linux-df-style, for every mounted LittleFS
// partition -- the general-purpose "littlefs" data partition (CONFIG_APP_ENABLE_LITTLEFS,
// main/features/filesystem/littlefs_manager.hpp) and the web server's "www" assets
// partition (CONFIG_APP_ENABLE_WEBSERVER, main/features/web/web_server.hpp), whichever
// of those are enabled.

#include "esp_console.h"
#include "esp_littlefs.h"

#if defined(CONFIG_APP_ENABLE_LITTLEFS)
    #include "features/filesystem/littlefs_manager.hpp"
#endif

#if defined(CONFIG_APP_ENABLE_WEBSERVER)
    #include "features/web/web_server.hpp"
#endif

#include <cstdio>

namespace Feature::CLI::Commands {

namespace {

// Formats bytes as e.g. "10.9M", "512K", "37B" (like `df -h`).
inline void format_size(size_t bytes, char* out, size_t out_size) {
    static const char* const units[] = {"B", "K", "M", "G"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0) {
        snprintf(out, out_size, "%.0f%s", value, units[unit]);
    } else {
        snprintf(out, out_size, "%.1f%s", value, units[unit]);
    }
}

inline void print_row(const char* label, const char* mount_point) {
    size_t total_bytes = 0, used_bytes = 0;
    const esp_err_t err = esp_littlefs_info(label, &total_bytes, &used_bytes);
    if (err != ESP_OK) {
        printf("%-14s  not mounted: %s\n", label, esp_err_to_name(err));
        return;
    }

    const size_t avail_bytes = total_bytes > used_bytes ? total_bytes - used_bytes : 0;
    const int use_percent = total_bytes > 0 ? static_cast<int>((used_bytes * 100) / total_bytes) : 0;

    char size_str[16], used_str[16], avail_str[16];
    format_size(total_bytes, size_str, sizeof(size_str));
    format_size(used_bytes, used_str, sizeof(used_str));
    format_size(avail_bytes, avail_str, sizeof(avail_str));

    printf("%-14s  %4s  %4s  %4s  %3d%% %s\n", label, size_str, used_str, avail_str,
           use_percent, mount_point);
}

inline int cli_cmd_df(int argc, char** argv) {
    (void)argc;
    (void)argv;

    printf("Filesystem      Size  Used Avail Use%% Mounted on\n");
#if defined(CONFIG_APP_ENABLE_LITTLEFS)
    print_row(Feature::LittleFS::PARTITION_LABEL, Feature::LittleFS::MOUNT_POINT);
#endif
#if defined(CONFIG_APP_ENABLE_WEBSERVER)
    print_row(Feature::Web::PARTITION_LABEL, Feature::Web::MOUNT_POINT);
#endif
    return 0;
}

} // namespace

inline void register_df() {
    const esp_console_cmd_t cmd = {
        .command = "df",
        .help = "Show LittleFS space usage per mounted partition (size, used, available)",
        .hint = nullptr,
        .func = &cli_cmd_df,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    register_command(cmd);
}

} // namespace Feature::CLI::Commands
