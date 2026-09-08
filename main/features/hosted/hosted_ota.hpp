#pragma once

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "eh_common_fw_version.h"
#include "esp_app_format.h"
#include "esp_err.h"
#include "esp_hosted.h"
#include "esp_hosted_ota.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Pushes the ESP32-C6 co-processor firmware (see coprocessor_fw/) to the co-processor
// over the existing ESP-Hosted link, from a raw flash partition staged with that image
// (main/partitions.csv's "slave_fw" partition -- see coprocessor_fw/README.md for how to
// get the built .bin into it). No network involved: the whole transfer rides the same
// SDIO link already used for Wi-Fi/BLE.
namespace Feature::Hosted::OTA {

namespace {
constexpr const char* TAG = "HostedOTA";
constexpr size_t CHUNK_SIZE = 1500;

inline bool partition_is_empty(const esp_partition_t* partition) {
    uint8_t buffer[256];
    size_t checked = 0;
    while (checked < 1024 && checked < partition->size) {
        const size_t to_read = std::min(sizeof(buffer), static_cast<size_t>(1024 - checked));
        if (esp_partition_read(partition, checked, buffer, to_read) != ESP_OK) return true;
        for (size_t i = 0; i < to_read; ++i) {
            if (buffer[i] != 0xff) return false;
        }
        checked += to_read;
    }
    return true;
}

// Walks the ESP app-image segment headers to find the image's true total size
// (image header + segments + checksum + optional SHA256), since the partition
// itself is just a fixed-size container padded with 0xFF past the real image.
inline esp_err_t parse_image_size(const esp_partition_t* partition, size_t& firmware_size) {
    esp_image_header_t header{};
    if (esp_partition_read(partition, 0, &header, sizeof(header)) != ESP_OK) return ESP_FAIL;
    if (header.magic != ESP_IMAGE_HEADER_MAGIC) return ESP_ERR_INVALID_ARG;

    size_t offset = sizeof(header);
    size_t total = sizeof(header);
    for (int i = 0; i < header.segment_count; ++i) {
        esp_image_segment_header_t segment{};
        if (esp_partition_read(partition, offset, &segment, sizeof(segment)) != ESP_OK) return ESP_FAIL;
        total += sizeof(segment) + segment.data_len;
        offset += sizeof(segment) + segment.data_len;
    }
    total += (16 - (total % 16)) % 16; // pad to 16 bytes
    total += 1;                        // checksum byte
    if (header.hash_appended) {
        total += (16 - (total % 16)) % 16;
        total += 32; // SHA-256
    }
    firmware_size = total;
    return ESP_OK;
}

inline bool coprocessor_supports_activate(const esp_hosted_coprocessor_fwver_t& ver) {
    return ver.major1 > 2 || (ver.major1 == 2 && ver.minor1 > 5);
}
} // namespace

/**
 * @brief Push the co-processor firmware staged in `partition_label` to the ESP32-C6
 *        over the ESP-Hosted link, activate it, and restart the host to resync.
 *        No-op (returns ESP_OK) unless `force` is set if the co-processor already
 *        reports this host's own ESP-Hosted protocol version (set `force` to push
 *        anyway, e.g. after only changing coprocessor_fw's own feature config, which
 *        doesn't change that protocol version). Call after Feature::WiFi::init() or
 *        Feature::BLE::init() (either brings up the hosted transport the co-processor
 *        version is read over).
 */
inline esp_err_t update_coprocessor_if_needed(const char* partition_label = "slave_fw", bool force = false) {
    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, partition_label);
    if (partition == nullptr) {
        ESP_LOGE(TAG, "Partition '%s' not found in the partition table", partition_label);
        return ESP_ERR_NOT_FOUND;
    }
    if (partition_is_empty(partition)) {
        ESP_LOGW(TAG, "Partition '%s' is empty -- nothing staged to push. Build coprocessor_fw/ "
                 "for esp32c6 and flash esp32p4_template_coprocessor.bin into this partition "
                 "(see coprocessor_fw/README.md)", partition_label);
        return ESP_ERR_NOT_FOUND;
    }

    size_t firmware_size = 0;
    if (parse_image_size(partition, firmware_size) != ESP_OK || firmware_size == 0 ||
        firmware_size > partition->size) {
        ESP_LOGE(TAG, "Partition '%s' doesn't hold a valid firmware image (bad header, or stale data "
                 "left over from a previous partition table with '%s' at a different offset). Re-stage "
                 "it -- see coprocessor_fw/README.md for the esptool write_flash command and the "
                 "partition's current offset in ../partitions.csv", partition_label, partition_label);
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "Co-processor image staged: %u bytes", static_cast<unsigned>(firmware_size));

    // Compare against *this host's own* compiled-in ESP-Hosted protocol version (not the
    // staged image's app version, which is an unrelated build identifier): coprocessor_fw/
    // pins the exact same espressif/esp_hosted release as this project, so if the running
    // co-processor already reports that same protocol version there's nothing to push.
    esp_hosted_coprocessor_fwver_t current_ver{};
    const bool have_current = esp_hosted_get_coprocessor_fwversion(&current_ver) == ESP_OK;
    if (have_current) {
        ESP_LOGI(TAG, "Co-processor currently running: %" PRIu32 ".%" PRIu32 ".%" PRIu32,
                 current_ver.major1, current_ver.minor1, current_ver.patch1);
        if (!force && current_ver.major1 == PROJECT_VERSION_MAJOR_1 &&
            current_ver.minor1 == PROJECT_VERSION_MINOR_1 &&
            current_ver.patch1 == PROJECT_VERSION_PATCH_1) {
            ESP_LOGI(TAG, "Co-processor already on ESP-Hosted v%d.%d.%d, skipping OTA",
                     PROJECT_VERSION_MAJOR_1, PROJECT_VERSION_MINOR_1, PROJECT_VERSION_PATCH_1);
            return ESP_OK;
        }
    } else {
        ESP_LOGW(TAG, "Could not read current co-processor version, pushing update anyway");
    }

    if (esp_hosted_slave_ota_begin() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to begin co-processor OTA");
        return ESP_FAIL;
    }

    uint8_t chunk[CHUNK_SIZE];
    size_t offset = 0;
    while (offset < firmware_size) {
        const size_t to_read = std::min(CHUNK_SIZE, firmware_size - offset);
        if (esp_partition_read(partition, offset, chunk, to_read) != ESP_OK ||
            esp_hosted_slave_ota_write(chunk, to_read) != ESP_OK) {
            ESP_LOGE(TAG, "Co-processor OTA transfer failed at offset %u", static_cast<unsigned>(offset));
            esp_hosted_slave_ota_end();
            return ESP_FAIL;
        }
        offset += to_read;
    }

    if (esp_hosted_slave_ota_end() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to finalize co-processor OTA");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Co-processor OTA transfer complete (%u bytes)", static_cast<unsigned>(firmware_size));

    // esp_hosted_slave_ota_activate() only exists on co-processor firmware v2.6.0+;
    // re-read the version (now stale/pre-OTA, but still what's actually running until
    // activation) to decide whether it's safe to call.
    if (have_current && coprocessor_supports_activate(current_ver)) {
        if (esp_hosted_slave_ota_activate() == ESP_OK) {
            ESP_LOGI(TAG, "Co-processor firmware activated, it will reboot");
        } else {
            ESP_LOGW(TAG, "Failed to activate new co-processor firmware");
        }
    } else {
        ESP_LOGI(TAG, "Co-processor firmware too old for the activate RPC; power-cycle the board "
                 "to boot the new image");
    }

    ESP_LOGW(TAG, "Restarting host to resync with the co-processor...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

} // namespace Feature::Hosted::OTA
