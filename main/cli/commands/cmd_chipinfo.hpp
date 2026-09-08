#pragma once

#include "esp_chip_info.h"
#include "esp_console.h"
#include "esp_flash.h"

#include <cstdio>

namespace Feature::CLI::Commands {

namespace {

inline int cli_cmd_chip_info(int argc, char** argv) {
    (void)argc;
    (void)argv;

    esp_chip_info_t info{};
    esp_chip_info(&info);

    const char* model = "unknown";
    switch (info.model) {
        case CHIP_ESP32:   model = "ESP32";   break;
        case CHIP_ESP32S2: model = "ESP32-S2"; break;
        case CHIP_ESP32S3: model = "ESP32-S3"; break;
        case CHIP_ESP32C3: model = "ESP32-C3"; break;
        case CHIP_ESP32C2: model = "ESP32-C2"; break;
        case CHIP_ESP32C6: model = "ESP32-C6"; break;
        case CHIP_ESP32H2: model = "ESP32-H2"; break;
        case CHIP_ESP32P4: model = "ESP32-P4"; break;
        default: break;
    }

    printf("Model:    %s\n", model);
    printf("Revision: v%u.%u\n", info.revision / 100, info.revision % 100);
    printf("Cores:    %u\n", info.cores);
    printf("Features: %s%s%s%s%s\n",
           (info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded-flash " : "",
           (info.features & CHIP_FEATURE_EMB_PSRAM) ? "embedded-psram " : "",
           (info.features & CHIP_FEATURE_WIFI_BGN) ? "wifi " : "",
           (info.features & CHIP_FEATURE_BLE) ? "ble " : "",
           (info.features & CHIP_FEATURE_BT) ? "bt-classic " : "");

    uint32_t flash_size = 0;
    if (esp_flash_get_size(nullptr, &flash_size) == ESP_OK) {
        printf("Flash:    %u MB\n", static_cast<unsigned>(flash_size / (1024 * 1024)));
    }
    return 0;
}

} // namespace

inline void register_chip_info() {
    const esp_console_cmd_t cmd = {
        .command = "chip-info",
        .help = "Show chip model, revision, cores, features, flash size",
        .hint = nullptr,
        .func = &cli_cmd_chip_info,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    register_command(cmd);
}

} // namespace Feature::CLI::Commands
