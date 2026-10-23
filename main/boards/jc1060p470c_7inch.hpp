#pragma once
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "boards/reserved_pin.hpp"
// Guition JC1060P470C, 7-inch variant (JC-ESP32P4-M3 module, schematic JC1060P470C_I_W_Y V1.0).
// Pinout taken from the vendor demo bundle "JC1060P470C_I_W_Y_New_Panel":
//   * xiaozhi-esp32-main/main/boards/guition-jc1060p470-new/config.h
//   * common_components/espressif__esp32_p4_function_ev_board (BSP used by lvgl_demo_v9)
//   * ethernet/sdkconfig (EMAC/PHY pins), uart_echo_rs485 (RS485 pins on GPIO26/27)

namespace Board::JC1060P470C7Inch {

constexpr const char* BOARD_NAME = "JC1060P470C 7\" (ESP32-P4)";

// Display Specifications (MIPI DSI - JD9165, 1024x600 landscape panel)
// The panel is natively landscape, so no software rotation is needed (unlike JC8012P4A1).
constexpr int DISPLAY_WIDTH       = CONFIG_APP_DISPLAY_WIDTH;
constexpr int DISPLAY_HEIGHT      = CONFIG_APP_DISPLAY_HEIGHT;
constexpr int DISPLAY_NATIVE_WIDTH  = CONFIG_APP_DISPLAY_WIDTH;
constexpr int DISPLAY_NATIVE_HEIGHT = CONFIG_APP_DISPLAY_HEIGHT;
constexpr int DISPLAY_DSI_LANES   = 2;
constexpr int DISPLAY_REFRESH_HZ  = 60;
// DSI/DPI timing from the vendor demo (both the BSP and the xiaozhi board file agree on the
// porches; the demos differ only in lane bit rate - 550 Mbps is the value Guition ships in
// its own board file and leaves ~30% headroom over the 416 Mbps/lane this mode needs).
constexpr int DISPLAY_DSI_LANE_BITRATE_MBPS = 550;
constexpr int DISPLAY_DPI_CLOCK_MHZ = 52;
constexpr int DISPLAY_HSYNC_PULSE_WIDTH = 24;
constexpr int DISPLAY_HSYNC_BACK_PORCH  = 136;
constexpr int DISPLAY_HSYNC_FRONT_PORCH = 160;
constexpr int DISPLAY_VSYNC_PULSE_WIDTH = 2;
constexpr int DISPLAY_VSYNC_BACK_PORCH  = 21;
constexpr int DISPLAY_VSYNC_FRONT_PORCH = 12;
// Panel reset: Guition's own board file uses GPIO27, the BSP copy in the same bundle uses
// GPIO0. Configurable under "Board Configuration > JC1060P470C Settings" so a dark panel can
// be retried from menuconfig instead of a code change.
constexpr gpio_num_t PIN_LCD_RST   = static_cast<gpio_num_t>(CONFIG_JC1060P470C_PIN_LCD_RST);
constexpr gpio_num_t PIN_BACKLIGHT  = GPIO_NUM_23;  // active high
constexpr int DISPLAY_DSI_LDO_CHANNEL = 3;
constexpr int DISPLAY_DSI_LDO_VOLTAGE_MV = 2500;

// I2C Bus: see "I2C Subsystem" Kconfig (I2C0_PIN_SDA/SCL) - not board-fixed anymore.
// (Shared between Touch GT911, Audio ES8311, RTC RX8025T: SDA GPIO7 / SCL GPIO8.)

// Touch Controller (GT911, I2C address 0x5D)
constexpr gpio_num_t PIN_TOUCH_RST = GPIO_NUM_22;
constexpr gpio_num_t PIN_TOUCH_INT = GPIO_NUM_21;

// Audio Codec (ES8311 / I2S, NS4150 PA)
constexpr gpio_num_t PIN_I2S_MCLK  = GPIO_NUM_13;
constexpr gpio_num_t PIN_I2S_BCLK  = GPIO_NUM_12;
constexpr gpio_num_t PIN_I2S_DOUT  = GPIO_NUM_9;   // DSDIN on codec
constexpr gpio_num_t PIN_I2S_DIN   = GPIO_NUM_48;  // ASDOUT on codec / ES7210 SDOUT
constexpr gpio_num_t PIN_I2S_LRCK  = GPIO_NUM_10;
constexpr gpio_num_t PIN_AUDIO_PA  = GPIO_NUM_11;  // PA_CTRL (active high)

// ESP32-C6 Hosted Co-Processor (SDIO 4-bit Transport, internal to the JC-ESP32P4-M3 module)
constexpr gpio_num_t PIN_SDIO_CLK  = GPIO_NUM_18;
constexpr gpio_num_t PIN_SDIO_CMD  = GPIO_NUM_19;
constexpr gpio_num_t PIN_SDIO_D0   = GPIO_NUM_14;
constexpr gpio_num_t PIN_SDIO_D1   = GPIO_NUM_15;
constexpr gpio_num_t PIN_SDIO_D2   = GPIO_NUM_16;
constexpr gpio_num_t PIN_SDIO_D3   = GPIO_NUM_17;
constexpr gpio_num_t PIN_C6_RESET  = GPIO_NUM_54;  // C6_CHIP_PU
constexpr gpio_num_t PIN_C6_WAKEUP = GPIO_NUM_6;
// C6 reset line as the ESP-Hosted driver must see it (checked in board_config.hpp).
constexpr gpio_num_t HOSTED_RESET_GPIO = PIN_C6_RESET;

// TF Card Slot (SDMMC Slot 0). TF_VCC is fed straight from ESP_LDO_VO4, so there is no
// separate power-enable GPIO on this board.
constexpr gpio_num_t PIN_SDMMC_CLK = GPIO_NUM_43;
constexpr gpio_num_t PIN_SDMMC_CMD = GPIO_NUM_44;
constexpr gpio_num_t PIN_SDMMC_D0  = GPIO_NUM_39;
constexpr gpio_num_t PIN_SDMMC_D1  = GPIO_NUM_40;
constexpr gpio_num_t PIN_SDMMC_D2  = GPIO_NUM_41;
constexpr gpio_num_t PIN_SDMMC_D3  = GPIO_NUM_42;
constexpr gpio_num_t PIN_SDMMC_PWR_EN = GPIO_NUM_NC;
constexpr int        SDMMC_LDO_CHANNEL = CONFIG_SDMMC_LDO_CHANNEL; // ESP_LDO_VO4 by default

// Ethernet (RMII EMAC + IP101 PHY, RJ45 with integrated magnetics) - populated on the
// Ethernet-equipped variants of this board only.
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

// USB Host (USB 2.0 High-Speed Type-C, no on-board hub)
constexpr bool       HAS_USB_HOST         = true;
constexpr bool       HAS_4PORT_USB_HUB    = false;
constexpr gpio_num_t PIN_USB_HUB_RST      = GPIO_NUM_NC;
constexpr gpio_num_t PIN_USB_HOST_PWR_EN  = GPIO_NUM_NC;

// RS485 transceiver (MAX485) - GPIO26 = TX, GPIO27 = RX in the vendor's uart_echo_rs485
// demo. Nothing in this project drives them, so they stay out of RESERVED_PINS; note that
// GPIO27 doubles as the panel reset default (see PIN_LCD_RST above), so the RS485 port and
// the panel reset line cannot both be used on the same unit.
constexpr gpio_num_t PIN_RS485_TX  = GPIO_NUM_26;
constexpr gpio_num_t PIN_RS485_RX  = GPIO_NUM_27;

// Every named pin above that's physically wired on this board (GPIO_NUM_NC entries are
// harmless - filtered out by conflict checks). Used to catch a user-chosen pin (e.g. the
// generic WM8960 codec's I2S/button pins) colliding with something already in use here.
// PIN_AUDIO_PA is only listed while its owning feature (ES8311) is actually enabled: with
// the feature off, nothing in this project ever configures that GPIO.
constexpr ReservedPin RESERVED_PINS[] = {
    BOARD_PIN(PIN_TOUCH_RST), BOARD_PIN(PIN_TOUCH_INT),
    BOARD_PIN(PIN_LCD_RST), BOARD_PIN(PIN_BACKLIGHT),
    BOARD_PIN(PIN_I2S_MCLK), BOARD_PIN(PIN_I2S_BCLK), BOARD_PIN(PIN_I2S_DOUT), BOARD_PIN(PIN_I2S_DIN), BOARD_PIN(PIN_I2S_LRCK),
#if defined(CONFIG_ONBOARD_AUDIO_ES8311)
    BOARD_PIN(PIN_AUDIO_PA),
#endif
    BOARD_PIN(PIN_SDIO_CLK), BOARD_PIN(PIN_SDIO_CMD), BOARD_PIN(PIN_SDIO_D0), BOARD_PIN(PIN_SDIO_D1), BOARD_PIN(PIN_SDIO_D2), BOARD_PIN(PIN_SDIO_D3),
    BOARD_PIN(PIN_C6_RESET), BOARD_PIN(PIN_C6_WAKEUP),
    BOARD_PIN(PIN_SDMMC_CLK), BOARD_PIN(PIN_SDMMC_CMD), BOARD_PIN(PIN_SDMMC_D0), BOARD_PIN(PIN_SDMMC_D1), BOARD_PIN(PIN_SDMMC_D2), BOARD_PIN(PIN_SDMMC_D3),
    BOARD_PIN(PIN_SDMMC_PWR_EN),
    BOARD_PIN(PIN_ETH_MDC), BOARD_PIN(PIN_ETH_MDIO), BOARD_PIN(PIN_ETH_PHY_RST),
    BOARD_PIN(PIN_ETH_RMII_CLK), BOARD_PIN(PIN_ETH_RMII_TX_EN), BOARD_PIN(PIN_ETH_RMII_TXD0), BOARD_PIN(PIN_ETH_RMII_TXD1),
    BOARD_PIN(PIN_ETH_RMII_CRS_DV), BOARD_PIN(PIN_ETH_RMII_RXD0), BOARD_PIN(PIN_ETH_RMII_RXD1),
    BOARD_PIN(PIN_USB_HUB_RST), BOARD_PIN(PIN_USB_HOST_PWR_EN),
};

} // namespace Board::JC1060P470C7Inch
