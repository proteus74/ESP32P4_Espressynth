#pragma once

// "wifi <status|scan|connect|disconnect|forget> [ssid] [password]": single command for Wi-Fi
// status/discovery/connection management. "connect" also saves the credentials in NVS so they
// survive a reboot (taking precedence over the menuconfig ones); "forget" clears them. Only
// registered when Wi-Fi is enabled (CONFIG_APP_ENABLE_WIFI); backed by
// main/features/wifi/wifi_manager.hpp.

#include "esp_console.h"
#include "esp_wifi.h"
#include "features/wifi/wifi_manager.hpp"

#include <cstdio>
#include <cstring>

namespace Feature::CLI::Commands {

namespace {

constexpr const char* WIFI_USAGE = "Usage: wifi <status|scan|connect|disconnect|forget> [ssid] [password]\n";

inline const char* auth_mode_name(wifi_auth_mode_t mode) {
    switch (mode) {
        case WIFI_AUTH_OPEN:  return "open";
        case WIFI_AUTH_WEP:   return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
        case WIFI_AUTH_WPA3_PSK: return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
        default: return "other";
    }
}

inline int wifi_status() {
    char saved_ssid[33] = {};
    char saved_password[65] = {};
    if (Feature::WiFi::load_saved_credentials(saved_ssid, sizeof(saved_ssid), saved_password, sizeof(saved_password))) {
        printf("Saved network: %s (from \"wifi connect\"; \"wifi forget\" to clear)\n", saved_ssid);
    } else {
        printf("Saved network: none (boot uses the menuconfig SSID \"%s\")\n", CONFIG_WIFI_SSID);
    }
    if (!Feature::WiFi::is_connected()) {
        printf("Not connected.\n");
        return 0;
    }

    wifi_ap_record_t ap_info{};
    if (Feature::WiFi::get_ap_info(ap_info) == ESP_OK) {
        printf("SSID: %s\n", reinterpret_cast<const char*>(ap_info.ssid));
        printf("RSSI: %d dBm\n", ap_info.rssi);
        printf("Auth: %s\n", auth_mode_name(ap_info.authmode));
    }

    esp_netif_ip_info_t ip_info{};
    if (Feature::WiFi::get_ip_info(ip_info) == ESP_OK) {
        printf("IP:      " IPSTR "\n", IP2STR(&ip_info.ip));
        printf("Netmask: " IPSTR "\n", IP2STR(&ip_info.netmask));
        printf("Gateway: " IPSTR "\n", IP2STR(&ip_info.gw));
    }
    return 0;
}

inline int wifi_scan() {
    printf("Scanning...\n");

    constexpr int MAX_RESULTS = 20;
    static Feature::WiFi::ScanResult results[MAX_RESULTS];
    const int count = Feature::WiFi::scan(results, MAX_RESULTS);
    if (count < 0) {
        printf("Scan failed.\n");
        return 1;
    }
    if (count == 0) {
        printf("No networks found.\n");
        return 0;
    }

    printf("%-33s %5s  %s\n", "SSID", "RSSI", "Auth");
    for (int i = 0; i < count; ++i) {
        printf("%-33s %5d  %s\n", results[i].ssid, results[i].rssi, auth_mode_name(results[i].authmode));
    }
    return 0;
}

inline int wifi_connect(const char* ssid, const char* password) {
    const esp_err_t err = Feature::WiFi::connect(ssid, password);
    if (err != ESP_OK) {
        printf("Connect failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    const esp_err_t save_err = Feature::WiFi::save_credentials(ssid, password);
    if (save_err != ESP_OK) {
        printf("Connecting to %s... (could not save credentials: %s)\n", ssid, esp_err_to_name(save_err));
        return 0;
    }
    printf("Connecting to %s... credentials saved, used again after reboot.\n", ssid);
    return 0;
}

inline int wifi_forget() {
    const esp_err_t err = Feature::WiFi::clear_saved_credentials();
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        printf("Forget failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Saved credentials cleared - next boot uses the menuconfig SSID \"%s\".\n", CONFIG_WIFI_SSID);
    return 0;
}

inline int wifi_disconnect() {
    const esp_err_t err = Feature::WiFi::disconnect();
    if (err != ESP_OK) {
        printf("Disconnect failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Disconnecting...\n");
    return 0;
}

inline int cli_cmd_wifi(int argc, char** argv) {
    if (argc < 2) {
        printf("%s", WIFI_USAGE);
        return 1;
    }
    const char* sub = argv[1];

    if (std::strcmp(sub, "status") == 0 && argc == 2) return wifi_status();
    if (std::strcmp(sub, "scan") == 0 && argc == 2) return wifi_scan();
    if (std::strcmp(sub, "connect") == 0 && (argc == 3 || argc == 4)) {
        return wifi_connect(argv[2], argc == 4 ? argv[3] : nullptr);
    }
    if (std::strcmp(sub, "disconnect") == 0 && argc == 2) return wifi_disconnect();
    if (std::strcmp(sub, "forget") == 0 && argc == 2) return wifi_forget();

    printf("%s", WIFI_USAGE);
    return 1;
}

} // namespace

inline void register_wifi() {
    const esp_console_cmd_t cmd = {
        .command = "wifi",
        .help = "Manage Wi-Fi: \"status\", \"scan\" for networks, \"connect\" <ssid> [password] (saved for the "
                "next boot), \"disconnect\", \"forget\" the saved network",
        .hint = "<status|scan|connect|disconnect|forget> [ssid] [password]",
        .func = &cli_cmd_wifi,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    register_command(cmd);
}

} // namespace Feature::CLI::Commands
