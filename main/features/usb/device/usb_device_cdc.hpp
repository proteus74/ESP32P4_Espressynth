#pragma once

#include <cstdint>
#include <cstddef>
#include "tusb.h"
#include "esp_log.h"

namespace Feature::USBDevice::CDC {

struct Event {
    const uint8_t* data;   // Valid only for the duration of the callback
    size_t length;
};

using EventCallback = void (*)(const Event& event);

namespace {
constexpr const char* TAG = "USBDevCDC";
inline Feature::USBDevice::CDC::EventCallback event_callback = nullptr;
} // namespace

/**
 * @brief Register a callback invoked from the USB device task for every chunk of data
 *        received from the host. Pass nullptr to unregister.
 */
inline void set_callback(EventCallback callback) {
    event_callback = callback;
}

/**
 * @brief Send bytes to the host over the CDC-ACM data endpoint.
 */
inline size_t send(const uint8_t* data, size_t length) {
    if (!tud_cdc_connected()) return 0;
    const uint32_t written = tud_cdc_write(data, length);
    tud_cdc_write_flush();
    return written;
}

} // namespace Feature::USBDevice::CDC

extern "C" __attribute__((used)) void tud_cdc_rx_cb(uint8_t itf) {
    (void)itf;
    using namespace Feature::USBDevice::CDC;
    uint8_t buffer[64];
    while (tud_cdc_available()) {
        const uint32_t read = tud_cdc_read(buffer, sizeof(buffer));
        if (read == 0) break;
        if (event_callback != nullptr) {
            const Feature::USBDevice::CDC::Event event{buffer, read};
            event_callback(event);
        }
    }
}

extern "C" __attribute__((used)) void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
    (void)itf;
    ESP_LOGI("USBDevCDC", "Line state changed: DTR=%d RTS=%d", dtr, rts);
}
