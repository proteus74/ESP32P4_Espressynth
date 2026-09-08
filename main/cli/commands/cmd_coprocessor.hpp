#pragma once

// Co-processor firmware update command. Only registered when the feature itself is
// (CONFIG_APP_ENABLE_COPROCESSOR_OTA); backed by main/features/hosted/hosted_ota.hpp, which already
// runs this automatically at boot -- this command lets you re-trigger it on demand (e.g. to
// retry, or force a push after re-staging slave_fw) without rebooting first.

#include "esp_console.h"
#include "esp_err.h"
#include "features/hosted/hosted_ota.hpp"

#include <cstdio>
#include <cstring>

namespace Feature::CLI::Commands {

namespace {

inline int cli_cmd_coprocessor_update(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "howto") == 0) {
        // Dev-machine-specific ESP-IDF path (this project's original build machine); adjust
        // if yours differs. See coprocessor_fw/README.md for the OS-independent version.
        printf("C:\\esp\\v6.1-rc1\\esp-idf\\export.bat\n"
               "cd coprocessor_fw\n"
               "build_and_stage.bat COM16              # Windows\n"
               "\n"
               "./build_and_stage.sh /dev/ttyUSB0      # Linux/macOS\n");
        return 0;
    }

    bool force = false;
    if (argc == 2 && std::strcmp(argv[1], "force") == 0) {
        force = true;
    } else if (argc > 1) {
        printf("Usage: coprocessor-update [force|howto]\n");
        return 1;
    }

    // On a successful push this restarts the host and never returns; reaching the code
    // below means it was skipped (already up to date) or failed.
    const esp_err_t err = Feature::Hosted::OTA::update_coprocessor_if_needed("slave_fw", force);
    if (err != ESP_OK) {
        printf("Co-processor update failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Co-processor already up to date, nothing pushed. Use 'coprocessor-update force' to push anyway.\n");
    return 0;
}

} // namespace

inline void register_coprocessor() {
    const esp_console_cmd_t cmd = {
        .command = "coprocessor-update",
        .help = "Push the image to the ESP32-C6 co-processor now. 'howto' shows how to build/stage it ",
        .hint = "[force|howto]",
        .func = &cli_cmd_coprocessor_update,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    register_command(cmd);
}

} // namespace Feature::CLI::Commands
