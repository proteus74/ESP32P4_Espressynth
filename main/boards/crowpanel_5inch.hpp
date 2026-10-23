#pragma once
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "boards/reserved_pin.hpp"
// Elecrow CrowPanel Advanced 5" ESP32-P4 HMI (800x480 RGB IPS, GT911, NS4168, ESP32-C6 hosted).
// Pinout from Elecrow's wiki and the factory source
// (github.com/Elecrow-RD/-CrowPanel-Advanced-5inch-ESP32-P4-HMI-AI-Display-800x480-IPS-Touch-Screen,
// factory_sourcecode/V1.0/ESP32-P4-Advance-5inch-lvgl: bsp_display.h, bsp_stc8h1kxx.h, sdkconfig.defaults).

// The CrowPanel's only console port is the CH340 USB-UART on UART0 (COM port). The P4's own
// USB-Serial-JTAG would sit on GPIO24/25, which carry the PDM microphone here - with that console
// selected the firmware boots silently (no logs, no CLI). Usually a leftover from switching an
// existing sdkconfig over from another board.
#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
#error "CrowPanel 5\": set menuconfig -> Component config -> ESP-STDIO -> Channel for console output -> Default: UART0"
#endif

namespace Board::CrowPanel5Inch {

constexpr const char* BOARD_NAME = "Elecrow CrowPanel Advanced 5\" (ESP32-P4)";

// Display: 16-bit parallel RGB565, natively landscape.
constexpr int DISPLAY_WIDTH         = CONFIG_APP_DISPLAY_WIDTH;
constexpr int DISPLAY_HEIGHT        = CONFIG_APP_DISPLAY_HEIGHT;
constexpr int DISPLAY_NATIVE_WIDTH  = CONFIG_APP_DISPLAY_WIDTH;
constexpr int DISPLAY_NATIVE_HEIGHT = CONFIG_APP_DISPLAY_HEIGHT;
constexpr int DISPLAY_REFRESH_HZ    = 28;
constexpr int DISPLAY_PIXEL_CLOCK_HZ = 12 * 1000 * 1000; // Reduce PSRAM scan-out demand under synth/UI load.
constexpr int DISPLAY_HSYNC_PULSE_WIDTH = 4;
constexpr int DISPLAY_HSYNC_BACK_PORCH  = 8;
constexpr int DISPLAY_HSYNC_FRONT_PORCH = 8;
constexpr int DISPLAY_VSYNC_PULSE_WIDTH = 4;
constexpr int DISPLAY_VSYNC_BACK_PORCH  = 16;
constexpr int DISPLAY_VSYNC_FRONT_PORCH = 16;
constexpr gpio_num_t PIN_RGB_HSYNC = GPIO_NUM_40;
constexpr gpio_num_t PIN_RGB_VSYNC = GPIO_NUM_41;
constexpr gpio_num_t PIN_RGB_DE    = GPIO_NUM_2;
constexpr gpio_num_t PIN_RGB_PCLK  = GPIO_NUM_3;
// Data bits D0..D15 (B0-B4, G0-G5, R0-R4).
constexpr gpio_num_t PIN_RGB_DATA[16] = {
    GPIO_NUM_8,  GPIO_NUM_7,  GPIO_NUM_6,  GPIO_NUM_5,  GPIO_NUM_4,
    GPIO_NUM_14, GPIO_NUM_13, GPIO_NUM_12, GPIO_NUM_11, GPIO_NUM_10, GPIO_NUM_9,
    GPIO_NUM_19, GPIO_NUM_18, GPIO_NUM_17, GPIO_NUM_16, GPIO_NUM_15,
};
// No direct panel reset or backlight GPIO: both go through the STC8 co-MCU (see below).
constexpr gpio_num_t PIN_LCD_RST   = GPIO_NUM_NC;
constexpr gpio_num_t PIN_BACKLIGHT = GPIO_NUM_NC;

// I2C bus (GT911 + STC8): see "I2C Subsystem" Kconfig (I2C0_PIN_SDA/SCL default 45/46).
constexpr gpio_num_t PIN_TOUCH_RST = GPIO_NUM_36; // also reachable via STC8 GPIO out 0
constexpr gpio_num_t PIN_TOUCH_INT = GPIO_NUM_42;

// STC8H1K co-MCU (I2C slave): register map from bsp_stc8h1kxx.h
constexpr uint8_t STC8_I2C_ADDR          = CONFIG_CROWPANEL_STC8_I2C_ADDR;
constexpr uint8_t STC8_REG_BATTERY       = 0x00;
constexpr uint8_t STC8_REG_GET_GPIO      = 0x10;
constexpr uint8_t STC8_REG_SET_GPIO      = 0x18;
constexpr uint8_t STC8_REG_SET_PWM       = 0x20;
constexpr uint8_t STC8_GPIO_OUT_TP_RST        = 0;
constexpr uint8_t STC8_GPIO_OUT_CSI_RST       = 1;
constexpr uint8_t STC8_GPIO_OUT_AUDIO_SD      = 2;
constexpr uint8_t STC8_GPIO_OUT_LCD_BL_POWER  = 3;
constexpr uint8_t STC8_PWM_LCD_BL             = 0;

// Audio: NS4168 class-D amp (I2S only, no MCLK) + PDM microphone.
constexpr gpio_num_t PIN_I2S_MCLK  = GPIO_NUM_NC;
constexpr gpio_num_t PIN_I2S_BCLK  = GPIO_NUM_22;
constexpr gpio_num_t PIN_I2S_DOUT  = GPIO_NUM_23;
constexpr gpio_num_t PIN_I2S_DIN   = GPIO_NUM_NC;
constexpr gpio_num_t PIN_I2S_LRCK  = GPIO_NUM_21;
constexpr gpio_num_t PIN_AUDIO_PA  = GPIO_NUM_NC; // amp shutdown is an STC8 output
constexpr gpio_num_t PIN_PDM_MIC_CLK = GPIO_NUM_24;
constexpr gpio_num_t PIN_PDM_MIC_DIN = GPIO_NUM_25;

// ESP32-C6 Hosted Co-Processor (SDIO 4-bit via GPIO matrix, RGB panel occupies the IOMUX pins)
constexpr gpio_num_t PIN_SDIO_CLK  = GPIO_NUM_53;
constexpr gpio_num_t PIN_SDIO_CMD  = GPIO_NUM_54;
constexpr gpio_num_t PIN_SDIO_D0   = GPIO_NUM_52;
constexpr gpio_num_t PIN_SDIO_D1   = GPIO_NUM_51;
constexpr gpio_num_t PIN_SDIO_D2   = GPIO_NUM_50;
constexpr gpio_num_t PIN_SDIO_D3   = GPIO_NUM_49;
// Driven by the ESP-Hosted driver itself (CONFIG_ESP_HOSTED_HOST_RESET_GPIO), not by board_init.
constexpr gpio_num_t PIN_C6_RESET  = GPIO_NUM_NC;
constexpr gpio_num_t PIN_C6_WAKEUP = GPIO_NUM_NC;
// C6 reset line, owned by the ESP-Hosted driver (checked in board_config.hpp).
constexpr gpio_num_t HOSTED_RESET_GPIO = GPIO_NUM_20;

// TF Card Slot (SDMMC Slot 0, 1-bit only: D1-D3 pins double as RGB HSYNC/VSYNC and touch INT)
constexpr gpio_num_t PIN_SDMMC_CLK = GPIO_NUM_43;
constexpr gpio_num_t PIN_SDMMC_CMD = GPIO_NUM_44;
constexpr gpio_num_t PIN_SDMMC_D0  = GPIO_NUM_39;
constexpr gpio_num_t PIN_SDMMC_D1  = GPIO_NUM_NC;
constexpr gpio_num_t PIN_SDMMC_D2  = GPIO_NUM_NC;
constexpr gpio_num_t PIN_SDMMC_D3  = GPIO_NUM_NC;
constexpr gpio_num_t PIN_SDMMC_PWR_EN = GPIO_NUM_NC;
constexpr int        SDMMC_LDO_CHANNEL = CONFIG_SDMMC_LDO_CHANNEL;

// No Ethernet PHY.
constexpr bool       HAS_ETHERNET         = false;
constexpr gpio_num_t PIN_ETH_MDC          = GPIO_NUM_NC;
constexpr gpio_num_t PIN_ETH_MDIO         = GPIO_NUM_NC;
constexpr gpio_num_t PIN_ETH_PHY_RST      = GPIO_NUM_NC;
constexpr int        ETH_PHY_ADDR         = 0;

// USB Host on the second USB-C (USB 2.0 HS PHY, no on-board hub). Hardware limit: both USB-C
// VBUS pins only feed the board through Schottky diodes, the board never sources 5 V. Devices
// (and hubs) only enumerate if VBUS comes from elsewhere, e.g. a powered hub that does not need
// upstream VBUS or a USB-C OTG Y-cable with a 5 V supply.
constexpr bool       HAS_USB_HOST         = true;
constexpr bool       HAS_4PORT_USB_HUB    = false;
constexpr gpio_num_t PIN_USB_HUB_RST      = GPIO_NUM_NC;
constexpr gpio_num_t PIN_USB_HOST_PWR_EN  = GPIO_NUM_NC;

// UART header pins (UART1 TX47/RX48 shared with the wireless-module slot, UART3 TX27/RX28).
// Not driven by this project; RESERVED_PINS only lists what the firmware actually claims.
constexpr ReservedPin RESERVED_PINS[] = {
    BOARD_PIN(PIN_RGB_HSYNC), BOARD_PIN(PIN_RGB_VSYNC), BOARD_PIN(PIN_RGB_DE), BOARD_PIN(PIN_RGB_PCLK),
    BOARD_PIN(PIN_RGB_DATA[0]), BOARD_PIN(PIN_RGB_DATA[1]), BOARD_PIN(PIN_RGB_DATA[2]), BOARD_PIN(PIN_RGB_DATA[3]),
    BOARD_PIN(PIN_RGB_DATA[4]), BOARD_PIN(PIN_RGB_DATA[5]), BOARD_PIN(PIN_RGB_DATA[6]), BOARD_PIN(PIN_RGB_DATA[7]),
    BOARD_PIN(PIN_RGB_DATA[8]), BOARD_PIN(PIN_RGB_DATA[9]), BOARD_PIN(PIN_RGB_DATA[10]), BOARD_PIN(PIN_RGB_DATA[11]),
    BOARD_PIN(PIN_RGB_DATA[12]), BOARD_PIN(PIN_RGB_DATA[13]), BOARD_PIN(PIN_RGB_DATA[14]), BOARD_PIN(PIN_RGB_DATA[15]),
    BOARD_PIN(PIN_TOUCH_RST), BOARD_PIN(PIN_TOUCH_INT),
    BOARD_PIN(PIN_I2S_BCLK), BOARD_PIN(PIN_I2S_DOUT), BOARD_PIN(PIN_I2S_LRCK),
    BOARD_PIN(PIN_SDIO_CLK), BOARD_PIN(PIN_SDIO_CMD), BOARD_PIN(PIN_SDIO_D0), BOARD_PIN(PIN_SDIO_D1), BOARD_PIN(PIN_SDIO_D2), BOARD_PIN(PIN_SDIO_D3),
    BOARD_PIN(PIN_SDMMC_CLK), BOARD_PIN(PIN_SDMMC_CMD), BOARD_PIN(PIN_SDMMC_D0),
};

} // namespace Board::CrowPanel5Inch
