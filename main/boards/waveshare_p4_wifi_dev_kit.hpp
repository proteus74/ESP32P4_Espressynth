#pragma once
#include "sdkconfig.h"
#include "driver/gpio.h"
// https://docs.waveshare.com/ESP32-P4-WIFI6-DEV-KIT

// Repository: https://github.com/waveshareteam/ESP32-P4-Platform
namespace Board::WaveshareP4WiFiDevKit {

constexpr const char* BOARD_NAME = "Waveshare ESP32-P4-WIFI6-DEV-KIT";

// Display Specifications (MIPI DSI 2-lane, Supports Waveshare DSI displays e.g., 7-inch 1024x600 or 10.1-inch 1280x800)
constexpr int DISPLAY_WIDTH       = 1024;
constexpr int DISPLAY_HEIGHT      = 600;
constexpr int DISPLAY_NATIVE_WIDTH  = 1024;
constexpr int DISPLAY_NATIVE_HEIGHT = 600;
constexpr int DISPLAY_DSI_LANES   = 2;
constexpr int DISPLAY_REFRESH_HZ  = 60;
constexpr gpio_num_t PIN_LCD_RST   = GPIO_NUM_NC;
constexpr gpio_num_t PIN_BACKLIGHT  = GPIO_NUM_NC;
constexpr int DISPLAY_DSI_LDO_CHANNEL = 3;
constexpr int DISPLAY_DSI_LDO_VOLTAGE_MV = 2500;

// I2C Bus: see "I2C Subsystem" Kconfig (I2C0_PIN_SDA/SCL) - not board-fixed anymore.

// Touch Controller Defaults
constexpr gpio_num_t PIN_TOUCH_RST = GPIO_NUM_NC;
constexpr gpio_num_t PIN_TOUCH_INT = GPIO_NUM_NC;

// Audio Codec (ES8311 / NS4150B PA)
constexpr gpio_num_t PIN_I2S_MCLK  = GPIO_NUM_13;
constexpr gpio_num_t PIN_I2S_BCLK  = GPIO_NUM_12;
constexpr gpio_num_t PIN_I2S_DOUT  = GPIO_NUM_9;   // DSDIN on Codec
constexpr gpio_num_t PIN_I2S_DIN   = GPIO_NUM_11;  // ASDOUT on Codec
constexpr gpio_num_t PIN_I2S_LRCK  = GPIO_NUM_10;
constexpr gpio_num_t PIN_AUDIO_PA  = GPIO_NUM_53;  // PA_Ctrl (active high)

// ESP32-C6 Hosted Co-Processor (SDIO 4-bit Transport)
constexpr gpio_num_t PIN_SDIO_CLK  = GPIO_NUM_18;
constexpr gpio_num_t PIN_SDIO_CMD  = GPIO_NUM_19;
constexpr gpio_num_t PIN_SDIO_D0   = GPIO_NUM_14;
constexpr gpio_num_t PIN_SDIO_D1   = GPIO_NUM_15;
constexpr gpio_num_t PIN_SDIO_D2   = GPIO_NUM_16;
constexpr gpio_num_t PIN_SDIO_D3   = GPIO_NUM_17;
constexpr gpio_num_t PIN_C6_RESET  = GPIO_NUM_54;
constexpr gpio_num_t PIN_C6_WAKEUP = GPIO_NUM_6;

// TF Card Slot (SDMMC Slot 0, 4-line SDIO 3.0)
constexpr gpio_num_t PIN_SDMMC_CLK = GPIO_NUM_43;
constexpr gpio_num_t PIN_SDMMC_CMD = GPIO_NUM_44;
constexpr gpio_num_t PIN_SDMMC_D0  = GPIO_NUM_39;
constexpr gpio_num_t PIN_SDMMC_D1  = GPIO_NUM_40;
constexpr gpio_num_t PIN_SDMMC_D2  = GPIO_NUM_41;
constexpr gpio_num_t PIN_SDMMC_D3  = GPIO_NUM_42;
constexpr gpio_num_t PIN_SDMMC_PWR_EN = GPIO_NUM_45; // Active-low SD card power enable
constexpr int        SDMMC_LDO_CHANNEL = CONFIG_SDMMC_LDO_CHANNEL; // ESP_LDO_VO4 by default

// Ethernet (RMII EMAC + IP101/LAN8720 PHY)
constexpr bool       HAS_ETHERNET         = true;
constexpr gpio_num_t PIN_ETH_MDC          = GPIO_NUM_31;
constexpr gpio_num_t PIN_ETH_MDIO         = GPIO_NUM_52;
constexpr gpio_num_t PIN_ETH_PHY_RST      = GPIO_NUM_51;
constexpr int        ETH_PHY_ADDR         = 1;
// RMII data/clock pins - hardcoded directly in ethernet_manager.hpp's emac_config (not
// Kconfig-selectable), so listed here as fixed constants purely so RESERVED_PINS below can
// include them.
constexpr gpio_num_t PIN_ETH_RMII_CLK     = GPIO_NUM_50;
constexpr gpio_num_t PIN_ETH_RMII_TX_EN   = GPIO_NUM_49;
constexpr gpio_num_t PIN_ETH_RMII_TXD0    = GPIO_NUM_34;
constexpr gpio_num_t PIN_ETH_RMII_TXD1    = GPIO_NUM_35;
constexpr gpio_num_t PIN_ETH_RMII_CRS_DV  = GPIO_NUM_28;
constexpr gpio_num_t PIN_ETH_RMII_RXD0    = GPIO_NUM_29;
constexpr gpio_num_t PIN_ETH_RMII_RXD1    = GPIO_NUM_30;

// USB Host & 4-Port USB Hub (FE1.1S USB 2.0 High-Speed Hub)
constexpr bool       HAS_USB_HOST         = true;
constexpr bool       HAS_4PORT_USB_HUB    = true;
constexpr gpio_num_t PIN_USB_HUB_RST      = GPIO_NUM_NC;
constexpr gpio_num_t PIN_USB_HOST_PWR_EN  = GPIO_NUM_NC; // Power control for USB-A ports

// Every named pin above that's physically wired on this board (GPIO_NUM_NC entries are
// harmless - filtered out by conflict checks). Used to catch a user-chosen pin (e.g. the
// generic WM8960 codec's I2S/button pins) colliding with something already in use here.
// PIN_AUDIO_PA and PIN_SDMMC_PWR_EN are only listed while their owning feature (ES8311 /
// the SD card slot) is actually enabled: with the feature off, nothing in this project ever
// configures that GPIO, so it's genuinely free for another use (e.g. a WM8960 codec wired
// to this board's 40-pin header, which reuses both pins at the standard RPi HAT positions).
constexpr gpio_num_t RESERVED_PINS[] = {
    PIN_TOUCH_RST, PIN_TOUCH_INT,
    PIN_LCD_RST, PIN_BACKLIGHT,
    PIN_I2S_MCLK, PIN_I2S_BCLK, PIN_I2S_DOUT, PIN_I2S_DIN, PIN_I2S_LRCK,
#if defined(CONFIG_ONBOARD_AUDIO_ES8311)
    PIN_AUDIO_PA,
#endif
    PIN_SDIO_CLK, PIN_SDIO_CMD, PIN_SDIO_D0, PIN_SDIO_D1, PIN_SDIO_D2, PIN_SDIO_D3,
    PIN_C6_RESET, PIN_C6_WAKEUP,
    PIN_SDMMC_CLK, PIN_SDMMC_CMD, PIN_SDMMC_D0, PIN_SDMMC_D1, PIN_SDMMC_D2, PIN_SDMMC_D3,
#if defined(CONFIG_APP_ENABLE_SDCARD)
    PIN_SDMMC_PWR_EN,
#endif
    PIN_ETH_MDC, PIN_ETH_MDIO, PIN_ETH_PHY_RST,
    PIN_ETH_RMII_CLK, PIN_ETH_RMII_TX_EN, PIN_ETH_RMII_TXD0, PIN_ETH_RMII_TXD1,
    PIN_ETH_RMII_CRS_DV, PIN_ETH_RMII_RXD0, PIN_ETH_RMII_RXD1,
    PIN_USB_HUB_RST, PIN_USB_HOST_PWR_EN,
};

} // namespace Board::WaveshareP4WiFiDevKit
