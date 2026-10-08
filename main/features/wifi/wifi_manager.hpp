#pragma once

#include "boards/board_config.hpp"
#include <cstring>
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "nvs.h"
#include "nvs_flash.h"

#if defined(CONFIG_APP_ENABLE_WIFI_CAPTIVE_PORTAL)
    #include "dns_server.h"
    #include "esp_timer.h"
#endif

namespace Feature::WiFi {

namespace {
constexpr const char* TAG = "FeatureWiFi";
inline esp_netif_t* sta_netif = nullptr;
inline int retry_count = 0;
inline bool connected = false;

#if defined(CONFIG_APP_ENABLE_WIFI_CAPTIVE_PORTAL)
inline esp_netif_t* ap_netif = nullptr;
inline dns_server_handle_t dns_handle = nullptr;
inline esp_timer_handle_t retry_timer = nullptr;
inline bool captive_portal_active = false;

// Starts the fallback SoftAP + DNS-redirect-everything captive portal alongside the
// still-retrying station, and arms a periodic background reconnect attempt so the
// device rejoins WIFI_SSID on its own (no reboot needed) once it's reachable again.
inline void start_captive_portal() {
    if (captive_portal_active) return;

    // STA needs no AP netif. Allocate its DHCP/driver state only for fallback.
    ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif == nullptr) {
        ESP_LOGE(TAG, "Failed to create captive portal AP interface");
        return;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t ap_config{};
    std::strncpy(reinterpret_cast<char*>(ap_config.ap.ssid), CONFIG_WIFI_CAPTIVE_PORTAL_SSID,
                 sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen(CONFIG_WIFI_CAPTIVE_PORTAL_SSID);
    std::strncpy(reinterpret_cast<char*>(ap_config.ap.password), CONFIG_WIFI_CAPTIVE_PORTAL_PASSWORD,
                 sizeof(ap_config.ap.password) - 1);
    ap_config.ap.authmode = strlen(CONFIG_WIFI_CAPTIVE_PORTAL_PASSWORD) == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    ap_config.ap.max_connection = 4;
    // In APSTA mode the driver switches the AP to whatever channel the station ends up on
    // once connected; this is just the (required, must be 1-13) starting channel used while
    // the station isn't connected to anything yet.
    ap_config.ap.channel = 1;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    // Not DNS_SERVER_CONFIG_SINGLE(): that macro's designated initializer omits
    // dns_entry_pair_t::ip, which -Werror=missing-field-initializers rejects in C++.
    dns_server_config_t dns_config{};
    dns_config.num_of_entries = 1;
    dns_config.item[0].name = "*"; // answer every query
    dns_config.item[0].if_key = "WIFI_AP_DEF"; // with this device's own softAP IP
    dns_handle = start_dns_server(&dns_config);
    ESP_ERROR_CHECK(esp_timer_start_periodic(retry_timer,
        static_cast<uint64_t>(CONFIG_WIFI_CAPTIVE_PORTAL_RETRY_INTERVAL_SEC) * 1000000ULL));
    captive_portal_active = true;

    esp_netif_ip_info_t ip_info{};
    esp_netif_get_ip_info(ap_netif, &ip_info);
    ESP_LOGW(TAG, "Could not join \"%s\" -- started fallback AP \"%s\" with captive portal at "
             IPSTR " (retrying \"%s\" every %ds in the background)",
             CONFIG_WIFI_SSID, CONFIG_WIFI_CAPTIVE_PORTAL_SSID, IP2STR(&ip_info.ip),
             CONFIG_WIFI_SSID, CONFIG_WIFI_CAPTIVE_PORTAL_RETRY_INTERVAL_SEC);
}

// Tears the captive portal back down once the station has a real connection again.
inline void stop_captive_portal() {
    if (!captive_portal_active) return;

    ESP_ERROR_CHECK(esp_timer_stop(retry_timer));
    stop_dns_server(dns_handle);
    dns_handle = nullptr;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    esp_netif_destroy_default_wifi(ap_netif);
    ap_netif = nullptr;
    captive_portal_active = false;
    ESP_LOGI(TAG, "Wi-Fi reconnected -- fallback AP and captive portal closed");
}

inline void captive_portal_retry_timer_cb(void*) {
    if (!connected) {
        retry_count = 0;
        esp_wifi_connect();
    }
}
#endif // CONFIG_APP_ENABLE_WIFI_CAPTIVE_PORTAL

inline void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (strlen(CONFIG_WIFI_SSID) > 0) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        connected = false;
        const auto* event = static_cast<wifi_event_sta_disconnected_t*>(event_data);
        if (retry_count < CONFIG_WIFI_MAXIMUM_RETRY) {
            ++retry_count;
            ESP_LOGW(TAG, "Wi-Fi disconnected (reason=%d), retrying (%d/%d)...",
                     event->reason, retry_count, CONFIG_WIFI_MAXIMUM_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "Wi-Fi disconnected (reason=%d), giving up after %d retries",
                     event->reason, CONFIG_WIFI_MAXIMUM_RETRY);
#if defined(CONFIG_APP_ENABLE_WIFI_CAPTIVE_PORTAL)
            start_captive_portal();
#endif
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        ESP_LOGI(TAG, "Wi-Fi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        retry_count = 0;
        connected = true;
#if defined(CONFIG_APP_ENABLE_WIFI_CAPTIVE_PORTAL)
        stop_captive_portal();
#endif
    }
}
} // namespace

/**
 * @brief True once the station has an IP address.
 */
inline bool is_connected() {
    return connected;
}

#if defined(CONFIG_APP_ENABLE_WIFI_CAPTIVE_PORTAL)
/**
 * @brief True while the fallback AP + captive portal is up (station couldn't connect).
 *        main/features/web/web_server.hpp uses this to redirect unknown paths to "/"
 *        only while it's actually acting as a captive portal.
 */
inline bool is_captive_portal_active() {
    return captive_portal_active;
}
#endif

// Station credentials saved at runtime (CLI "wifi connect") in NVS. They take precedence over
// the build-time CONFIG_WIFI_SSID/CONFIG_WIFI_PASSWORD at boot; "wifi forget" clears them.
constexpr const char* CREDENTIALS_NVS_NAMESPACE = "wifi_cred";

/** @brief Load saved credentials; false if none are stored. Buffers must hold 33 / 65 bytes. */
inline bool load_saved_credentials(char* ssid, size_t ssid_len, char* password, size_t password_len) {
    nvs_handle_t nvs;
    if (nvs_open(CREDENTIALS_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;
    bool ok = nvs_get_str(nvs, "ssid", ssid, &ssid_len) == ESP_OK && ssid[0] != '\0';
    if (ok && nvs_get_str(nvs, "pass", password, &password_len) != ESP_OK) password[0] = '\0';
    nvs_close(nvs);
    return ok;
}

/** @brief Persist credentials so they survive a reboot (an empty password = open network). */
inline esp_err_t save_credentials(const char* ssid, const char* password) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CREDENTIALS_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_str(nvs, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(nvs, "pass", password != nullptr ? password : "");
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

/** @brief Forget the saved credentials; the next boot uses the build-time ones again. */
inline esp_err_t clear_saved_credentials() {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CREDENTIALS_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(nvs);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

/**
 * @brief Connect (or switch) to an access point. Overwrites the station config and
 *        (re)connects; safe to call whether or not a connection is already active.
 */
inline esp_err_t connect(const char* ssid, const char* password) {
    wifi_config_t wifi_config{};
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid), ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password != nullptr) {
        std::strncpy(reinterpret_cast<char*>(wifi_config.sta.password), password, sizeof(wifi_config.sta.password) - 1);
    }
    wifi_config.sta.threshold.authmode = (password == nullptr || password[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    const esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) return err;

    retry_count = 0;
    esp_wifi_disconnect(); // ok if not currently connected/connecting
    return esp_wifi_connect();
}

/**
 * @brief Disconnect from the current access point, if any.
 */
inline esp_err_t disconnect() {
    return esp_wifi_disconnect();
}

/**
 * @brief Get the station's current IP configuration. Returns ESP_ERR_INVALID_STATE if
 *        Wi-Fi hasn't been initialized yet.
 */
inline esp_err_t get_ip_info(esp_netif_ip_info_t& ip_info) {
    if (sta_netif == nullptr) return ESP_ERR_INVALID_STATE;
    return esp_netif_get_ip_info(sta_netif, &ip_info);
}

/**
 * @brief Get the connected access point's SSID and RSSI. Returns ESP_ERR_WIFI_NOT_CONNECT
 *        if not currently connected.
 */
inline esp_err_t get_ap_info(wifi_ap_record_t& ap_info) {
    return esp_wifi_sta_get_ap_info(&ap_info);
}

// One scanned access point, filled in by scan() below.
struct ScanResult {
    char ssid[33] = {};
    int8_t rssi = 0;
    wifi_auth_mode_t authmode = WIFI_AUTH_OPEN;
};

/**
 * @brief Blocking scan for nearby access points (takes a few seconds). Writes up to
 *        max_results entries into `out` and returns how many were written, or -1 on error.
 */
inline int scan(ScanResult* out, int max_results) {
    wifi_scan_config_t scan_config{};
    if (esp_wifi_scan_start(&scan_config, true) != ESP_OK) return -1;

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) return 0;

    constexpr uint16_t MAX_SCAN_RECORDS = 20;
    static wifi_ap_record_t ap_records[MAX_SCAN_RECORDS];
    uint16_t to_fetch = ap_count > MAX_SCAN_RECORDS ? MAX_SCAN_RECORDS : ap_count;
    if (esp_wifi_scan_get_ap_records(&to_fetch, ap_records) != ESP_OK) return -1;

    const int n = to_fetch > static_cast<uint16_t>(max_results) ? max_results : static_cast<int>(to_fetch);
    for (int i = 0; i < n; ++i) {
        std::strncpy(out[i].ssid, reinterpret_cast<const char*>(ap_records[i].ssid), sizeof(out[i].ssid) - 1);
        out[i].rssi = ap_records[i].rssi;
        out[i].authmode = ap_records[i].authmode;
    }
    return n;
}

/**
 * @brief Initialize Wi-Fi station mode over the ESP-Hosted link (ESP32-C6 co-processor).
 *        Auto-connects to CONFIG_WIFI_SSID/CONFIG_WIFI_PASSWORD if a SSID is configured.
 */
inline esp_err_t init() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    sta_netif = esp_netif_create_default_wifi_sta();
    if (sta_netif == nullptr) return ESP_FAIL;

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) return err;

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr));

#if defined(CONFIG_APP_ENABLE_WIFI_CAPTIVE_PORTAL)
    const esp_timer_create_args_t retry_timer_args = {
        .callback = &captive_portal_retry_timer_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_portal_retry",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&retry_timer_args, &retry_timer));
#endif

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // Saved (CLI) credentials win over the build-time ones.
    static char ssid[33] = CONFIG_WIFI_SSID;
    static char password[65] = CONFIG_WIFI_PASSWORD;
    if (load_saved_credentials(ssid, sizeof(ssid), password, sizeof(password))) {
        ESP_LOGI(TAG, "Using saved Wi-Fi credentials for \"%s\" (CLI: wifi forget to reset)", ssid);
    }
    if (strlen(ssid) > 0) {
        wifi_config_t wifi_config{};
        // Fields need no terminator at full length (32-char SSID / 64-char PSK are valid).
        std::memcpy(wifi_config.sta.ssid, ssid, strnlen(ssid, sizeof(wifi_config.sta.ssid)));
        std::memcpy(wifi_config.sta.password, password, strnlen(password, sizeof(wifi_config.sta.password)));
        wifi_config.sta.threshold.authmode = strlen(password) == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    }

    err = esp_wifi_start();
    if (err != ESP_OK) return err;

#if defined(CONFIG_BOARD_JC8012P4A1) || defined(CONFIG_WAVESHARE_P4_WIFI_DEV_KIT_BOARD)
    if (strlen(CONFIG_WIFI_SSID) > 0) {
        ESP_LOGI(TAG, "Wi-Fi initialized: ESP-Hosted SDIO(CLK:%d CMD:%d D0:%d), connecting to SSID \"%s\"",
                 CurrentBoard::PIN_SDIO_CLK, CurrentBoard::PIN_SDIO_CMD, CurrentBoard::PIN_SDIO_D0, CONFIG_WIFI_SSID);
    } else {
        ESP_LOGI(TAG, "Wi-Fi initialized: ESP-Hosted SDIO(CLK:%d CMD:%d D0:%d), no SSID configured",
                 CurrentBoard::PIN_SDIO_CLK, CurrentBoard::PIN_SDIO_CMD, CurrentBoard::PIN_SDIO_D0);
    }
#else
    if (strlen(CONFIG_WIFI_SSID) > 0) {
        ESP_LOGI(TAG, "Wi-Fi initialized: connecting to SSID \"%s\"", CONFIG_WIFI_SSID);
    } else {
        ESP_LOGI(TAG, "Wi-Fi initialized, no SSID configured");
    }
#endif
    return ESP_OK;
}

} // namespace Feature::WiFi
