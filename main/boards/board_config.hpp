#pragma once
#include "sdkconfig.h"

#if defined(CONFIG_BOARD_JC8012P4A1)
    #include "jc8012p4a1.hpp"
    namespace CurrentBoard = Board::JC8012P4A1;
#elif defined(CONFIG_BOARD_JC1060P470C_7_INCH)
    #include "jc1060p470c_7inch.hpp"
    namespace CurrentBoard = Board::JC1060P470C7Inch;
#elif defined(CONFIG_WAVESHARE_P4_WIFI_DEV_KIT_BOARD)
    #include "waveshare_p4_wifi_dev_kit.hpp"
    namespace CurrentBoard = Board::WaveshareP4WiFiDevKit;
#elif defined(CONFIG_BOARD_CROWPANEL_5INCH)
    #include "crowpanel_5inch.hpp"
    namespace CurrentBoard = Board::CrowPanel5Inch;
#elif defined(CONFIG_BOARD_CUSTOM_P4)
    #include "custom_p4.hpp"
    namespace CurrentBoard = Board::Custom;
#else
    #error "No valid board target selected in Kconfig! Run idf.py menuconfig to choose a board."
#endif

// The web UI is the only way to edit presets, so at least one network interface is mandatory.
// Kconfig forces this for every real board; this catches a Custom board with neither a PHY nor
// an ESP32-C6 co-processor.
#if !defined(CONFIG_APP_ENABLE_ETHERNET) && !(defined(CONFIG_APP_ENABLE_WIFI) && defined(CONFIG_APP_ENABLE_HOSTED_COPROCESSOR))
    #error "No network interface: enable Ethernet (board needs an Ethernet PHY) or the ESP32-C6 co-processor with Wi-Fi (menuconfig -> Espressynth)."
#endif

// C6 SDIO/reset pins: app_startup.hpp hands CurrentBoard::PIN_SDIO_* and HOSTED_RESET_GPIO to
// esp_hosted at runtime; the CONFIG_ESP_HOSTED_HOST_SDIO_PIN_* Kconfig values are ignored
// (except on the Custom board).
