#pragma once

#include "sdkconfig.h"
#include "boards/board_init.hpp"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#if defined(CONFIG_APP_DISPLAY_SPI_ILI9341)
#include "esp_lcd_ili9341.h"
#endif

namespace Feature::SPIDisplay {
inline esp_lcd_panel_io_handle_t io = nullptr;
inline esp_lcd_panel_handle_t panel = nullptr;
constexpr spi_host_device_t host = SPI2_HOST;
constexpr int buffer_lines = CONFIG_APP_DISPLAY_HEIGHT < 40 ? CONFIG_APP_DISPLAY_HEIGHT : 40;

inline esp_err_t init() {
    constexpr const char* TAG = "SPIDisplay";
    const int pins[] = {CONFIG_APP_DISPLAY_SPI_PIN_CLK, CONFIG_APP_DISPLAY_SPI_PIN_MOSI,
                       CONFIG_APP_DISPLAY_SPI_PIN_DC, CONFIG_APP_DISPLAY_SPI_PIN_CS,
                       CONFIG_APP_DISPLAY_SPI_PIN_RESET, CONFIG_APP_DISPLAY_SPI_PIN_BACKLIGHT,
                       CONFIG_APP_DISPLAY_SPI_PIN_MISO};
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        ESP_RETURN_ON_FALSE(pins[i] == -1 || (i == 6 ? GPIO_IS_VALID_GPIO(pins[i])
                                                      : GPIO_IS_VALID_OUTPUT_GPIO(pins[i])),
                            ESP_ERR_INVALID_ARG, TAG, "Invalid display GPIO");
        for (size_t j = 0; j < i; ++j) {
            ESP_RETURN_ON_FALSE(pins[i] == -1 || pins[i] != pins[j], ESP_ERR_INVALID_ARG,
                                TAG, "Display GPIOs must be distinct");
        }
#if defined(CONFIG_APP_ENABLE_I2C0)
        ESP_RETURN_ON_FALSE(pins[i] == -1 ||
                            (pins[i] != CONFIG_I2C0_PIN_SDA && pins[i] != CONFIG_I2C0_PIN_SCL),
                            ESP_ERR_INVALID_ARG, TAG, "Display GPIO conflicts with I2C0");
#endif
#if defined(CONFIG_APP_ENABLE_I2C1)
        ESP_RETURN_ON_FALSE(pins[i] == -1 ||
                            (pins[i] != CONFIG_I2C1_PIN_SDA && pins[i] != CONFIG_I2C1_PIN_SCL),
                            ESP_ERR_INVALID_ARG, TAG, "Display GPIO conflicts with I2C1");
#endif
    }
    ESP_RETURN_ON_FALSE(pins[0] >= 0 && pins[1] >= 0 && pins[2] >= 0,
                        ESP_ERR_INVALID_ARG, TAG, "SPI display requires CLK, MOSI and D/C");
    ESP_RETURN_ON_FALSE((CONFIG_APP_DISPLAY_WIDTH <= 240 && CONFIG_APP_DISPLAY_HEIGHT <= 320) ||
                        (CONFIG_APP_DISPLAY_WIDTH <= 320 && CONFIG_APP_DISPLAY_HEIGHT <= 240),
                        ESP_ERR_INVALID_ARG, TAG, "Display exceeds controller's 240x320 address space");
    spi_bus_config_t bus = {};
    bus.sclk_io_num = pins[0];
    bus.mosi_io_num = pins[1];
    bus.miso_io_num = pins[6];
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = CONFIG_APP_DISPLAY_WIDTH * buffer_lines * sizeof(uint16_t);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(host, &bus, SPI_DMA_CH_AUTO), TAG, "SPI bus init failed");
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.dc_gpio_num = static_cast<gpio_num_t>(pins[2]);
    io_config.cs_gpio_num = static_cast<gpio_num_t>(pins[3]);
    io_config.pclk_hz = 20 * 1000 * 1000;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.trans_queue_depth = 10;
    esp_err_t err = esp_lcd_new_panel_io_spi(
        static_cast<esp_lcd_spi_bus_handle_t>(host), &io_config, &io);
    if (err == ESP_OK) {
        esp_lcd_panel_dev_config_t config = {};
        config.reset_gpio_num = static_cast<gpio_num_t>(pins[4]);
        config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        config.bits_per_pixel = 16;
#if defined(CONFIG_APP_DISPLAY_SPI_ILI9341)
        err = esp_lcd_new_panel_ili9341(io, &config, &panel);
#else
        err = esp_lcd_new_panel_st7789(io, &config, &panel);
#endif
    }
    if (err == ESP_OK) err = esp_lcd_panel_reset(panel);
    if (err == ESP_OK) err = esp_lcd_panel_init(panel);
    if (err == ESP_OK) err = esp_lcd_panel_swap_xy(panel, CONFIG_APP_DISPLAY_WIDTH > 240);
    if (err == ESP_OK) err = esp_lcd_panel_mirror(panel, CONFIG_APP_DISPLAY_WIDTH > 240, false);
    if (err == ESP_OK) err = esp_lcd_panel_disp_on_off(panel, true);
    if (err == ESP_OK && pins[5] >= 0) {
        gpio_config_t backlight = {};
        backlight.pin_bit_mask = 1ULL << pins[5];
        backlight.mode = GPIO_MODE_OUTPUT;
        err = gpio_config(&backlight);
        if (err == ESP_OK) err = gpio_set_level(static_cast<gpio_num_t>(pins[5]), 1);
    }
    if (err != ESP_OK) {
        if (panel != nullptr) esp_lcd_panel_del(panel);
        if (io != nullptr) esp_lcd_panel_io_del(io);
        spi_bus_free(host);
        panel = nullptr;
        io = nullptr;
    }
    return err;
}
} // namespace Feature::SPIDisplay
