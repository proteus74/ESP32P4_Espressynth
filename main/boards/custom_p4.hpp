#pragma once
#include "sdkconfig.h"
#include "driver/gpio.h"

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

// Every named pin above that's physically wired on this board (GPIO_NUM_NC entries are
// harmless - filtered out by conflict checks). Used to catch a user-chosen pin (e.g. the
// generic WM8960 codec's I2S/button pins) colliding with something already in use here.
constexpr gpio_num_t RESERVED_PINS[] = {
    PIN_TOUCH_RST, PIN_TOUCH_INT,
    PIN_LCD_RST, PIN_BACKLIGHT,
    PIN_I2S_MCLK, PIN_I2S_BCLK, PIN_I2S_DOUT, PIN_I2S_DIN, PIN_I2S_LRCK, PIN_AUDIO_PA,
};

} // namespace Board::Custom
