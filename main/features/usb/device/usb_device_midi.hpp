#pragma once

#include <cstdint>
#include <cstddef>
#include "esp_err.h"
#include "tusb.h"

namespace Feature::USBDevice::MIDI {

struct Event {
    uint8_t cable;
    uint8_t cin;
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
    uint8_t length;
    uint8_t channel;    // 0-15 for channel voice messages, 0xff otherwise
};

using EventCallback = void (*)(const Event& event);

namespace {
inline Feature::USBDevice::MIDI::EventCallback event_callback = nullptr;

inline uint8_t message_length(uint8_t cin) {
    switch (cin) {
        case 0x05:
        case 0x0f:
            return 1;
        case 0x02:
        case 0x06:
        case 0x0c:
        case 0x0d:
            return 2;
        case 0x00:
        case 0x01:
            return 0;
        default:
            return 3;
    }
}
} // namespace

/**
 * @brief Register a callback invoked from the USB device task for every MIDI message
 *        received from the host. Pass nullptr to unregister.
 */
inline void set_callback(EventCallback callback) {
    event_callback = callback;
}

/**
 * @brief Send raw MIDI stream bytes (e.g. {0x90, 0x3C, 0x7F} for Note On) to the host.
 */
inline esp_err_t send(const uint8_t* bytes, size_t length) {
    if (bytes == nullptr || length == 0) return ESP_ERR_INVALID_ARG;
    if (!tud_midi_mounted()) return ESP_ERR_INVALID_STATE;
    const uint32_t written = tud_midi_stream_write(0, bytes, static_cast<uint32_t>(length));
    return written == length ? ESP_OK : ESP_FAIL;
}

} // namespace Feature::USBDevice::MIDI

// Polled from Feature::USBDevice::class_task() - device-side MIDI has no RX callback.
extern "C" __attribute__((used)) void usb_device_midi_poll() {
    using namespace Feature::USBDevice::MIDI;
    uint8_t packet[4];
    while (tud_midi_available() && tud_midi_packet_read(packet)) {
        if (event_callback == nullptr) continue;
        const uint8_t cin = packet[0] & 0x0f;
        const uint8_t status = packet[1];
        const bool is_channel_message = status >= 0x80 && status <= 0xef;
        const Feature::USBDevice::MIDI::Event event{
            static_cast<uint8_t>(packet[0] >> 4), cin, status, packet[2], packet[3], message_length(cin),
            is_channel_message ? static_cast<uint8_t>(status & 0x0f) : static_cast<uint8_t>(0xff)
        };
        if (event.length != 0) event_callback(event);
    }
}
