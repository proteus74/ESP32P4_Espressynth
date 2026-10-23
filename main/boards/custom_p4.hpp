#pragma once
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "boards/reserved_pin.hpp"

namespace Board::Custom {

constexpr const char* BOARD_NAME = CONFIG_CUSTOM_BOARD_NAME;

// Display Specifications
constexpr int DISPLAY_WIDTH       = CONFIG_APP_DISPLAY_WIDTH;
constexpr int DISPLAY_HEIGHT      = CONFIG_APP_DISPLAY_HEIGHT;
constexpr int DISPLAY_NATIVE_WIDTH  = CONFIG_APP_DISPLAY_WIDTH;
constexpr int DISPLAY_NATIVE_HEIGHT = CONFIG_APP_DISPLAY_HEIGHT;
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

// Audio Defaults
constexpr gpio_num_t PIN_I2S_MCLK  = GPIO_NUM_NC;
constexpr gpio_num_t PIN_I2S_BCLK  = GPIO_NUM_NC;
constexpr gpio_num_t PIN_I2S_DOUT  = GPIO_NUM_NC;
constexpr gpio_num_t PIN_I2S_DIN   = GPIO_NUM_NC;
constexpr gpio_num_t PIN_I2S_LRCK  = GPIO_NUM_NC;
constexpr gpio_num_t PIN_AUDIO_PA  = GPIO_NUM_NC;

// TF Card: ESP32-P4 SDMMC slot 0 IOMUX pins (the only pins slot 0 can use)
constexpr gpio_num_t PIN_SDMMC_CLK = GPIO_NUM_43;
constexpr gpio_num_t PIN_SDMMC_CMD = GPIO_NUM_44;
constexpr gpio_num_t PIN_SDMMC_D0  = GPIO_NUM_39;
constexpr gpio_num_t PIN_SDMMC_D1  = GPIO_NUM_40;
constexpr gpio_num_t PIN_SDMMC_D2  = GPIO_NUM_41;
constexpr gpio_num_t PIN_SDMMC_D3  = GPIO_NUM_42;
constexpr gpio_num_t PIN_SDMMC_PWR_EN = GPIO_NUM_NC;
constexpr int        SDMMC_LDO_CHANNEL = CONFIG_SDMMC_LDO_CHANNEL;

// Every named pin above that's physically wired on this board (GPIO_NUM_NC entries are
// harmless - filtered out by conflict checks). Used to catch a user-chosen pin (e.g. the
// generic WM8960 codec's I2S/button pins) colliding with something already in use here.
constexpr ReservedPin RESERVED_PINS[] = {
    BOARD_PIN(PIN_TOUCH_RST), BOARD_PIN(PIN_TOUCH_INT),
    BOARD_PIN(PIN_LCD_RST), BOARD_PIN(PIN_BACKLIGHT),
    BOARD_PIN(PIN_I2S_MCLK), BOARD_PIN(PIN_I2S_BCLK), BOARD_PIN(PIN_I2S_DOUT), BOARD_PIN(PIN_I2S_DIN), BOARD_PIN(PIN_I2S_LRCK), BOARD_PIN(PIN_AUDIO_PA),
};

} // namespace Board::Custom
