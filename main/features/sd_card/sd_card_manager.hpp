#pragma once

#include "boards/board_config.hpp"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_err.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if SOC_SDMMC_IO_POWER_EXTERNAL
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif
#include "sdmmc_cmd.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// IDF uses this helper internally between card initialization and FATFS
// mounting. We call it after probing so the card geometry can be logged even
// when the card contains a raw or exFAT volume.
extern "C" esp_err_t esp_vfs_fat_mount_initialized(
    sdmmc_card_t* card, const char* base_path,
    const esp_vfs_fat_mount_config_t* mount_config);

namespace Feature::SDCard {

constexpr const char* MOUNT_POINT = "/sdcard";

namespace {
// Set once esp_vfs_fat_mount_initialized() actually succeeds below - lets callers (e.g.
// the synth engine's SoundFont loader, main/audio/engine/SoundFontPlayer.h) tell an
// absent/unformatted card apart from one that mounted fine, without re-probing the
// hardware or duplicating init()'s own mount-result handling.
inline bool s_mounted = false;
} // namespace

/** @brief True once init() has successfully mounted a FAT filesystem on the SD card. */
inline bool is_mounted() {
    return s_mounted;
}

inline esp_err_t init() {
    constexpr const char* TAG = "FeatureSDCard";

    if (CurrentBoard::PIN_SDMMC_PWR_EN != GPIO_NUM_NC) {
        esp_err_t power_err = gpio_set_direction(CurrentBoard::PIN_SDMMC_PWR_EN, GPIO_MODE_OUTPUT);
        if (power_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure SD card power enable: %s", esp_err_to_name(power_err));
            return power_err;
        }
        power_err = gpio_set_level(CurrentBoard::PIN_SDMMC_PWR_EN, 0);
        if (power_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to enable SD card power: %s", esp_err_to_name(power_err));
            return power_err;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    // SDMMC_HOST_DEFAULT() defaults to Slot 1 on ESP32-P4, but that's the flexible
    // GPIO-matrix slot ESP-Hosted's SDIO transport uses for the C6 link. The TF card
    // sits on Slot 0, whose pins are hardware-fixed to exactly PIN_SDMMC_CLK/CMD/D0-D3
    // above -- using Slot 1 here races ESP-Hosted for the same controller slot.
    host.slot = SDMMC_HOST_SLOT_0;
#if SOC_SDMMC_IO_POWER_EXTERNAL
    ESP_LOGI(TAG, "Using on-chip SD signal LDO VO%d", CurrentBoard::SDMMC_LDO_CHANNEL);
    // VO4 is the SD-card supply on the JC8012P4A1. Acquire it directly before
    // starting SDMMC, matching the board's known-working low-level sequence.
    esp_ldo_channel_config_t ldo_config = {};
    ldo_config.chan_id = CurrentBoard::SDMMC_LDO_CHANNEL;
    ldo_config.voltage_mv = 3300;
    esp_ldo_channel_handle_t ldo_handle = nullptr;
    esp_err_t ldo_err = esp_ldo_acquire_channel(&ldo_config, &ldo_handle);
    if (ldo_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure SD signal LDO VO%d: %s",
                 CurrentBoard::SDMMC_LDO_CHANNEL, esp_err_to_name(ldo_err));
        return ldo_err;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
#endif
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    // Boards that only wire D0 (CrowPanel: D1-D3 are RGB/touch pins) run the slot in 1-bit mode.
    const bool one_bit_bus = CurrentBoard::PIN_SDMMC_D1 == GPIO_NUM_NC;
    if (one_bit_bus) {
        host.flags &= ~SDMMC_HOST_FLAG_4BIT;
    }
    slot_config.width = one_bit_bus ? 1 : 4;
    slot_config.clk = CurrentBoard::PIN_SDMMC_CLK;
    slot_config.cmd = CurrentBoard::PIN_SDMMC_CMD;
    slot_config.d0 = CurrentBoard::PIN_SDMMC_D0;
    slot_config.d1 = CurrentBoard::PIN_SDMMC_D1;
    slot_config.d2 = CurrentBoard::PIN_SDMMC_D2;
    slot_config.d3 = CurrentBoard::PIN_SDMMC_D3;
    slot_config.cd = GPIO_NUM_NC;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    // Initialize the card separately from FATFS. This lets us distinguish a
    // detected card without a FAT filesystem (e.g. a raw sample library or
    // exFAT card) from a card that is electrically unreachable.
    sdmmc_card_t* card = static_cast<sdmmc_card_t*>(std::calloc(1, sizeof(sdmmc_card_t)));
    if (card == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = host.init();
    if (err != ESP_OK) {
        std::free(card);
        return err;
    }
    err = sdmmc_host_init_slot(host.slot, &slot_config);
    if (err != ESP_OK) {
        std::free(card);
        return err;
    }

    err = sdmmc_card_init(&host, card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD card communication failed: %s", esp_err_to_name(err));
        sdmmc_host_deinit_slot(host.slot);
        std::free(card);
        return err;
    }

    const uint64_t card_size_mb =
        static_cast<uint64_t>(card->csd.capacity) * card->csd.sector_size / (1024ULL * 1024ULL);
    const char* card_type = card->is_mmc ? "MMC" : (card->is_sdio ? "SDIO" : "SD");
    ESP_LOGI(TAG, "SD card detected: name=%s type=%s speed=%u MHz size=%llu MB",
             card->cid.name, card_type, card->max_freq_khz / 1000,
             static_cast<unsigned long long>(card_size_mb));

    err = esp_vfs_fat_mount_initialized(card, MOUNT_POINT, &mount_config);
    if (err != ESP_OK) {
        // Overwhelmingly the common case here is simply a blank/non-FAT card, not a
        // fault -- an INFO note is enough, not a warning (see also the "vfs_fat_sdmmc"
        // log level dialed down in App::init(), which would otherwise add its own
        // warning/error lines for the exact same non-event).
        ESP_LOGI(TAG, "SD card detected, but has no FAT filesystem (insert a FAT-formatted "
                      "card, or format it)");
        sdmmc_host_deinit_slot(host.slot);
        std::free(card);
        return err;
    }
    ESP_LOGI(TAG, "SD card FAT filesystem mounted at %s", MOUNT_POINT);
    s_mounted = true;

    const char* test_file_path = "/sdcard/hello.txt";
    FILE* file = std::fopen(test_file_path, "r");
    if (file == nullptr) {
        ESP_LOGD(TAG, "SD card read check: /hello.txt not found");
        return ESP_OK;
    }

    char buffer[128] = {};
    if (std::fgets(buffer, sizeof(buffer), file) != nullptr) {
        buffer[std::strcspn(buffer, "\r\n")] = '\0';
        ESP_LOGD(TAG, "SD card read check: /hello.txt -> %s", buffer);
    }
    std::fclose(file);
    return ESP_OK;
}

} // namespace Feature::SDCard
