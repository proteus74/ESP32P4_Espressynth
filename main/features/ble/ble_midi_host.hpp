#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <atomic>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "ble_profile.hpp"

// BLE-MIDI ("MIDI over Bluetooth Low Energy", Apple's widely-adopted spec) central: connects
// to wireless MIDI keyboards/controllers that expose the standard BLE-MIDI GATT service.
namespace Feature::BLE::MIDI {
inline std::atomic<uint32_t> connection_generation{0};
inline uint32_t connection_sequence() { return connection_generation.load(std::memory_order_acquire); }

struct Event {
    uint16_t conn_handle;
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
    uint8_t channel;    // 0-15, decoded from status & 0x0f
};

using EventCallback = void (*)(const Event& event);

} // namespace Feature::BLE::MIDI

namespace Feature::BLE {

namespace {
constexpr const char* MIDI_TAG = "BLEMIDI";

// BLE-MIDI service/characteristic 128-bit UUIDs (fixed by the spec). Named variables rather
// than inline BLE_UUID128_DECLARE(): that macro takes the address of a compound literal, which
// is an rvalue in C++ (unlike C) and can't have its address taken directly.
// 03B80E5A-EDE8-4B33-A751-6CE34EC4C700
const ble_uuid128_t MIDI_SVC_UUID_T = BLE_UUID128_INIT(
    0x00, 0xc7, 0xc4, 0x4e, 0xe3, 0x6c, 0x51, 0xa7, 0x33, 0x4b, 0xe8, 0xed, 0x5a, 0x0e, 0xb8, 0x03);
// 7772E5DB-3868-4112-A1A9-F2669D106BF3
const ble_uuid128_t MIDI_CHR_IO_UUID_T = BLE_UUID128_INIT(
    0xf3, 0x6b, 0x10, 0x9d, 0x66, 0xf2, 0xa9, 0xa1, 0x12, 0x41, 0x68, 0x38, 0xdb, 0xe5, 0x72, 0x77);
const ble_uuid16_t CCCD_UUID_T = BLE_UUID16_INIT(BLE_GATT_DSC_CLT_CFG_UUID16);

constexpr int MAX_MIDI_CONNECTIONS = 4;

struct midi_connection_t {
    uint16_t conn_handle = 0xffff; // 0xffff = free slot
    uint16_t chr_handle = 0;
};
inline midi_connection_t midi_connections[MAX_MIDI_CONNECTIONS]{};
inline Feature::BLE::MIDI::EventCallback midi_callback = nullptr;

inline midi_connection_t* midi_find(uint16_t conn_handle) {
    for (auto& c : midi_connections) if (c.conn_handle == conn_handle) return &c;
    return nullptr;
}

inline midi_connection_t* midi_find_or_alloc(uint16_t conn_handle) {
    midi_connection_t* existing = midi_find(conn_handle);
    if (existing != nullptr) return existing;
    for (auto& c : midi_connections) {
        if (c.conn_handle == 0xffff) {
            c = midi_connection_t{};
            c.conn_handle = conn_handle;
            return &c;
        }
    }
    return nullptr;
}

inline void midi_forget(uint16_t conn_handle) {
    midi_connection_t* c = midi_find(conn_handle);
    if (c != nullptr) *c = midi_connection_t{};
}

inline bool midi_advertises_interest(const struct ble_hs_adv_fields& fields) {
    for (int i = 0; i < fields.num_uuids128; ++i) {
        if (ble_uuid_cmp(&fields.uuids128[i].u, &MIDI_SVC_UUID_T.u) == 0) return true;
    }
    return false;
}

inline void midi_on_peer_ready(const struct peer* p) {
    const struct peer_chr* chr = peer_chr_find_uuid(p, &MIDI_SVC_UUID_T.u, &MIDI_CHR_IO_UUID_T.u);
    if (chr == nullptr) return; // not a BLE-MIDI device

    midi_connection_t* conn = midi_find_or_alloc(p->conn_handle);
    if (conn == nullptr) {
        ESP_LOGW(MIDI_TAG, "Too many concurrent BLE-MIDI connections tracked");
        return;
    }
    conn->chr_handle = chr->chr.val_handle;

    const struct peer_dsc* cccd = peer_dsc_find_uuid(p, &MIDI_SVC_UUID_T.u, &MIDI_CHR_IO_UUID_T.u,
                                                      &CCCD_UUID_T.u);
    if (cccd == nullptr) {
        ESP_LOGW(MIDI_TAG, "BLE-MIDI characteristic has no CCCD, cannot subscribe; conn_handle=%u", p->conn_handle);
        return;
    }
    uint8_t value[2] = {1, 0};
    const int rc = ble_gattc_write_flat(p->conn_handle, cccd->dsc.handle, value, sizeof(value), nullptr, nullptr);
    if (rc != 0) {
        ESP_LOGW(MIDI_TAG, "Failed to subscribe to BLE-MIDI notifications; rc=%d", rc);
        return;
    }
    ESP_LOGI(MIDI_TAG, "BLE-MIDI device connected and subscribed; conn_handle=%u", p->conn_handle);
    Feature::BLE::MIDI::connection_generation.fetch_add(1, std::memory_order_release);
}

inline void midi_on_disconnected(uint16_t conn_handle) {
    midi_forget(conn_handle);
}

// Number of data bytes that follow a MIDI status byte (0 for System Real-Time, SysEx handled
// separately). Only channel voice messages and the few common system messages are decoded.
inline uint8_t midi_data_length(uint8_t status) {
    const uint8_t type = status & 0xf0;
    if (type >= 0x80 && type <= 0xb0) return 2; // Note Off/On, Poly Pressure, Control Change
    if (type == 0xc0 || type == 0xd0) return 1; // Program Change, Channel Pressure
    if (type == 0xe0) return 2;                 // Pitch Bend
    if (status == 0xf2) return 2;                // Song Position Pointer
    if (status == 0xf1 || status == 0xf3) return 1; // MTC Quarter Frame / Song Select
    return 0;
}

inline void emit(uint16_t conn_handle, uint8_t status, uint8_t data1, uint8_t data2) {
    if (midi_callback == nullptr) return;
    const Feature::BLE::MIDI::Event event{
        conn_handle, status, data1, data2, static_cast<uint8_t>(status & 0x0f)
    };
    midi_callback(event);
}

// Decodes a single BLE-MIDI packet (header byte + one or more timestamped MIDI events,
// per the BLE-MIDI spec's running-status rules). Multi-packet SysEx is skipped, not decoded.
inline void decode_packet(uint16_t conn_handle, const uint8_t* data, uint16_t length) {
    if (length < 2) return;
    size_t i = 1; // skip header byte (top bit set + 6-bit timestampHigh)
    uint8_t running_status = 0;

    while (i < length) {
        if ((data[i] & 0x80) == 0) {
            ++i; // not a timestamp byte where one was expected; resync
            continue;
        }
        ++i; // consume the timestamp byte itself
        if (i >= length) break;

        uint8_t status = data[i];
        if ((status & 0x80) != 0) {
            // Explicit status byte follows the timestamp.
            ++i;
            if (status == 0xf0) { // SysEx start: not decoded, skip to the terminator.
                while (i < length && data[i] != 0xf7) ++i;
                if (i < length) ++i;
                running_status = 0;
                continue;
            }
            if (status >= 0xf8) { // System Real-Time: single byte, doesn't change running status.
                emit(conn_handle, status, 0, 0);
                continue;
            }
            running_status = status;
        } else {
            // No status byte: running-status continuation. `data[i]` is the first data byte.
            if (running_status == 0) { ++i; continue; } // nothing to run against; resync
            status = running_status;
        }

        const uint8_t data_len = midi_data_length(status);
        uint8_t d1 = 0, d2 = 0;
        if (data_len >= 1 && i < length) d1 = data[i++];
        if (data_len >= 2 && i < length) d2 = data[i++];
        emit(conn_handle, status, d1, d2);
    }
}

inline void midi_on_notify(uint16_t conn_handle, uint16_t attr_handle, const uint8_t* data, uint16_t length) {
    const midi_connection_t* conn = midi_find(conn_handle);
    if (conn == nullptr || attr_handle != conn->chr_handle) return;
    decode_packet(conn_handle, data, length);
}
} // namespace

} // namespace Feature::BLE

namespace Feature::BLE::MIDI {

/**
 * @brief Register a callback invoked for every MIDI message received from a connected
 *        BLE-MIDI device. Pass nullptr to unregister. Only one callback is stored at a time.
 */
inline void set_callback(EventCallback callback) {
    Feature::BLE::midi_callback = callback;
}

/**
 * @brief True if a BLE-MIDI device is subscribed on the given connection.
 */
inline bool is_connected(uint16_t conn_handle) {
    const auto* conn = Feature::BLE::midi_find(conn_handle);
    return conn != nullptr && conn->chr_handle != 0;
}

/**
 * @brief Number of currently subscribed BLE-MIDI peers (0..MAX_MIDI_CONNECTIONS). Lets a
 *        status display (e.g. GET /api/midi/devices) show whether any BLE-MIDI peer is
 *        connected without the caller needing to already know a conn_handle.
 */
inline uint8_t connected_count() {
    uint8_t count = 0;
    for (const auto& c : Feature::BLE::midi_connections) {
        if (c.conn_handle != 0xffff && c.chr_handle != 0) ++count;
    }
    return count;
}

/**
 * @brief Send a MIDI channel message (e.g. {0x90, 0x3C, 0x7F} for Note On) to the BLE-MIDI
 *        device on the given connection. Returns ESP_ERR_INVALID_STATE if not connected/
 *        subscribed on that connection.
 */
inline esp_err_t send(uint16_t conn_handle, const uint8_t* bytes, size_t length) {
    if (bytes == nullptr || length == 0 || length > 3) return ESP_ERR_INVALID_ARG;
    const auto* conn = Feature::BLE::midi_find(conn_handle);
    if (conn == nullptr || conn->chr_handle == 0) return ESP_ERR_INVALID_STATE;

    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000) & 0x1fff; // 13-bit ms
    uint8_t packet[2 + 3];
    packet[0] = static_cast<uint8_t>(0x80 | ((now_ms >> 7) & 0x3f));
    packet[1] = static_cast<uint8_t>(0x80 | (now_ms & 0x7f));
    std::memcpy(&packet[2], bytes, length);

    const int rc = ble_gattc_write_no_rsp_flat(conn_handle, conn->chr_handle, packet,
                                                static_cast<uint16_t>(2 + length));
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

} // namespace Feature::BLE::MIDI

extern "C" __attribute__((used)) void ble_midi_host_start() {
    using namespace Feature::BLE;
    ProfileHooks hooks{};
    hooks.name = "MIDI";
    hooks.advertises_interest = midi_advertises_interest;
    hooks.on_peer_ready = midi_on_peer_ready;
    hooks.on_disconnected = midi_on_disconnected;
    hooks.on_notify = midi_on_notify;
    register_profile(hooks);
    ESP_LOGI(MIDI_TAG, "BLE-MIDI host enabled (wireless MIDI keyboards / controllers)");
}
