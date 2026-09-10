#pragma once

#include "esp_err.h"
#include "esp_littlefs.h"
#include "esp_log.h"

// Mounts the "littlefs" data partition (see ../../partitions.csv) as a LittleFS filesystem,
// via the joltwallet/littlefs component (main/idf_component.yml). Default/seed content
// (e.g. the boot splash image, see display_lvgl.hpp) is provisioned separately, offline, by
// flashing littlefs_data/ onto this same partition via `idf.py littlefs-flash` (see the root
// CMakeLists.txt's littlefs_create_partition_image(littlefs ...) call and its warning
// comment) - NOT at runtime here, and NOT embedded in the app binary at all.
namespace Feature::LittleFS {

constexpr const char* MOUNT_POINT = "/littlefs";
constexpr const char* PARTITION_LABEL = "littlefs";

inline esp_err_t init() {
    constexpr const char* TAG = "FeatureLittleFS";

    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = MOUNT_POINT;
    conf.partition_label = PARTITION_LABEL;
    conf.format_if_mount_failed = true;
    conf.dont_mount = false;

    const esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Partition '%s' not found in the partition table", PARTITION_LABEL);
        } else {
            ESP_LOGE(TAG, "Failed to mount/format LittleFS partition '%s': %s", PARTITION_LABEL,
                      esp_err_to_name(err));
        }
        return err;
    }

    size_t total_bytes = 0, used_bytes = 0;
    if (esp_littlefs_info(PARTITION_LABEL, &total_bytes, &used_bytes) == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS mounted at %s: %u KB total, %u KB used", MOUNT_POINT,
                 static_cast<unsigned>(total_bytes / 1024), static_cast<unsigned>(used_bytes / 1024));
    } else {
        ESP_LOGI(TAG, "LittleFS mounted at %s", MOUNT_POINT);
    }
    return ESP_OK;
}

} // namespace Feature::LittleFS
