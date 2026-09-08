#pragma once

#include "app_core.hpp"
#include "boards/board_config.hpp"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tusb.h"
#include "host/usbh.h"
#include "host/hub.h"

#if CONFIG_IDF_TARGET_ESP32P4
#include "soc/usb_periph.h"
#endif

#if CONFIG_USB_HOST_CLASS_MIDI
#include "usb_midi_host.hpp"
#endif

#if CONFIG_USB_HOST_CLASS_HID
#include "usb_hid_keyboard.hpp"
#endif

namespace Feature::USBHost {

inline void task(void* arg) {
    constexpr const char* TAG = "FeatureUSBHost";
    ESP_LOGI(TAG, "TinyUSB Host event task started");
    while (true) {
        tuh_task_ext(250, false);
        hub_status_xfer_watchdog();
    }
}

inline esp_err_t init() {
    constexpr const char* TAG = "FeatureUSBHost";
    bool hub_reset = false;

    // 1. Manage On-board USB Hub if enabled
#if defined(CONFIG_APP_ENABLE_USB_HUB_4PORT)
    if constexpr (CurrentBoard::HAS_4PORT_USB_HUB) {
        const gpio_num_t usb_hub_reset_pin = CurrentBoard::PIN_USB_HUB_RST;
        if (usb_hub_reset_pin != GPIO_NUM_NC) {
            ESP_ERROR_CHECK(gpio_set_direction(usb_hub_reset_pin, GPIO_MODE_OUTPUT));
            ESP_ERROR_CHECK(gpio_set_pull_mode(usb_hub_reset_pin, GPIO_PULLUP_ONLY));
            ESP_ERROR_CHECK(gpio_set_level(usb_hub_reset_pin, 1));
            hub_reset = true;
        }
    } else {
        ESP_LOGW(TAG, "4-Port USB Hub is enabled in config, but not present on board: %s", CurrentBoard::BOARD_NAME);
    }
#endif

    // 2. Select configured Host Class driver
#if CONFIG_IDF_TARGET_ESP32P4
#if CONFIG_USB_HOST_CLASS_MIDI
    usb_midi_host_start();
#endif
#if CONFIG_USB_HOST_CLASS_HID
    usb_hid_keyboard_start();
#endif

    usb_phy_config_t phy_config = {};
    phy_config.controller = USB_PHY_CTRL_OTG;
    phy_config.target = USB_PHY_TARGET_UTMI;
    phy_config.otg_mode = USB_OTG_MODE_HOST;
    phy_config.otg_speed = USB_PHY_SPEED_UNDEFINED;
    usb_phy_handle_t phy_handle = nullptr;
    const esp_err_t phy_result = usb_new_phy(&phy_config, &phy_handle);
    if (phy_result != ESP_OK) {
        ESP_LOGE(TAG, "USB PHY init failed: %s", esp_err_to_name(phy_result));
        return phy_result;
    }

    const tusb_rhport_init_t rhport_init = {
        .role = TUSB_ROLE_HOST,
        .speed = TUSB_SPEED_HIGH,
    };
    if (!tuh_rhport_init(1, &rhport_init)) {
        ESP_LOGE(TAG, "TinyUSB root port init failed");
        return ESP_FAIL;
    }
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif

    // Keyboard/mouse HID polling lives here too, so this stays on the main core rather
    // than the audio core (see app_core.hpp) -- input responsiveness wins that trade-off.
    xTaskCreatePinnedToCore(task, "usb_host_task", 4096, nullptr, 8, nullptr, App::MAIN_CORE);

    const char* classes =
#if CONFIG_USB_HOST_CLASS_MIDI && CONFIG_USB_HOST_CLASS_HID
        "MIDI,HID";
#elif CONFIG_USB_HOST_CLASS_MIDI
        "MIDI";
#elif CONFIG_USB_HOST_CLASS_HID
        "HID";
#else
        "none";
#endif
    ESP_LOGI(TAG, "USB Host initialized: hub_reset=%s classes=[%s]", hub_reset ? "yes" : "n/a", classes);
    return ESP_OK;
}

} // namespace Feature::USBHost

extern "C" __attribute__((used)) void tuh_mount_cb(uint8_t address) {
    uint16_t vendor_id = 0;
    uint16_t product_id = 0;
    tuh_vid_pid_get(address, &vendor_id, &product_id);
    ESP_LOGI("TinyUSB", "Device mounted: address=%u VID=0x%04x PID=0x%04x",
             address, vendor_id, product_id);
}

extern "C" __attribute__((used)) void tuh_umount_cb(uint8_t address) {
    ESP_LOGI("TinyUSB", "Device removed: address=%u", address);
}
