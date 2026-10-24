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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
// Station settings, loaded from NVS in init() and changed through apply_settings() (web UI
// Settings -> Network, CLI "wifi connect"). Empty SSID = nothing configured yet.
inline char sta_ssid[33] = {};
inline char sta_password[65] = {};

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
    if (sta_ssid[0] != '\0') {
        ESP_LOGW(TAG, "Could not join \"%s\" -- started fallback AP \"%s\" with captive portal at "
                 IPSTR " (retrying \"%s\" every %ds in the background)",
                 sta_ssid, CONFIG_WIFI_CAPTIVE_PORTAL_SSID, IP2STR(&ip_info.ip),
                 sta_ssid, CONFIG_WIFI_CAPTIVE_PORTAL_RETRY_INTERVAL_SEC);
    } else {
        ESP_LOGW(TAG, "No Wi-Fi network configured -- started hotspot \"%s\" with captive portal at " IPSTR
                 " (set the network in Settings -> Network)",
                 CONFIG_WIFI_CAPTIVE_PORTAL_SSID, IP2STR(&ip_info.ip));
    }
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
    if (!connected && sta_ssid[0] != '\0') {
        retry_count = 0;
        esp_wifi_connect();
    }
}
#endif // CONFIG_APP_ENABLE_WIFI_CAPTIVE_PORTAL

inline void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (sta_ssid[0] != '\0') {
            esp_wifi_connect();
        }
#if defined(CONFIG_APP_ENABLE_WIFI_CAPTIVE_PORTAL)
        else {
            start_captive_portal(); // nothing configured yet: open the setup hotspot right away
        }
#endif
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

// Station credentials saved in NVS (web UI Settings -> Network, CLI "wifi connect"). There is no
// build-time SSID/password: without saved credentials the device opens the setup hotspot.
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

/** @brief Forget the saved credentials; the next boot opens the setup hotspot again. */
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

// Asynchronous scan for the web UI: scan_start() returns at once, a small task runs the blocking
// scan() above, scan_get() reports the result.
enum class ScanState { Idle, Running, Done, Failed };
inline ScanResult async_scan_results[20];
inline int async_scan_count = 0;
inline volatile ScanState async_scan_state = ScanState::Idle;

inline void async_scan_task(void*) {
    const int n = scan(async_scan_results, 20);
    async_scan_count = n > 0 ? n : 0;
    async_scan_state = n >= 0 ? ScanState::Done : ScanState::Failed;
    vTaskDelete(nullptr);
}

/** @brief Start a background scan; false if one is already running or the task can't start. */
inline bool scan_start() {
    if (async_scan_state == ScanState::Running) return false;
    async_scan_state = ScanState::Running;
    if (xTaskCreate(async_scan_task, "wifi_scan", 4096, nullptr, 4, nullptr) != pdPASS) {
        async_scan_state = ScanState::Failed;
        return false;
    }
    return true;
}

inline ScanState scan_state() {
    return async_scan_state;
}

/**
 * @brief Save and apply new station settings (Settings -> Network). An empty `ssid` clears the
 *        saved network (the hotspot opens); `password` may be null/empty for an open network.
 *        Safe to call from any task; the (re)connect happens asynchronously.
 */
inline esp_err_t apply_settings(const char* ssid, const char* password) {
    if (ssid == nullptr) ssid = "";
    if (ssid[0] == '\0') {
        clear_saved_credentials();
        sta_ssid[0] = '\0';
        sta_password[0] = '\0';
    } else {
        const esp_err_t err = save_credentials(ssid, password != nullptr ? password : "");
        if (err != ESP_OK) return err;
        std::strncpy(sta_ssid, ssid, sizeof(sta_ssid) - 1);
        sta_ssid[sizeof(sta_ssid) - 1] = '\0';
        std::strncpy(sta_password, password != nullptr ? password : "", sizeof(sta_password) - 1);
        sta_password[sizeof(sta_password) - 1] = '\0';
    }

    if (sta_ssid[0] == '\0') {
        esp_wifi_disconnect();
#if defined(CONFIG_APP_ENABLE_WIFI_CAPTIVE_PORTAL)
        start_captive_portal();
#endif
        return ESP_OK;
    }
    return connect(sta_ssid, sta_password);
}

/** @brief The saved station SSID / password (empty = none configured). */
inline const char* configured_ssid() { return sta_ssid; }
inline const char* configured_password() { return sta_password; }

/**
 * @brief Initialize Wi-Fi station mode over the ESP-Hosted link (ESP32-C6 co-processor).
 *        Connects to the network saved in NVS; with none saved (or after WIFI_MAXIMUM_RETRY
 *        failed attempts) the captive-portal hotspot opens instead.
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

    // Saved credentials (Settings -> Network / CLI); nothing saved = no network configured.
    if (!load_saved_credentials(sta_ssid, sizeof(sta_ssid), sta_password, sizeof(sta_password))) {
        sta_ssid[0] = '\0';
        sta_password[0] = '\0';
    }
    if (sta_ssid[0] != '\0') {
        wifi_config_t wifi_config{};
        // Fields need no terminator at full length (32-char SSID / 64-char PSK are valid).
        std::memcpy(wifi_config.sta.ssid, sta_ssid, strnlen(sta_ssid, sizeof(wifi_config.sta.ssid)));
        std::memcpy(wifi_config.sta.password, sta_password, strnlen(sta_password, sizeof(wifi_config.sta.password)));
        wifi_config.sta.threshold.authmode = strlen(sta_password) == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    }

    err = esp_wifi_start();
    if (err != ESP_OK) return err;

    if (sta_ssid[0] != '\0') {
        ESP_LOGI(TAG, "Wi-Fi initialized, connecting to SSID \"%s\"", sta_ssid);
    } else {
        ESP_LOGI(TAG, "Wi-Fi initialized, no network configured yet");
    }
    return ESP_OK;
}

} // namespace Feature::WiFi
