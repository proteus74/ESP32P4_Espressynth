#pragma once

#include "esp_app_desc.h"
#include "esp_console.h"

#include <cstdio>

namespace Feature::CLI::Commands {

namespace {

inline int cli_cmd_version(int argc, char** argv) {
    (void)argc;
    (void)argv;

    const esp_app_desc_t* desc = esp_app_get_description();
    printf("%s %s\n", desc->project_name, desc->version);
    printf("Built %s %s\n", desc->date, desc->time);
    printf("ESP-IDF %s\n", desc->idf_ver);
    return 0;
}

} // namespace

inline void register_version() {
    const esp_console_cmd_t cmd = {
        .command = "version",
        .help = "Show firmware version and build info",
        .hint = nullptr,
        .func = &cli_cmd_version,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    register_command(cmd);
}

} // namespace Feature::CLI::Commands
