#pragma once
#include <atomic>

#include "app_core.hpp"
#include "boards/board_config.hpp"
#include "boards/board_init.hpp"
#include "features/io/i2c_device_scan.hpp"
#include "features/io/i2c_bus_manager.hpp"
#include "esp_log.h"

#if defined(CONFIG_APP_ENABLE_WIFI) || defined(CONFIG_APP_ENABLE_ETHERNET)
    #include "esp_event.h"
    #include "esp_netif.h"
#endif

#if defined(CONFIG_APP_ENABLE_WIFI) || defined(CONFIG_APP_ENABLE_BLUETOOTH)
    #include "esp_hosted.h"
#endif

#if defined(CONFIG_APP_ENABLE_LVGL)
    #include "features/display/display_lvgl.hpp"
#endif

#if defined(CONFIG_APP_ENABLE_SSD1306)
    #include "features/display/display_ssd1306.hpp"
#endif
#if defined(CONFIG_APP_DISPLAY_OUTPUT_SPI) && !defined(CONFIG_APP_ENABLE_LVGL)
    #include "features/display/display_spi.hpp"
#endif

#if defined(CONFIG_APP_ENABLE_WIFI)
    #include "features/wifi/wifi_manager.hpp"
#endif

#if defined(CONFIG_APP_ENABLE_BLUETOOTH)
    #include "features/ble/ble_manager.hpp"
#endif

#if defined(CONFIG_APP_ENABLE_COPROCESSOR_OTA)
    #include "features/hosted/hosted_ota.hpp"
#endif

#if defined(CONFIG_APP_ENABLE_ETHERNET)
    #include "features/ethernet/ethernet_manager.hpp"
#endif

#if defined(CONFIG_APP_ENABLE_USB_HOST)
    #include "features/usb/host/usb_host_manager.hpp"
#endif

#if defined(CONFIG_APP_ENABLE_USB_DEVICE)
    #include "features/usb/device/usb_device_manager.hpp"
#endif

#if defined(CONFIG_ONBOARD_AUDIO_ES8311)
    #include "features/audio/es8311_manager.hpp"
#endif

#if defined(CONFIG_ONBOARD_AUDIO_WM8960)
    #include "features/audio/wm8960_manager.hpp"
#endif

#if defined(CONFIG_ONBOARD_AUDIO_NS4168)
    #include "features/audio/ns4168_manager.hpp"
#endif

#if defined(CONFIG_APP_ENABLE_SYNTH)
    #include "features/synth/synth_manager.hpp"
#if defined(CONFIG_APP_ENABLE_WEBSERVER)
    #include "features/synth/synth_web_api.hpp"
#endif
#endif

#if defined(CONFIG_APP_ENABLE_SDCARD)
    #include "features/sd_card/sd_card_manager.hpp"
#endif

#if defined(CONFIG_APP_ENABLE_WEBSERVER)
    #include "features/web/web_server.hpp"
#endif

#if defined(CONFIG_APP_ENABLE_LITTLEFS)
    #include "features/filesystem/littlefs_manager.hpp"
#endif

#if defined(CONFIG_APP_ENABLE_CLI)
    #include "cli/cli.hpp"
#endif

namespace App {

#if defined(CONFIG_APP_ENABLE_SDCARD)
inline void sd_card_init_task(void*) {
    // Feature::SDCard::init() already logs whatever went wrong (or didn't) at the
    // appropriate level itself -- no need to log again here.
    Feature::SDCard::init();
    vTaskDelete(nullptr);
}
#endif

#if defined(CONFIG_APP_ENABLE_ETHERNET) || defined(CONFIG_APP_ENABLE_WIFI) || defined(CONFIG_APP_ENABLE_BLUETOOTH)
// All network bring-up (Ethernet PHY/EMAC, and Wi-Fi/BLE via the ESP-Hosted co-processor)
// runs here, off the main init task, since none of it needs to finish before the rest of
// App::init() (USB, audio, CLI, ...) proceeds.
inline void network_init_task(void*) {
#if defined(CONFIG_APP_ENABLE_ETHERNET)
    const esp_err_t ethernet_err = Feature::Ethernet::init();
    if (ethernet_err != ESP_OK) {
        ESP_LOGW("MainApp", "Ethernet unavailable: %s", esp_err_to_name(ethernet_err));
    }
#endif

#if defined(CONFIG_APP_ENABLE_WIFI) || defined(CONFIG_APP_ENABLE_BLUETOOTH)
    const esp_err_t hosted_init_err = static_cast<esp_err_t>(esp_hosted_init());
    if (hosted_init_err != ESP_OK) {
        ESP_LOGW("MainApp", "ESP-Hosted initialization failed: %s", esp_err_to_name(hosted_init_err));
        vTaskDelete(nullptr);
        return;
    }

#if defined(CONFIG_APP_ENABLE_BLUETOOTH)
    const esp_err_t ble_err = Feature::BLE::init();
    if (ble_err != ESP_OK) {
        ESP_LOGW("MainApp", "Bluetooth LE unavailable: %s", esp_err_to_name(ble_err));
    }
#else
    // Feature::BLE::init() normally performs this handshake; with BLE off nothing else would
    // reset the C6 and open the SDIO link, so Wi-Fi init would time out.
    if (esp_hosted_connect_to_slave() != 0) {
        ESP_LOGW("MainApp", "ESP-Hosted co-processor link failed");
    }
#endif

#if defined(CONFIG_APP_ENABLE_COPROCESSOR_OTA)
    const esp_err_t coprocessor_ota_err = Feature::Hosted::OTA::update_coprocessor_if_needed();
    if (coprocessor_ota_err != ESP_OK) {
        ESP_LOGW("MainApp", "Co-processor OTA check skipped: %s", esp_err_to_name(coprocessor_ota_err));
    }
#endif

#if defined(CONFIG_APP_ENABLE_WIFI)
    const esp_err_t wifi_err = Feature::WiFi::init();
    if (wifi_err != ESP_OK) {
        ESP_LOGW("MainApp", "Wi-Fi unavailable: %s", esp_err_to_name(wifi_err));
    }
#endif
#if defined(CONFIG_APP_ENABLE_LVGL) && defined(CONFIG_APP_DISPLAY_OUTPUT_RGB)
    Feature::LVGL::resync_display();
#endif
#endif

    vTaskDelete(nullptr);
}
#endif

inline void init() {
    constexpr const char* TAG = "MainApp";

    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "Starting Application on Board: %s", CurrentBoard::BOARD_NAME);
    ESP_LOGI(TAG, "===========================================");

    // These two tags are vendored esp_hosted logging (main/idf_component.yml), not this
    // project's code, so their wording can't be trimmed - just dial them down to warnings
    // and errors only.
    esp_log_level_set("eh_init_evt", ESP_LOG_WARN);
    esp_log_level_set("eh_sdio", ESP_LOG_WARN);

#if defined(CONFIG_APP_ENABLE_SDCARD)
    // A card with no (or an unrecognized) FAT filesystem is an expected, routine case
    // here (see Feature::SDCard::init(), which already logs one clean INFO line for it),
    // not a fault -- silence this ESP-IDF component's own warning/error pair for that
    // same mount attempt so it doesn't look like something actually broke.
    esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);
#endif

    ESP_ERROR_CHECK(Board::init());

    ESP_LOGI(TAG, "Scanning for optional I2C devices (ADS1115/GP8413)...");
    Feature::IO::scan_configured_devices();

#if defined(CONFIG_APP_ADS1115_SUPPORT) || defined(CONFIG_APP_GP8413_SUPPORT)
    ESP_ERROR_CHECK(Feature::IO::i2c_bus_manager_init());
#endif

#if defined(CONFIG_APP_ENABLE_USB_DEVICE)
    ESP_LOGI(TAG, "Feature: USB 1.1 Device (TinyUSB) enabled -> Initializing...");
    ESP_ERROR_CHECK(Feature::USBDevice::init());
#endif

#if defined(CONFIG_APP_ENABLE_LITTLEFS)
    // Deliberately ahead of LVGL below (was the other way round before the boot splash
    // screen existed): Feature::LVGL::init() now reads its splash image from
    // /littlefs/system/splash.png (see display_lvgl.hpp), which needs this mount to already
    // be up. Each feature still checks its own Kconfig guard independently - either can stay
    // disabled without the other breaking, this only fixes their relative order when both
    // are on.
    ESP_LOGI(TAG, "Feature: LittleFS enabled -> Initializing...");
    const esp_err_t littlefs_err = Feature::LittleFS::init();
    if (littlefs_err != ESP_OK) {
        ESP_LOGW(TAG, "LittleFS unavailable: %s", esp_err_to_name(littlefs_err));
    }
#endif

#if defined(CONFIG_APP_DISPLAY_OUTPUT_SPI) && !defined(CONFIG_APP_ENABLE_LVGL) && CONFIG_APP_DISPLAY_SPI_PIN_CLK >= 0
    ESP_ERROR_CHECK(Feature::SPIDisplay::init());
#endif

#if defined(CONFIG_APP_ENABLE_LVGL)
    ESP_LOGI(TAG, "Feature: LVGL enabled -> Initializing...");
    ESP_ERROR_CHECK(Feature::LVGL::init());
#endif

#if defined(CONFIG_APP_ENABLE_SSD1306)
    const esp_err_t oled_err = Feature::SSD1306::init();
    if (oled_err != ESP_OK) {
        ESP_LOGW(TAG, "SSD1306 unavailable: %s", esp_err_to_name(oled_err));
    }
#endif

#if defined(CONFIG_APP_ENABLE_WIFI) || defined(CONFIG_APP_ENABLE_ETHERNET)
    // esp_netif_init() blocks until the lwip tcpip task is up and its mbox created, and is
    // safe to call redundantly (Feature::WiFi::init()/Feature::Ethernet::init() each call it
    // again and just get ESP_ERR_INVALID_STATE back). Doing it here, synchronously, before
    // network_init_task is even created avoids a race with Feature::Web::init() below: that
    // one runs on this task right after network_init_task is kicked off, and httpd_start()
    // needs a live tcpip task to open its listening socket -- without this, a Web Server
    // enabled on a board with no Ethernet PHY (only Wi-Fi, brought up asynchronously) could
    // call httpd_start() before esp_netif_init() ran on the network task, hitting
    // "assert failed: tcpip_send_msg_wait_sem ... (Invalid mbox)".
    const esp_err_t netif_err = esp_netif_init();
    if (netif_err != ESP_OK && netif_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "esp_netif_init() failed: %s", esp_err_to_name(netif_err));
    }
    const esp_err_t event_loop_err = esp_event_loop_create_default();
    if (event_loop_err != ESP_OK && event_loop_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "esp_event_loop_create_default() failed: %s", esp_err_to_name(event_loop_err));
    }
#endif

#if defined(CONFIG_APP_ENABLE_ETHERNET) || defined(CONFIG_APP_ENABLE_WIFI) || defined(CONFIG_APP_ENABLE_BLUETOOTH)
    ESP_LOGI(TAG, "Feature: Networking enabled -> Starting network init task...");
    if (xTaskCreatePinnedToCore(network_init_task, "network_init", 12288, nullptr, 6, nullptr,
                                 MAIN_CORE) != pdPASS) {
        ESP_LOGW(TAG, "Failed to start network initialization task");
    }
#endif

#if defined(CONFIG_APP_ENABLE_SDCARD)
    ESP_LOGI(TAG, "Feature: SD Card enabled -> Initializing...");
    if (xTaskCreatePinnedToCore(sd_card_init_task, "sd_card_init", 6144, nullptr, 5, nullptr,
                                 MAIN_CORE) != pdPASS) {
        ESP_LOGW(TAG, "Failed to start SD card initialization task");
    }
#endif

#if defined(CONFIG_APP_ENABLE_WEBSERVER)
    ESP_LOGI(TAG, "Feature: Web Server enabled -> Initializing...");
    const esp_err_t web_err = Feature::Web::begin();
    if (web_err != ESP_OK) {
        ESP_LOGW(TAG, "Web server unavailable: %s", esp_err_to_name(web_err));
    } else {
#if defined(CONFIG_APP_ENABLE_SYNTH)
        // Registers the synth engine's WS + REST routes on Feature::Web's server. Must run
        // between begin() and register_catch_all() - see both their comments.
        Feature::Synth::register_web_routes();
#endif
        Feature::Web::register_catch_all();
    }
#endif

#if defined(CONFIG_APP_ENABLE_USB_HOST)
    ESP_LOGI(TAG, "Feature: TinyUSB Host enabled -> Initializing...");
    ESP_ERROR_CHECK(Feature::USBHost::init());
#endif

#if defined(CONFIG_ONBOARD_AUDIO_ES8311)
    ESP_LOGI(TAG, "Feature: ES8311 Audio Codec enabled -> Initializing...");
    const esp_err_t audio_err = Feature::Audio::ES8311::init();
    if (audio_err != ESP_OK) {
        ESP_LOGW(TAG, "Audio codec unavailable: %s", esp_err_to_name(audio_err));
    }
#endif

#if defined(CONFIG_ONBOARD_AUDIO_WM8960)
    ESP_LOGI(TAG, "Feature: WM8960 Audio HAT enabled -> Initializing...");
    const esp_err_t wm8960_err = Feature::Audio::WM8960::init();
    if (wm8960_err != ESP_OK) {
        ESP_LOGW(TAG, "WM8960 codec unavailable: %s", esp_err_to_name(wm8960_err));
    }
#endif

#if defined(CONFIG_ONBOARD_AUDIO_NS4168)
    ESP_LOGI(TAG, "Feature: NS4168 amplifier enabled -> Initializing...");
    const esp_err_t ns4168_err = Feature::Audio::NS4168::init();
    if (ns4168_err != ESP_OK) {
        ESP_LOGW(TAG, "NS4168 amplifier unavailable: %s", esp_err_to_name(ns4168_err));
    }
#endif

#if defined(CONFIG_APP_ENABLE_SYNTH)
    // After the onboard codec(s) above (an open ES8311 handle and/or WM8960/NS4168 TX
    // handles) and LittleFS earlier (needs /littlefs mounted for presets/samples/soundfonts).
    ESP_LOGI(TAG, "Feature: Synth Engine enabled -> Initializing...");
    const esp_err_t synth_err = Feature::Synth::init();
    if (synth_err != ESP_OK) {
        ESP_LOGW(TAG, "Synth engine unavailable: %s", esp_err_to_name(synth_err));
    }
#endif

#if defined(CONFIG_APP_ENABLE_CLI)
    ESP_LOGI(TAG, "Feature: CLI enabled -> Initializing...");
    ESP_ERROR_CHECK(Feature::CLI::init());
#endif

#if defined(CONFIG_APP_ENABLE_LVGL)
    Feature::LVGL::finish_startup_screen();
#endif
#if defined(CONFIG_APP_ENABLE_SSD1306)
    Feature::SSD1306::finish_startup_screen();
#endif

    ESP_LOGI(TAG, "Application initialization complete. Running main loop...");
}

} // namespace App
