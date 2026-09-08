#pragma once

#include "app_core.hpp"
#include "boards/board_config.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_private/usb_phy.h"
#include "tusb.h"
#include <cstdio>

#if CONFIG_IDF_TARGET_ESP32P4
#include "hal/usb_wrap_ll.h"
#include "soc/usb_wrap_struct.h"
#endif

#if defined(CONFIG_APP_ENABLE_USB_DEVICE)
#include "usb_device_descriptors.hpp"
#endif

#if defined(CONFIG_USB_DEV_ENABLE_CDC)
#include "usb_device_cdc.hpp"
#endif

#if defined(CONFIG_USB_DEV_ENABLE_MIDI)
#include "usb_device_midi.hpp"
#endif

#if defined(CONFIG_USB_DEV_ENABLE_UAC2)
#include "usb_device_audio.hpp"
#endif

namespace Feature::USBDevice {

inline void task(void* arg) {
    (void)arg;
    constexpr const char* TAG = "FeatureUSBDevice";
    ESP_LOGI(TAG, "TinyUSB Device background task started");
    while (true) {
        tud_task();
    }
}

#if CONFIG_USB_DEV_ENABLE_MIDI || CONFIG_USB_DEV_ENABLE_UAC2
inline void class_task(void* arg) {
    (void)arg;
    constexpr const char* TAG = "FeatureUSBDevice";
    ESP_LOGI(TAG, "TinyUSB Device class polling task started");
    while (true) {
#if CONFIG_USB_DEV_ENABLE_MIDI
        usb_device_midi_poll();
#endif
#if CONFIG_USB_DEV_ENABLE_UAC2
        usb_device_audio_poll();
#endif
        vTaskDelay(1);
    }
}
#endif

inline esp_err_t init() {
    constexpr const char* TAG = "FeatureUSBDevice";

#if CONFIG_IDF_TARGET_ESP32P4
    // rhport0: the SoC's FS-only OTG1.1 controller (separate from rhport1's HS OTG used for
    // Host mode, and from the external USB-UART bridge used for flashing/console).
    usb_phy_config_t phy_config = {};
    phy_config.controller = USB_PHY_CTRL_OTG;
    phy_config.target = USB_PHY_TARGET_INT;
    phy_config.otg_mode = USB_OTG_MODE_DEVICE;
    phy_config.otg_speed = USB_PHY_SPEED_FULL;
    usb_phy_handle_t phy_handle = nullptr;
    const esp_err_t phy_result = usb_new_phy(&phy_config, &phy_handle);
    if (phy_result != ESP_OK) {
        ESP_LOGE(TAG, "USB Device PHY init failed: %s", esp_err_to_name(phy_result));
        return phy_result;
    }

    // P4 defaults USB-Serial/JTAG to FS PHY0 (the board's Type-C USB connector) and
    // OTG1.1 to PHY1. Route OTG1.1 to PHY0 so this device stack owns that connector.
    usb_wrap_ll_phy_select(&USB_WRAP, 0);

    const tusb_rhport_init_t rhport_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_FULL,
    };
    if (!tud_rhport_init(0, &rhport_init)) {
        ESP_LOGE(TAG, "TinyUSB device port init failed");
        return ESP_FAIL;
    }

    // The bootloader briefly runs the stock USB-Serial-JTAG identity on these same pins on
    // every boot before our app ever touches the PHY. That never triggers a real electrical
    // disconnect (no VBUS/pull-up drop) our new descriptor can announce itself with, so a host
    // that already latched onto the bootloader's session has no reason to re-enumerate. Force
    // a soft disconnect/reconnect cycle so the host sees a fresh connect event for us instead.
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
    tud_connect();
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif

    // Pinned to the audio core: when UAC2 is enabled, speaker/mic sample I/O runs
    // directly inside these tasks' TinyUSB callbacks (see app_core.hpp).
    xTaskCreatePinnedToCore(task, "usb_device_task", 4096, nullptr, 5, nullptr, App::AUDIO_CORE);
#if CONFIG_USB_DEV_ENABLE_MIDI || CONFIG_USB_DEV_ENABLE_UAC2
    xTaskCreatePinnedToCore(class_task, "usb_device_class", 3072, nullptr, 4, nullptr,
                            App::AUDIO_CORE);
#endif

#if defined(CONFIG_APP_ENABLE_USB_DEVICE)
    char ifaces[96] = {};
    size_t ifaces_len = 0;
    auto append_iface = [&](const char* name) {
        ifaces_len += static_cast<size_t>(snprintf(ifaces + ifaces_len, sizeof(ifaces) - ifaces_len,
                                                     "%s%s", ifaces_len ? "," : "", name));
    };
#if defined(CONFIG_USB_DEV_ENABLE_CDC)
    append_iface("CDC");
#endif
#if defined(CONFIG_USB_DEV_ENABLE_MIDI)
    append_iface("MIDI");
#endif
#if defined(CONFIG_USB_DEV_ENABLE_UAC2)
    append_iface("UAC2");
#endif
#if defined(CONFIG_USB_DEV_ENABLE_HID_KEYBOARD)
    append_iface("HID-Kbd");
#endif
#if defined(CONFIG_USB_DEV_ENABLE_HID_MOUSE)
    append_iface("HID-Mouse");
#endif
#if defined(CONFIG_USB_DEV_ENABLE_HID_GAMEPAD)
    append_iface("HID-Gamepad");
#endif
#if defined(CONFIG_USB_DEV_ENABLE_MSC)
    append_iface("MSC");
#endif
#if defined(CONFIG_USB_DEV_ENABLE_DFU)
    append_iface("DFU");
#endif
    ESP_LOGI(TAG, "USB Device initialized: %s VID=0x%04X PID=0x%04X ifaces=[%s]",
             CONFIG_USB_DEV_MANUFACTURER, CONFIG_USB_DEV_VID, CONFIG_USB_DEV_PID, ifaces);
#else
    ESP_LOGI(TAG, "USB Device initialized");
#endif
    return ESP_OK;
}

} // namespace Feature::USBDevice

extern "C" __attribute__((used)) void tud_mount_cb(void) {
    ESP_LOGI("FeatureUSBDevice", "USB Device: mounted (enumerated by host)");
}

extern "C" __attribute__((used)) void tud_umount_cb(void) {
    ESP_LOGI("FeatureUSBDevice", "USB Device: unmounted");
}

extern "C" __attribute__((used)) void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    ESP_LOGI("FeatureUSBDevice", "USB Device: suspended");
}

extern "C" __attribute__((used)) void tud_resume_cb(void) {
    ESP_LOGI("FeatureUSBDevice", "USB Device: resumed");
}
