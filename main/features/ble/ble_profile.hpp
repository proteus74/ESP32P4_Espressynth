#pragma once

#include <cstdint>
#include "host/ble_gap.h"
#include "esp_central.h"

// Shared vocabulary between the BLE central core (ble_manager.hpp) and the individual BLE
// "profile" hosts (ble_hid_host.hpp, ble_midi_host.hpp). Kept in its own header so the profile
// files never need to include ble_manager.hpp back: NimBLE only allows one GAP event callback
// per connection, so ble_manager.hpp owns scanning/connecting/GATT-discovery and fans events
// out to whichever profiles registered interest, instead of each profile driving GAP itself
// (unlike the USB Host side, where TinyUSB's own class-driver dispatch makes that unnecessary).
namespace Feature::BLE {

struct ProfileHooks {
    // Human-readable device class label shown by discovery listings (e.g. the CLI's
    // "ble-scan") when advertises_interest() below matches, e.g. "HID", "MIDI". nullptr if
    // this profile doesn't want to label matching devices.
    const char* name = nullptr;
    // Return true if this advertisement looks like a device this profile wants to connect to.
    bool (*advertises_interest)(const struct ble_hs_adv_fields& fields) = nullptr;
    // Called once GATT service/characteristic discovery completes for a newly connected peer.
    // Implementations should check peer_svc_find_uuid() for their own service and, if present,
    // subscribe to their characteristic(s); no-op if the peer doesn't support this profile.
    void (*on_peer_ready)(const struct peer* p) = nullptr;
    // Called when a connection is torn down, so any per-connection state can be released.
    void (*on_disconnected)(uint16_t conn_handle) = nullptr;
    // Called for every GATT notification/indication received on any connection; implementations
    // should ignore attr_handle values that don't belong to them.
    void (*on_notify)(uint16_t conn_handle, uint16_t attr_handle, const uint8_t* data, uint16_t length) = nullptr;
};

namespace {
constexpr int MAX_PROFILES = 4;
inline ProfileHooks registered_profiles[MAX_PROFILES]{};
inline int registered_profile_count = 0;
} // namespace

/**
 * @brief Register a BLE central profile (HID host, MIDI host, ...). Returns false if the
 *        fixed-size registry is full.
 */
inline bool register_profile(const ProfileHooks& hooks) {
    if (registered_profile_count >= MAX_PROFILES) return false;
    registered_profiles[registered_profile_count++] = hooks;
    return true;
}

inline int profile_count() { return registered_profile_count; }
inline const ProfileHooks& profile_at(int index) { return registered_profiles[index]; }

} // namespace Feature::BLE
