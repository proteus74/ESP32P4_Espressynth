#pragma once

#include "boards/board_config.hpp"
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_hosted.h"
#include "esp_hosted_bt_host_stack.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "esp_central.h"
#include "ble_profile.hpp"

#if defined(CONFIG_BLE_ENABLE_MIDI_HOST)
    #include "ble_midi_host.hpp"
#endif
#if defined(CONFIG_BLE_ENABLE_HID_HOST)
    #include "ble_hid_host.hpp"
#endif

// Provided by NimBLE's bonding/persist store (nvs-backed), compiled in whenever
// CONFIG_BT_NIMBLE_ENABLED is set. No header ships the prototype, so declare it as the
// upstream NimBLE examples do.
extern "C" void ble_store_config_init(void);

namespace Feature::BLE {

// A device seen while scanning (Feature::BLE::scan()), regardless of whether any registered
// profile (HID/MIDI host) recognises it. Used by the CLI's "ble-scan" command.
struct DiscoveredDevice {
    ble_addr_t addr{};
    int8_t rssi = 0;
    char name[32] = {};
    char device_class[16] = {}; // e.g. "HID", "MIDI"; empty if no profile recognises it
};

// A currently connected peer, tracked independently of profile matching. Used by the CLI's
// "ble-list" command and to resolve a MAC address to a connection handle for "ble-disconnect".
struct ConnectedDevice {
    uint16_t conn_handle = 0xffff; // 0xffff = free slot
    ble_addr_t addr{};
    char name[32] = {};         // from a prior ble-scan discovery, if any; empty if unknown
    char device_class[16] = {}; // from a prior ble-scan discovery, if any; empty if unknown
};

/**
 * @brief Format a BLE address as "AA:BB:CC:DD:EE:FF". `out_size` must be at least 18.
 */
inline void format_mac(const ble_addr_t& addr, char* out, size_t out_size) {
    // addr.val[] is little-endian on the wire; human MAC notation lists the bytes MSB-first,
    // i.e. val[5] down to val[0].
    std::snprintf(out, out_size, "%02X:%02X:%02X:%02X:%02X:%02X",
                  addr.val[5], addr.val[4], addr.val[3], addr.val[2], addr.val[1], addr.val[0]);
}

namespace {
constexpr const char* TAG = "FeatureBLE";
inline int active_connections = 0;

constexpr int MAX_DISCOVERED_DEVICES = 16;
inline DiscoveredDevice discovered_devices[MAX_DISCOVERED_DEVICES]{};
inline int discovered_device_total = 0;

constexpr int MAX_TRACKED_CONNECTIONS = MYNEWT_VAL(BLE_MAX_CONNECTIONS);
inline ConnectedDevice connected_devices[MAX_TRACKED_CONNECTIONS]{};

int gap_event(struct ble_gap_event* event, void* arg);

// Parses "AA:BB:CC:DD:EE:FF" (case-insensitive) into 6 address bytes, in the same val[]
// (little-endian) order used by ble_addr_t / format_mac() above.
inline bool parse_mac(const char* str, uint8_t out_val[6]) {
    unsigned bytes[6];
    if (std::sscanf(str, "%2x:%2x:%2x:%2x:%2x:%2x",
                     &bytes[5], &bytes[4], &bytes[3], &bytes[2], &bytes[1], &bytes[0]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; ++i) out_val[i] = static_cast<uint8_t>(bytes[i]);
    return true;
}

// Returns the device_class label of the first registered profile (HID host, MIDI host, ...)
// whose advertises_interest() matches these advertising fields, nullptr if none do.
inline const char* classify_device(const struct ble_hs_adv_fields& fields) {
    for (int i = 0; i < profile_count(); ++i) {
        const ProfileHooks& hooks = profile_at(i);
        if (hooks.name != nullptr && hooks.advertises_interest != nullptr && hooks.advertises_interest(fields)) {
            return hooks.name;
        }
    }
    return nullptr;
}

inline void set_device_class(char* out, size_t out_size, const char* device_class) {
    if (device_class == nullptr) return; // leave as-is: keep a class found from an earlier report
    std::strncpy(out, device_class, out_size - 1);
    out[out_size - 1] = '\0';
}

// Records/updates an advertising device in discovered_devices, regardless of whether any
// profile is interested in it. Called for every BLE_GAP_EVENT_DISC seen during an explicit
// scan() window.
inline void record_discovery(const struct ble_gap_disc_desc* disc) {
    struct ble_hs_adv_fields fields{};
    const bool have_fields = ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) == 0;
    const char* device_class = have_fields ? classify_device(fields) : nullptr;

    for (int i = 0; i < discovered_device_total; ++i) {
        if (ble_addr_cmp(&discovered_devices[i].addr, &disc->addr) == 0) {
            discovered_devices[i].rssi = disc->rssi;
            if (have_fields && fields.name_len > 0 && discovered_devices[i].name[0] == '\0') {
                const size_t len = fields.name_len < sizeof(discovered_devices[i].name) - 1
                                        ? fields.name_len : sizeof(discovered_devices[i].name) - 1;
                std::memcpy(discovered_devices[i].name, fields.name, len);
                discovered_devices[i].name[len] = '\0';
            }
            set_device_class(discovered_devices[i].device_class, sizeof(discovered_devices[i].device_class),
                              device_class);
            return;
        }
    }
    if (discovered_device_total >= MAX_DISCOVERED_DEVICES) return;

    DiscoveredDevice& entry = discovered_devices[discovered_device_total++];
    entry.addr = disc->addr;
    entry.rssi = disc->rssi;
    entry.name[0] = '\0';
    entry.device_class[0] = '\0';
    if (have_fields && fields.name_len > 0) {
        const size_t len = fields.name_len < sizeof(entry.name) - 1 ? fields.name_len : sizeof(entry.name) - 1;
        std::memcpy(entry.name, fields.name, len);
        entry.name[len] = '\0';
    }
    set_device_class(entry.device_class, sizeof(entry.device_class), device_class);
}

inline void on_disc_complete(const struct peer* p, int status, void* arg) {
    if (status != 0) {
        ESP_LOGW(TAG, "GATT discovery failed; status=%d conn_handle=%u", status, p->conn_handle);
        ble_gap_terminate(p->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }

    ESP_LOGI(TAG, "GATT discovery complete; conn_handle=%u", p->conn_handle);
    for (int i = 0; i < profile_count(); ++i) {
        const ProfileHooks& hooks = profile_at(i);
        if (hooks.on_peer_ready != nullptr) hooks.on_peer_ready(p);
    }
}

inline int gap_event(struct ble_gap_event* event, void* arg) {
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        record_discovery(&event->disc);
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "BLE connected; conn_handle=%u", event->connect.conn_handle);
            if (peer_add(event->connect.conn_handle) != 0) {
                ESP_LOGW(TAG, "Failed to track BLE peer; conn_handle=%u", event->connect.conn_handle);
                ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                return 0;
            }
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0) {
                for (auto& c : connected_devices) {
                    if (c.conn_handle == 0xffff) {
                        c.conn_handle = event->connect.conn_handle;
                        c.addr = desc.peer_ota_addr;
                        c.name[0] = '\0';
                        c.device_class[0] = '\0';
                        // Reuse the name/class from a prior discovery (ble-scan), if we saw one.
                        for (int i = 0; i < discovered_device_total; ++i) {
                            if (ble_addr_cmp(&discovered_devices[i].addr, &desc.peer_ota_addr) == 0) {
                                std::strncpy(c.name, discovered_devices[i].name, sizeof(c.name) - 1);
                                std::strncpy(c.device_class, discovered_devices[i].device_class,
                                             sizeof(c.device_class) - 1);
                                break;
                            }
                        }
                        break;
                    }
                }
            }
            ++active_connections;
            const int rc = peer_disc_all(event->connect.conn_handle, on_disc_complete, nullptr);
            if (rc != 0) {
                ESP_LOGW(TAG, "Failed to start GATT discovery; rc=%d", rc);
            }
        } else {
            ESP_LOGW(TAG, "BLE connect attempt failed; status=%d", event->connect.status);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected; conn_handle=%u reason=%d",
                 event->disconnect.conn.conn_handle, event->disconnect.reason);
        for (int i = 0; i < profile_count(); ++i) {
            const ProfileHooks& hooks = profile_at(i);
            if (hooks.on_disconnected != nullptr) hooks.on_disconnected(event->disconnect.conn.conn_handle);
        }
        for (auto& c : connected_devices) {
            if (c.conn_handle == event->disconnect.conn.conn_handle) {
                c = ConnectedDevice{};
                break;
            }
        }
        peer_delete(event->disconnect.conn.conn_handle);
        if (active_connections > 0) --active_connections;
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint8_t buffer[64];
        const uint16_t length = static_cast<uint16_t>(OS_MBUF_PKTLEN(event->notify_rx.om));
        const uint16_t copy_length = length > sizeof(buffer) ? static_cast<uint16_t>(sizeof(buffer)) : length;
        if (os_mbuf_copydata(event->notify_rx.om, 0, copy_length, buffer) != 0) return 0;
        for (int i = 0; i < profile_count(); ++i) {
            const ProfileHooks& hooks = profile_at(i);
            if (hooks.on_notify != nullptr) {
                hooks.on_notify(event->notify_rx.conn_handle, event->notify_rx.attr_handle, buffer, copy_length);
            }
        }
        return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        // Already bonded but the peer wants to pair again: drop the stale bond and retry.
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    default:
        return 0;
    }
}

inline void on_reset(int reason) {
    ESP_LOGW(TAG, "NimBLE host reset; reason=%d", reason);
}

inline void on_sync() {
    ble_hs_util_ensure_addr(0);
    active_connections = 0;
    // No scanning and no auto-connect at boot: discovery only happens on demand, via the
    // CLI's "ble-scan" (and manual "ble-connect") commands.
}

inline void host_task(void*) {
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run(); // returns only after nimble_port_stop()
    nimble_port_freertos_deinit();
}
} // namespace

/**
 * @brief Run a timed active discovery window, collecting every advertising BLE device
 *        (regardless of profile support) into the list returned by discovered_device_count()/
 *        discovered_device_at(). Blocks the calling task for approximately duration_ms.
 *        Continuous background scanning (for HID/MIDI auto-connect) resumes automatically
 *        once the window ends.
 */
inline esp_err_t scan(uint32_t duration_ms) {
    discovered_device_total = 0;
    ble_gap_disc_cancel(); // defensive: cancel any lingering discovery before starting a fresh one

    uint8_t own_addr_type;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) return ESP_FAIL;

    struct ble_gap_disc_params disc_params{};
    disc_params.passive = 1;
    // Don't de-duplicate: a device's name is often only present in its scan response,
    // a separate report from its ADV_IND.
    disc_params.filter_duplicates = 0;

    const int rc = ble_gap_disc(own_addr_type, static_cast<int32_t>(duration_ms), &disc_params, gap_event, nullptr);
    if (rc != 0) {
        ESP_LOGW(TAG, "Failed to start BLE scan; rc=%d", rc);
        return ESP_FAIL;
    }

    vTaskDelay(pdMS_TO_TICKS(duration_ms + 200));
    return ESP_OK;
}

inline int discovered_device_count() { return discovered_device_total; }
inline const DiscoveredDevice& discovered_device_at(int index) { return discovered_devices[index]; }

inline int connected_device_count() {
    int count = 0;
    for (const auto& c : connected_devices) if (c.conn_handle != 0xffff) ++count;
    return count;
}

inline ConnectedDevice connected_device_at(int index) {
    int seen = 0;
    for (const auto& c : connected_devices) {
        if (c.conn_handle == 0xffff) continue;
        if (seen == index) return c;
        ++seen;
    }
    return ConnectedDevice{};
}

/**
 * @brief Connect to a device by MAC address ("AA:BB:CC:DD:EE:FF"), as previously seen by
 *        scan(). Returns ESP_ERR_INVALID_ARG if the address doesn't parse, ESP_ERR_NOT_FOUND
 *        if it wasn't seen by a scan (its address type -- public/random -- is unknown).
 */
inline esp_err_t connect_by_mac(const char* mac_str) {
    uint8_t val[6];
    if (!parse_mac(mac_str, val)) return ESP_ERR_INVALID_ARG;

    ble_addr_t target{};
    std::memcpy(target.val, val, sizeof(val));

    bool known = false;
    for (int i = 0; i < discovered_device_total; ++i) {
        if (std::memcmp(discovered_devices[i].addr.val, val, sizeof(val)) == 0) {
            target.type = discovered_devices[i].addr.type;
            known = true;
            break;
        }
    }
    if (!known) return ESP_ERR_NOT_FOUND;

    ble_gap_disc_cancel(); // ok if not currently scanning

    uint8_t own_addr_type;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) return ESP_FAIL;

    const int rc = ble_gap_connect(own_addr_type, &target, 30000, nullptr, gap_event, nullptr);
    if (rc != 0) {
        ESP_LOGW(TAG, "Failed to connect to %s; rc=%d", mac_str, rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * @brief Disconnect the currently connected device with the given MAC address. Returns
 *        ESP_ERR_INVALID_ARG if the address doesn't parse, ESP_ERR_NOT_FOUND if no connected
 *        device matches it.
 */
inline esp_err_t disconnect_by_mac(const char* mac_str) {
    uint8_t val[6];
    if (!parse_mac(mac_str, val)) return ESP_ERR_INVALID_ARG;

    for (const auto& c : connected_devices) {
        if (c.conn_handle != 0xffff && std::memcmp(c.addr.val, val, sizeof(val)) == 0) {
            return ble_gap_terminate(c.conn_handle, BLE_ERR_REM_USER_CONN_TERM) == 0 ? ESP_OK : ESP_FAIL;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

/**
 * @brief Bring up Bluetooth LE: connect to the ESP-Hosted co-processor, start its BT
 *        controller, and initialize a NimBLE central. No scanning or connecting happens
 *        automatically -- use scan()/connect_by_mac() (or the CLI's "ble-scan"/"ble-connect")
 *        to discover and connect to peripherals. Once connected, a registered profile (BLE
 *        HID host, BLE-MIDI host) that recognises the peer still drives its own GATT setup.
 */
inline esp_err_t init() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    if (esp_hosted_connect_to_slave() != 0) {
        ESP_LOGE(TAG, "Failed to connect to ESP-Hosted co-processor");
        return ESP_FAIL;
    }

    esp_hosted_coprocessor_fwver_t fw_version{};
    const bool have_fw_version = esp_hosted_get_coprocessor_fwversion(&fw_version) == ESP_OK;

    // Bind NimBLE to ESP-Hosted's stack-agnostic HCI byte pipe: brings up the BT
    // controller on the co-processor and wires the hosted HCI transport to it.
    // (Built by hand rather than via ESP_HOSTED_BT_HOST_STACK_CONFIG_DEFAULT(): that
    // macro's partial `.custom = {0}` trips -Wmissing-field-initializers under -Werror.)
    esp_hosted_bt_host_stack_cfg_t bt_stack_cfg{};
    bt_stack_cfg.stack = ESP_HOSTED_BT_HOST_STACK_DEFAULT_TYPE;
    bt_stack_cfg.bring_up_controller = true;
    bt_stack_cfg.controller_ready_timeout_ms = EH_BT_CTRL_DEFAULT_READY_TIMEOUT_MS;
    if (esp_hosted_bt_host_stack_setup(&bt_stack_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to bind NimBLE to the hosted BT controller");
        return ESP_FAIL;
    }

    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    const int peer_rc = peer_init(MYNEWT_VAL(BLE_MAX_CONNECTIONS), 20, 32, 32);
    if (peer_rc != 0) {
        ESP_LOGE(TAG, "peer_init failed; rc=%d", peer_rc);
        return ESP_FAIL;
    }

#if defined(CONFIG_BT_NIMBLE_GAP_SERVICE)
    ble_svc_gap_device_name_set(CurrentBoard::BOARD_NAME);
#endif
    ble_store_config_init();

    // Let each enabled profile register its GAP/GATT hooks before scanning starts.
#if defined(CONFIG_BLE_ENABLE_MIDI_HOST)
    ble_midi_host_start();
#endif
#if defined(CONFIG_BLE_ENABLE_HID_HOST)
    ble_hid_host_start();
#endif

    nimble_port_freertos_init(host_task);

    if (have_fw_version) {
        ESP_LOGI(TAG, "Bluetooth LE initialized: co-processor fw v%" PRIu32 ".%" PRIu32 ".%" PRIu32,
                 fw_version.major1, fw_version.minor1, fw_version.patch1);
    } else {
        ESP_LOGI(TAG, "Bluetooth LE initialized");
    }
    return ESP_OK;
}

} // namespace Feature::BLE
