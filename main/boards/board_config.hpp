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

// The C6 SDIO pins belong to the esp_hosted component, so Kconfig can only seed them for a
// fresh sdkconfig (see the end of main/Kconfig.projbuild). A value left over from another
// board would silently kill Wi-Fi/BLE ("sdmmc_init_ocr ... 0x107"), so fail the build instead.
#if !defined(CONFIG_BOARD_CUSTOM_P4) && defined(CONFIG_ESP_HOSTED_HOST_SDIO_PIN_CLK)
#define BOARD_CHECK_HOSTED_PIN(cfg, pin) \
    static_assert(cfg == static_cast<int>(CurrentBoard::pin), \
                  "menuconfig -> Component config -> ESP-Hosted: " #cfg " does not match this board (" #pin ")")
BOARD_CHECK_HOSTED_PIN(CONFIG_ESP_HOSTED_HOST_SDIO_PIN_CLK, PIN_SDIO_CLK);
BOARD_CHECK_HOSTED_PIN(CONFIG_ESP_HOSTED_HOST_SDIO_PIN_CMD, PIN_SDIO_CMD);
BOARD_CHECK_HOSTED_PIN(CONFIG_ESP_HOSTED_HOST_SDIO_PIN_D0, PIN_SDIO_D0);
#if defined(CONFIG_ESP_HOSTED_HOST_SDIO_PIN_D3)
BOARD_CHECK_HOSTED_PIN(CONFIG_ESP_HOSTED_HOST_SDIO_PIN_D1, PIN_SDIO_D1);
BOARD_CHECK_HOSTED_PIN(CONFIG_ESP_HOSTED_HOST_SDIO_PIN_D2, PIN_SDIO_D2);
BOARD_CHECK_HOSTED_PIN(CONFIG_ESP_HOSTED_HOST_SDIO_PIN_D3, PIN_SDIO_D3);
#endif
BOARD_CHECK_HOSTED_PIN(CONFIG_ESP_HOSTED_HOST_RESET_GPIO, HOSTED_RESET_GPIO);
#undef BOARD_CHECK_HOSTED_PIN
#endif
