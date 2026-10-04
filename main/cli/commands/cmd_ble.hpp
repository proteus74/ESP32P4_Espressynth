#pragma once

// "ble <scan|connect|disconnect|list|forget|auto> [mac] [on|off]": single command for BLE discovery/connection
// management. Only registered when Bluetooth LE is enabled (CONFIG_APP_ENABLE_BLUETOOTH);
// backed by main/features/ble/ble_manager.hpp's device discovery and connection tracking.

#include "esp_console.h"
#include "esp_err.h"
#include "features/ble/ble_manager.hpp"

#include <cstdio>
#include <cstring>

namespace Feature::CLI::Commands {

namespace {

constexpr uint32_t BLE_SCAN_DURATION_MS = 4000;
constexpr const char* BLE_USAGE = "Usage: ble <scan|list> | ble <connect|disconnect|forget> <mac> | ble auto <mac> <on|off>\n";

inline int ble_scan() {
    printf("Scanning for BLE devices (%u ms)...\n", (unsigned)BLE_SCAN_DURATION_MS);
    if (Feature::BLE::scan(BLE_SCAN_DURATION_MS) != ESP_OK) {
        printf("Failed to start BLE scan.\n");
        return 1;
    }

    const int count = Feature::BLE::discovered_device_count();
    if (count == 0) {
        printf("No BLE devices found.\n");
        return 0;
    }

    char mac[18];
    for (int i = 0; i < count; ++i) {
        const Feature::BLE::DiscoveredDevice& device = Feature::BLE::discovered_device_at(i);
        Feature::BLE::format_mac(device.addr, mac, sizeof(mac));
        printf("%s  rssi=%4d  %-4s  %s\n", mac, device.rssi,
               device.device_class[0] != '\0' ? device.device_class : "-",
               device.name[0] != '\0' ? device.name : "(no name)");
    }
    return 0;
}

inline int ble_connect(const char* mac) {
    const esp_err_t err = Feature::BLE::connect_by_mac(mac);
    if (err != ESP_OK) {
        printf("Connect to %s failed: %s\n", mac,
               err == ESP_ERR_NOT_FOUND ? "unknown address, run 'ble scan' first" : esp_err_to_name(err));
        return 1;
    }
    printf("Connecting to %s...\n", mac);
    return 0;
}

inline int ble_disconnect(const char* mac) {
    const esp_err_t err = Feature::BLE::disconnect_by_mac(mac);
    if (err != ESP_OK) {
        printf("Disconnect from %s failed: %s\n", mac, err == ESP_ERR_NOT_FOUND ? "not connected" : esp_err_to_name(err));
        return 1;
    }
    printf("Disconnecting from %s...\n", mac);
    return 0;
}

inline int ble_list() {
    const int count = Feature::BLE::connected_device_count();
    char mac[18];
    if (count == 0) printf("No BLE devices connected.\n");
    for (int i = 0; i < count; ++i) {
        const Feature::BLE::ConnectedDevice device = Feature::BLE::connected_device_at(i);
        Feature::BLE::format_mac(device.addr, mac, sizeof(mac));
        printf("%s  conn_handle=%u  %-4s  %s\n", mac, device.conn_handle,
               device.device_class[0] != '\0' ? device.device_class : "-",
               device.name[0] != '\0' ? device.name : "(no name)");
    }

    const int known = Feature::BLE::known_device_count();
    if (known > 0) printf("Known devices:\n");
    for (int i = 0; i < known; ++i) {
        const Feature::BLE::KnownDevice device = Feature::BLE::known_device_at(i);
        Feature::BLE::format_mac(device.addr, mac, sizeof(mac));
        printf("%s  auto=%s  %s\n", mac, device.autoconnect ? "on " : "off",
               device.name[0] != '\0' ? device.name : "(no name)");
    }
    return 0;
}

inline int ble_report(esp_err_t err, const char* what, const char* mac) {
    if (err == ESP_OK) return 0;
    printf("%s %s failed: %s\n", what, mac, err == ESP_ERR_NOT_FOUND ? "unknown device" : esp_err_to_name(err));
    return 1;
}

inline int cli_cmd_ble(int argc, char** argv) {
    if (argc < 2) {
        printf("%s", BLE_USAGE);
        return 1;
    }
    const char* sub = argv[1];

    if (std::strcmp(sub, "scan") == 0 && argc == 2) return ble_scan();
    if (std::strcmp(sub, "connect") == 0 && argc == 3) return ble_connect(argv[2]);
    if (std::strcmp(sub, "disconnect") == 0 && argc == 3) return ble_disconnect(argv[2]);
    if (std::strcmp(sub, "list") == 0 && argc == 2) return ble_list();
    if (std::strcmp(sub, "forget") == 0 && argc == 3) return ble_report(Feature::BLE::forget(argv[2]), "Forget", argv[2]);
    if (std::strcmp(sub, "auto") == 0 && argc == 4) {
        return ble_report(Feature::BLE::set_autoconnect(argv[2], std::strcmp(argv[3], "on") == 0), "Auto-connect", argv[2]);
    }

    printf("%s", BLE_USAGE);
    return 1;
}

} // namespace

inline void register_ble() {
    const esp_console_cmd_t cmd = {
        .command = "ble",
        .help = "Manage BLE: \"scan\", \"connect\"/\"disconnect\"/\"forget\" <mac>, \"auto\" <mac> <on|off>, \"list\" connected + known devices",
        .hint = "<scan|connect|disconnect|list|forget|auto> [mac] [on|off]",
        .func = &cli_cmd_ble,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    register_command(cmd);
}

} // namespace Feature::CLI::Commands
