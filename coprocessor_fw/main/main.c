/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP32-C6 co-processor firmware for the ESP32P4_Template's ESP-Hosted link.
 * Wi-Fi RPC handling, the BT HCI byte-pipe, and the OTA/system feature are all
 * auto-initialized by the esp_hosted component itself (see sdkconfig.defaults);
 * this file only needs to bring up NVS and the default event loop first.
 */

#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"

static const char *TAG = "coprocessor_fw";

void app_main(void) {
    ESP_LOGI(TAG, "ESP32-C6 ESP-Hosted co-processor starting...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_event_loop_create_default());
}
