#pragma once
#include "sdkconfig.h"
#include "driver/gpio.h"

namespace Board::JC8012P4A1 {

constexpr const char* BOARD_NAME = "JC8012P4A1 (ESP32-P4)";

// Display Specifications (MIPI DSI - JD9365)
constexpr int DISPLAY_WIDTH       = 1280;
constexpr int DISPLAY_HEIGHT      = 800;
// The panel is physically 800x1280 portrait; the application uses landscape.
constexpr int DISPLAY_NATIVE_WIDTH  = 800;
constexpr int DISPLAY_NATIVE_HEIGHT = 1280;
constexpr int DISPLAY_DSI_LANES   = 2;
constexpr int DISPLAY_REFRESH_HZ  = 60;
constexpr gpio_num_t PIN_LCD_RST   = GPIO_NUM_27;
constexpr gpio_num_t PIN_BACKLIGHT  = GPIO_NUM_23;
constexpr int DISPLAY_DSI_LDO_CHANNEL = 3;
constexpr int DISPLAY_DSI_LDO_VOLTAGE_MV = 2500;

// I2C Bus: see "I2C Subsystem" Kconfig (I2C0_PIN_SDA/SCL) - not board-fixed anymore.
// (Shared between Touch GSL3680, Audio ES8311, RTC RX8025T.)

// Touch Controller (GSL3680)
constexpr gpio_num_t PIN_TOUCH_RST = GPIO_NUM_22;
constexpr gpio_num_t PIN_TOUCH_INT = GPIO_NUM_21;

// Audio Codec (ES8311 / I2S)
constexpr gpio_num_t PIN_I2S_MCLK  = GPIO_NUM_13;
constexpr gpio_num_t PIN_I2S_BCLK  = GPIO_NUM_12;
constexpr gpio_num_t PIN_I2S_DOUT  = GPIO_NUM_9;
constexpr gpio_num_t PIN_I2S_DIN   = GPIO_NUM_11;
constexpr gpio_num_t PIN_I2S_LRCK  = GPIO_NUM_10;
constexpr gpio_num_t PIN_AUDIO_PA  = GPIO_NUM_20;

// ESP32-C6 Hosted Co-Processor (SDIO 4-bit Transport)
constexpr gpio_num_t PIN_SDIO_CLK  = GPIO_NUM_18;
constexpr gpio_num_t PIN_SDIO_CMD  = GPIO_NUM_19;
constexpr gpio_num_t PIN_SDIO_D0   = GPIO_NUM_14;
constexpr gpio_num_t PIN_SDIO_D1   = GPIO_NUM_15;
constexpr gpio_num_t PIN_SDIO_D2   = GPIO_NUM_16;
constexpr gpio_num_t PIN_SDIO_D3   = GPIO_NUM_17;
constexpr gpio_num_t PIN_C6_RESET  = GPIO_NUM_54;
constexpr gpio_num_t PIN_C6_WAKEUP = GPIO_NUM_6;

// TF Card Slot (SDMMC Slot 0)
constexpr gpio_num_t PIN_SDMMC_CLK = GPIO_NUM_43;
constexpr gpio_num_t PIN_SDMMC_CMD = GPIO_NUM_44;
constexpr gpio_num_t PIN_SDMMC_D0  = GPIO_NUM_39;
constexpr gpio_num_t PIN_SDMMC_D1  = GPIO_NUM_40;
constexpr gpio_num_t PIN_SDMMC_D2  = GPIO_NUM_41;
constexpr gpio_num_t PIN_SDMMC_D3  = GPIO_NUM_42;
constexpr gpio_num_t PIN_SDMMC_PWR_EN = GPIO_NUM_45; // Active-low SD card power enable
constexpr int        SDMMC_LDO_CHANNEL = CONFIG_SDMMC_LDO_CHANNEL;

// Battery ADC
constexpr gpio_num_t PIN_BAT_ADC   = GPIO_NUM_52;

// Ethernet (Not fitted on JC8012P4A1)
constexpr bool       HAS_ETHERNET         = false;
constexpr gpio_num_t PIN_ETH_MDC          = GPIO_NUM_NC;
constexpr gpio_num_t PIN_ETH_MDIO         = GPIO_NUM_NC;

// USB Host
constexpr bool       HAS_USB_HOST         = true;
constexpr bool       HAS_4PORT_USB_HUB    = false;
constexpr gpio_num_t PIN_USB_HUB_RST      = GPIO_NUM_NC;
constexpr gpio_num_t PIN_USB_HOST_PWR_EN  = GPIO_NUM_NC;

// Every named pin above that's physically wired on this board (GPIO_NUM_NC entries are
// harmless - filtered out by conflict checks). Used to catch a user-chosen pin (e.g. the
// generic WM8960 codec's I2S/button pins) colliding with something already in use here.
// PIN_AUDIO_PA and PIN_SDMMC_PWR_EN are only listed while their owning feature (ES8311 /
// the SD card slot) is actually enabled: with the feature off, nothing in this project ever
// configures that GPIO, so it's genuinely free for another use.
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
    PIN_BAT_ADC,
    PIN_ETH_MDC, PIN_ETH_MDIO,
    PIN_USB_HUB_RST, PIN_USB_HOST_PWR_EN,
};

} // namespace Board::JC8012P4A1
