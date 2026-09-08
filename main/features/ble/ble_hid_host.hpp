#pragma once

#include <cstdint>
#include <cstring>
#include "esp_log.h"
#include "host/ble_hs.h"
#include "ble_profile.hpp"

// BLE HID-over-GATT (HOGP) central: connects to BLE keyboards and other HID peripherals,
// decoding the standard Boot Keyboard report for reliable cross-device key events while
// passing through raw reports from any other notifying HID Report characteristic.
namespace Feature::BLE::Keyboard {

struct Event {
    uint16_t conn_handle;
    uint8_t keycode;
    char ascii;         // Printable character, '\0' if none (e.g. modifiers)
    bool shift;
    bool pressed;        // true = key down, false = key up (only modifiers report release)
    bool is_modifier;    // true if keycode is Ctrl/Shift/Alt/GUI (0xE0-0xE7)
};

using EventCallback = void (*)(const Event& event);

} // namespace Feature::BLE::Keyboard

// BLE Boot Mouse Input Report (0x2A33) decoding: the same 3-4 byte relative-motion
// layout (buttons, dx, dy, [wheel]) as a USB boot mouse, see Feature::Mouse.
namespace Feature::BLE::Mouse {

struct Event {
    uint16_t conn_handle;
    uint8_t buttons;    // bit0=left, bit1=right, bit2=middle
    int8_t dx;
    int8_t dy;
    int8_t wheel;
};

using EventCallback = void (*)(const Event& event);
using ConnectionCallback = void (*)(bool connected);

} // namespace Feature::BLE::Mouse

namespace Feature::BLE::HID {

// Raw passthrough for HID Report characteristics that aren't the Boot Keyboard report
// (generic HID devices: gamepads, custom peripherals, keyboards without Boot Protocol, ...).
struct Event {
    uint16_t conn_handle;
    uint16_t length;
    uint8_t data[20];
};

using EventCallback = void (*)(const Event& event);

} // namespace Feature::BLE::HID

namespace Feature::BLE {

namespace {
constexpr const char* HID_TAG = "BLEHID";

// Bluetooth SIG-assigned 16-bit UUIDs for the HID Service (0x1812) and its characteristics.
constexpr uint16_t HID_SVC_UUID               = 0x1812;
constexpr uint16_t HID_CHR_PROTOCOL_MODE_UUID = 0x2A4E;
constexpr uint16_t HID_CHR_BOOT_KEYBOARD_UUID = 0x2A22;
constexpr uint16_t HID_CHR_BOOT_MOUSE_UUID    = 0x2A33;
constexpr uint16_t HID_CHR_REPORT_UUID        = 0x2A4D;
constexpr uint8_t  HID_PROTOCOL_MODE_BOOT     = 0x00;

// NimBLE's BLE_UUIDxx_DECLARE() macros take the address of a compound literal, which is an
// rvalue in C++ (unlike C) and can't have its address taken directly -- so peer_*_find_uuid()
// below are handed these named ble_uuid16_t's (via their common `.u` base member) instead.
const ble_uuid16_t HID_SVC_UUID_T               = BLE_UUID16_INIT(HID_SVC_UUID);
const ble_uuid16_t HID_CHR_PROTOCOL_MODE_UUID_T = BLE_UUID16_INIT(HID_CHR_PROTOCOL_MODE_UUID);
const ble_uuid16_t HID_CHR_BOOT_KEYBOARD_UUID_T = BLE_UUID16_INIT(HID_CHR_BOOT_KEYBOARD_UUID);
const ble_uuid16_t HID_CHR_BOOT_MOUSE_UUID_T    = BLE_UUID16_INIT(HID_CHR_BOOT_MOUSE_UUID);

constexpr int MAX_HID_CONNECTIONS = 4;
constexpr int MAX_GENERIC_REPORTS = 4;

struct hid_connection_t {
    uint16_t conn_handle = 0xffff; // 0xffff = free slot
    uint16_t boot_kb_handle = 0;
    uint16_t boot_mouse_handle = 0;
    uint8_t previous_boot_report[8]{};
    uint16_t generic_report_handles[MAX_GENERIC_REPORTS]{};
    uint8_t generic_report_count = 0;
};
inline hid_connection_t hid_connections[MAX_HID_CONNECTIONS]{};
inline Feature::BLE::Keyboard::EventCallback keyboard_callback = nullptr;
inline Feature::BLE::HID::EventCallback hid_callback = nullptr;
inline Feature::BLE::Mouse::EventCallback mouse_callback = nullptr;
inline Feature::BLE::Mouse::ConnectionCallback mouse_connection_callback = nullptr;
inline uint8_t mouse_connection_count = 0;

inline hid_connection_t* hid_find(uint16_t conn_handle) {
    for (auto& c : hid_connections) if (c.conn_handle == conn_handle) return &c;
    return nullptr;
}

inline hid_connection_t* hid_find_or_alloc(uint16_t conn_handle) {
    hid_connection_t* existing = hid_find(conn_handle);
    if (existing != nullptr) return existing;
    for (auto& c : hid_connections) {
        if (c.conn_handle == 0xffff) {
            c = hid_connection_t{};
            c.conn_handle = conn_handle;
            return &c;
        }
    }
    return nullptr;
}

inline void hid_forget(uint16_t conn_handle) {
    hid_connection_t* c = hid_find(conn_handle);
    if (c == nullptr) return;
    if (c->boot_mouse_handle != 0) {
        if (mouse_connection_count > 0) --mouse_connection_count;
        if (mouse_connection_count == 0 && mouse_connection_callback != nullptr) {
            mouse_connection_callback(false);
        }
    }
    *c = hid_connection_t{};
}

inline bool boot_report_contains_key(const uint8_t* report, uint8_t key) {
    for (size_t i = 2; i < 8; ++i) if (report[i] == key) return true;
    return false;
}

inline char boot_key_to_ascii(uint8_t key, bool shift) {
    if (key >= 0x04 && key <= 0x1d) {
        const char value = static_cast<char>('a' + key - 0x04);
        return shift ? static_cast<char>(value - 'a' + 'A') : value;
    }
    static constexpr char normal[]  = "1234567890\n\x1b\b\t -=[]\\#;'`,./";
    static constexpr char shifted[] = "!@#$%^&*()\n\x1b\b\t _+{}|~:\"~<>?";
    if (key >= 0x1e && key <= 0x38) return (shift ? shifted : normal)[key - 0x1e];
    return key == 0x2c ? ' ' : '\0';
}

inline void emit_key_event(uint16_t conn_handle, uint8_t keycode, bool shift, bool pressed, bool is_modifier) {
    if (keyboard_callback == nullptr) return;
    const Feature::BLE::Keyboard::Event event{
        conn_handle, keycode, is_modifier ? '\0' : boot_key_to_ascii(keycode, shift), shift, pressed, is_modifier
    };
    keyboard_callback(event);
}

inline void decode_boot_report(uint16_t conn_handle, const uint8_t* report, uint16_t length) {
    if (length < 8) return;
    hid_connection_t* conn = hid_find(conn_handle);
    if (conn == nullptr) return;

    // Keyboard error rollover (all-1/2/3 in the key array): ignore, matches USB HID handling.
    for (size_t i = 2; i < 8; ++i) {
        if (report[i] >= 1 && report[i] <= 3) return;
    }

    const bool shift = (report[0] & 0x22) != 0;
    const uint8_t changed_modifiers = report[0] ^ conn->previous_boot_report[0];
    for (uint8_t bit = 0; bit < 8; ++bit) {
        if ((changed_modifiers & (1u << bit)) != 0) {
            emit_key_event(conn_handle, static_cast<uint8_t>(0xe0 + bit), shift, (report[0] & (1u << bit)) != 0, true);
        }
    }
    for (size_t i = 2; i < 8; ++i) {
        const uint8_t key = report[i];
        if (key != 0 && !boot_report_contains_key(conn->previous_boot_report, key)) {
            emit_key_event(conn_handle, key, shift, true, false);
        }
    }
    std::memcpy(conn->previous_boot_report, report, 8);
}

inline void decode_boot_mouse_report(uint16_t conn_handle, const uint8_t* report, uint16_t length) {
    if (length < 3 || mouse_callback == nullptr) return;
    const Feature::BLE::Mouse::Event event{
        conn_handle, report[0], static_cast<int8_t>(report[1]), static_cast<int8_t>(report[2]),
        length >= 4 ? static_cast<int8_t>(report[3]) : static_cast<int8_t>(0)
    };
    mouse_callback(event);
}

inline void deliver_generic_report(uint16_t conn_handle, const uint8_t* data, uint16_t length) {
    if (hid_callback == nullptr) return;
    Feature::BLE::HID::Event event{};
    event.conn_handle = conn_handle;
    event.length = length > sizeof(event.data) ? static_cast<uint16_t>(sizeof(event.data)) : length;
    std::memcpy(event.data, data, event.length);
    hid_callback(event);
}

inline bool hid_advertises_interest(const struct ble_hs_adv_fields& fields) {
    for (int i = 0; i < fields.num_uuids16; ++i) {
        if (ble_uuid_u16(&fields.uuids16[i].u) == HID_SVC_UUID) return true;
    }
    return false;
}

inline void hid_subscribe(uint16_t conn_handle, const struct peer_chr* chr) {
    const struct peer_dsc* cccd = nullptr;
    const struct peer_dsc* d;
    SLIST_FOREACH(d, &chr->dscs, next) {
        if (ble_uuid_u16(&d->dsc.uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) { cccd = d; break; }
    }
    if (cccd == nullptr) {
        ESP_LOGW(HID_TAG, "HID report (handle=%u) has no CCCD, cannot subscribe", chr->chr.val_handle);
        return;
    }
    uint8_t value[2] = {1, 0};
    const int rc = ble_gattc_write_flat(conn_handle, cccd->dsc.handle, value, sizeof(value), nullptr, nullptr);
    if (rc != 0) {
        ESP_LOGW(HID_TAG, "Failed to subscribe to HID report (handle=%u); rc=%d", chr->chr.val_handle, rc);
    }
}

inline void hid_on_peer_ready(const struct peer* p) {
    const struct peer_svc* svc = peer_svc_find_uuid(p, &HID_SVC_UUID_T.u);
    if (svc == nullptr) return; // not a HID device

    ESP_LOGI(HID_TAG, "HID-over-GATT service found; conn_handle=%u", p->conn_handle);
    hid_connection_t* conn = hid_find_or_alloc(p->conn_handle);
    if (conn == nullptr) {
        ESP_LOGW(HID_TAG, "Too many concurrent BLE HID connections tracked");
        return;
    }

    // Prefer Boot Protocol Mode: a fixed 8-byte keyboard report that works the same way
    // across virtually every BLE keyboard, regardless of its own custom Report Map.
    const struct peer_chr* protocol_mode = peer_chr_find_uuid(p, &HID_SVC_UUID_T.u,
                                                                &HID_CHR_PROTOCOL_MODE_UUID_T.u);
    if (protocol_mode != nullptr) {
        uint8_t boot_mode = HID_PROTOCOL_MODE_BOOT;
        ble_gattc_write_no_rsp_flat(p->conn_handle, protocol_mode->chr.val_handle, &boot_mode, sizeof(boot_mode));
    }

    const struct peer_chr* boot_kb = peer_chr_find_uuid(p, &HID_SVC_UUID_T.u,
                                                          &HID_CHR_BOOT_KEYBOARD_UUID_T.u);
    if (boot_kb != nullptr) {
        conn->boot_kb_handle = boot_kb->chr.val_handle;
        hid_subscribe(p->conn_handle, boot_kb);
        ESP_LOGI(HID_TAG, "Subscribed to BLE Boot Keyboard reports; conn_handle=%u", p->conn_handle);
    }

    const struct peer_chr* boot_mouse = peer_chr_find_uuid(p, &HID_SVC_UUID_T.u,
                                                             &HID_CHR_BOOT_MOUSE_UUID_T.u);
    if (boot_mouse != nullptr) {
        conn->boot_mouse_handle = boot_mouse->chr.val_handle;
        hid_subscribe(p->conn_handle, boot_mouse);
        ESP_LOGI(HID_TAG, "Subscribed to BLE Boot Mouse reports; conn_handle=%u", p->conn_handle);
        const bool was_disconnected = mouse_connection_count == 0;
        ++mouse_connection_count;
        // Bump the counter before firing the callback: it re-queries is_connected()
        // (see app_events.hpp's update_mouse_cursor_visibility()), which would still
        // see the pre-increment count of 0 otherwise and conclude nothing is connected.
        if (was_disconnected && mouse_connection_callback != nullptr) {
            mouse_connection_callback(true);
        }
    }

    // Also subscribe to every notifying generic Report characteristic, covering HID devices
    // (gamepads, non-keyboard peripherals, keyboards without Boot Protocol) that skip it.
    const struct peer_chr* chr;
    SLIST_FOREACH(chr, &svc->chrs, next) {
        if (ble_uuid_u16(&chr->chr.uuid.u) != HID_CHR_REPORT_UUID) continue;
        if ((chr->chr.properties & BLE_GATT_CHR_PROP_NOTIFY) == 0) continue;
        if (conn->generic_report_count >= MAX_GENERIC_REPORTS) break;
        conn->generic_report_handles[conn->generic_report_count++] = chr->chr.val_handle;
        hid_subscribe(p->conn_handle, chr);
    }
    if (conn->generic_report_count > 0) {
        ESP_LOGI(HID_TAG, "Subscribed to %u generic HID report characteristic(s); conn_handle=%u",
                 conn->generic_report_count, p->conn_handle);
    }
}

inline void hid_on_disconnected(uint16_t conn_handle) {
    hid_forget(conn_handle);
}

inline void hid_on_notify(uint16_t conn_handle, uint16_t attr_handle, const uint8_t* data, uint16_t length) {
    const hid_connection_t* conn = hid_find(conn_handle);
    if (conn == nullptr) return;
    if (attr_handle == conn->boot_kb_handle) {
        decode_boot_report(conn_handle, data, length);
        return;
    }
    if (attr_handle == conn->boot_mouse_handle) {
        decode_boot_mouse_report(conn_handle, data, length);
        return;
    }
    for (uint8_t i = 0; i < conn->generic_report_count; ++i) {
        if (attr_handle == conn->generic_report_handles[i]) {
            deliver_generic_report(conn_handle, data, length);
            return;
        }
    }
}
} // namespace

} // namespace Feature::BLE

namespace Feature::BLE::Keyboard {

/**
 * @brief Register a callback invoked for every BLE Boot Keyboard key event.
 *        Pass nullptr to unregister. Only one callback is stored at a time.
 */
inline void set_callback(EventCallback callback) {
    Feature::BLE::keyboard_callback = callback;
}

/**
 * @brief Human readable name for a modifier keycode (0xE0-0xE7), "Unknown" otherwise.
 */
inline const char* modifier_name(uint8_t keycode) {
    static constexpr const char* names[] = {
        "Left Ctrl", "Left Shift", "Left Alt", "Left GUI/Windows",
        "Right Ctrl", "Right Shift", "Right Alt", "Right GUI/Windows"
    };
    return keycode >= 0xe0 && keycode <= 0xe7 ? names[keycode - 0xe0] : "Unknown";
}

} // namespace Feature::BLE::Keyboard

namespace Feature::BLE::Mouse {

/**
 * @brief Register a callback invoked for every BLE Boot Mouse report.
 *        Pass nullptr to unregister. Only one callback is stored at a time.
 */
inline void set_callback(EventCallback callback) {
    Feature::BLE::mouse_callback = callback;
}

/**
 * @brief Register a callback invoked when a BLE mouse connects or disconnects.
 *        Pass nullptr to unregister. Only one callback is stored at a time.
 */
inline void set_connection_callback(ConnectionCallback callback) {
    Feature::BLE::mouse_connection_callback = callback;
}

/**
 * @brief True if at least one BLE mouse is currently connected.
 */
inline bool is_connected() {
    return Feature::BLE::mouse_connection_count > 0;
}

} // namespace Feature::BLE::Mouse

namespace Feature::BLE::HID {

/**
 * @brief Register a callback invoked for every raw report from a non-Boot-Keyboard HID
 *        characteristic (generic HID devices). Pass nullptr to unregister.
 */
inline void set_callback(EventCallback callback) {
    Feature::BLE::hid_callback = callback;
}

} // namespace Feature::BLE::HID

extern "C" __attribute__((used)) void ble_hid_host_start() {
    using namespace Feature::BLE;
    ProfileHooks hooks{};
    hooks.name = "HID";
    hooks.advertises_interest = hid_advertises_interest;
    hooks.on_peer_ready = hid_on_peer_ready;
    hooks.on_disconnected = hid_on_disconnected;
    hooks.on_notify = hid_on_notify;
    register_profile(hooks);
    ESP_LOGI(HID_TAG, "BLE HID-over-GATT host enabled (keyboards + generic HID devices)");
}
