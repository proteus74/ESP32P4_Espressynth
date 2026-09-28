#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include "app_core.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/usbh.h"
#include "class/midi/midi_host.h"
#include "host/usbh_pvt.h"
#include <atomic>

namespace Feature::MIDI {

struct Event {
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
    uint8_t channel;    // 0-15, decoded from status & 0x0f
};

using EventCallback = void (*)(const Event& event);

namespace {
constexpr const char* TAG = "USBMIDI";
struct midi_packet_t { uint8_t data[4]; };
inline QueueHandle_t queue = nullptr;
inline Feature::MIDI::EventCallback event_callback = nullptr;
inline uint8_t connected_idx = 0;
inline uint8_t connected_address = 0;
inline bool connected = false;

// Outgoing messages: TinyUSB host is not thread-safe, so send() (called from httpd, the synth
// engine, ...) only queues; tx_drain() runs inside the USB host task via usbh_defer_func().
// Calling tuh_midi_stream_write() directly from other tasks raced the host stack and HCD ISR
// and wedged the whole USB host after a few web-UI knob turns.
struct tx_message_t { uint8_t cable; uint8_t length; uint8_t data[3]; };
inline QueueHandle_t tx_queue = nullptr;
inline std::atomic<bool> tx_drain_pending{false};

inline void tx_drain(void*) {
    tx_drain_pending = false;
    tx_message_t message{};
    while (xQueueReceive(tx_queue, &message, 0) == pdTRUE) {
        if (connected && tuh_midi_mounted(connected_idx)) {
            tuh_midi_stream_write(connected_idx, message.cable, message.data, message.length);
        }
    }
    if (connected && tuh_midi_mounted(connected_idx)) {
        tuh_midi_write_flush(connected_idx);
    }
}

inline void midi_task(void*) {
    midi_packet_t packet{};
    while (xQueueReceive(queue, &packet, portMAX_DELAY) == pdTRUE) {
        if (event_callback != nullptr) {
            const Feature::MIDI::Event event{
                packet.data[1], packet.data[2], packet.data[3],
                static_cast<uint8_t>(packet.data[1] & 0x0f)
            };
            event_callback(event);
        }
    }
}
} // namespace

/**
 * @brief Register a callback invoked from the MIDI task for every received message.
 *        Pass nullptr to unregister. Only one callback is stored at a time.
 */
inline void set_callback(EventCallback callback) {
    event_callback = callback;
}

/**
 * @brief True if a USB MIDI device is currently mounted.
 */
inline bool is_connected() {
    return connected && tuh_midi_mounted(connected_idx);
}

/**
 * @brief USB device address of the currently mounted MIDI device (tuh_midi_mount_cb_t::daddr).
 *        Only meaningful while is_connected() is true - lets a status display (e.g. GET
 *        /api/midi/devices) tell devices apart without a product-string lookup.
 */
inline uint8_t connected_device_address() {
    return connected_address;
}

/**
 * @brief Send raw MIDI stream bytes (e.g. {0x90, 0x3C, 0x7F} for Note On) to the
 *        connected device on the given virtual cable. Returns ESP_ERR_INVALID_STATE
 *        if no device is connected.
 */
inline esp_err_t send(const uint8_t* bytes, size_t length, uint8_t cable = 0) {
    if (bytes == nullptr || length == 0) return ESP_ERR_INVALID_ARG;
    if (length > sizeof(tx_message_t::data)) return ESP_ERR_INVALID_SIZE;
    if (!is_connected() || tx_queue == nullptr) return ESP_ERR_INVALID_STATE;
    tx_message_t message{};
    message.cable = cable;
    message.length = static_cast<uint8_t>(length);
    std::memcpy(message.data, bytes, length);
    if (xQueueSend(tx_queue, &message, 0) != pdTRUE) return ESP_ERR_NO_MEM;
    // At most one drain request in the USB host event queue at a time, so a fast-turning
    // knob can't flood it.
    if (!tx_drain_pending.exchange(true)) {
        usbh_defer_func(tx_drain, nullptr, false);
    }
    return ESP_OK;
}

} // namespace Feature::MIDI

extern "C" __attribute__((used)) void usb_midi_host_start() {
    using namespace Feature::MIDI;
    queue = xQueueCreate(64, sizeof(midi_packet_t));
    tx_queue = xQueueCreate(64, sizeof(tx_message_t));
    // MIDI is sound data, so this relay task runs on the audio core (see app_core.hpp),
    // separate from the mixed USB Host task (HID + raw MIDI byte parsing) that feeds it.
    // 2048: measured peak usage (CLI "top") is ~1170B - each loop iteration just decodes one
    // fixed-size 4-byte USB-MIDI packet and forwards it via the event callback (dispatch_midi
    // -> onMidiEvent -> a bounded xQueueSend), no recursion/unbounded depth - leaves ~1.7x
    // headroom over the observed worst case rather than the original 3072's ~2.6x.
    xTaskCreatePinnedToCore(midi_task, "usb_midi_task", 2048, nullptr, 5, nullptr, App::AUDIO_CORE);
    ESP_LOGI(TAG, "TinyUSB MIDI host enabled");
}

extern "C" __attribute__((used)) void tuh_midi_mount_cb(uint8_t idx, const tuh_midi_mount_cb_t* data) {
    using namespace Feature::MIDI;
    ESP_LOGI(TAG, "MIDI mounted: address=%u index=%u", data->daddr, idx);
    connected_idx = idx;
    connected_address = data->daddr;
    connected = true;
}

extern "C" __attribute__((used)) void tuh_midi_umount_cb(uint8_t idx) {
    using namespace Feature::MIDI;
    ESP_LOGI(TAG, "MIDI removed: index=%u", idx);
    if (connected && idx == connected_idx) connected = false;
}

extern "C" __attribute__((used)) void tuh_midi_rx_cb(uint8_t idx, uint32_t bytes) {
    using namespace Feature::MIDI;
    uint8_t packet[4];
    while (bytes >= 4 && tuh_midi_packet_read(idx, packet)) {
        // CIN 0x0/0x1 (Reserved/Miscellaneous) packets are how many devices pad a bulk
        // transfer to a full multiple of 4 bytes - not real MIDI messages. Forwarding them
        // used to flood the engine + web log with bogus "status=0x00" events on every
        // keypress and overwhelm httpd's async send queue. Drop them here, same filter
        // UsbMidiEvent::parse() already applies to the USB-device side.
        const uint8_t cin = packet[0] & 0x0f;
        if (cin != 0x0 && cin != 0x1 && queue != nullptr) {
            midi_packet_t event{};
            std::memcpy(event.data, packet, sizeof(event.data));
            xQueueSend(queue, &event, 0);
        }
        bytes -= 4;
    }
}

extern "C" __attribute__((used)) void tuh_midi_tx_cb(uint8_t idx, uint32_t bytes) {
    (void)idx;
    (void)bytes;
}
