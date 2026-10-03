#pragma once

#include "app_core.hpp"
#include "boards/board_init.hpp"
#include "eez_ui_bridge.hpp"
#include "esp_check.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lvgl_port.h"
#include <cstdio>
#include <cstring>

namespace Feature::SSD1306 {
inline lv_obj_t* label = nullptr;
inline lv_timer_t* timer = nullptr;
inline TickType_t startup_shown_at = 0;
inline bool show_preset = false;
inline char last_name[64] = {};

inline void update_preset(lv_timer_t*) {
    if (!show_preset) return;
    EezSynthStatus status = {};
    eez_synth_status(&status);
    if (strcmp(last_name, status.preset_name) != 0) {
        snprintf(last_name, sizeof(last_name), "%s", status.preset_name);
        lv_label_set_text(label, last_name);
    }
}

inline esp_err_t init() {
    constexpr const char* TAG = "SSD1306";
    auto bus = Board::get_i2c_bus_handle(CONFIG_APP_SSD1306_I2C_PORT);
    ESP_RETURN_ON_FALSE(bus != nullptr, ESP_ERR_INVALID_STATE, TAG, "I2C port is disabled");
    uint8_t address = 0x3c;
    if (i2c_master_probe(bus, address, 100) != ESP_OK) address = 0x3d;
    ESP_RETURN_ON_ERROR(i2c_master_probe(bus, address, 100), TAG, "SSD1306 not found at 0x3c/0x3d");
#if !defined(CONFIG_APP_ENABLE_LVGL)
    lvgl_port_cfg_t port = ESP_LVGL_PORT_INIT_CONFIG();
    port.task_affinity = App::MAIN_CORE;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port), TAG, "LVGL port init failed");
#endif
    esp_lcd_panel_io_i2c_config_t io_config = {};
    io_config.dev_addr = address;
    io_config.scl_speed_hz = Board::i2c_bus_frequency_hz(CONFIG_APP_SSD1306_I2C_PORT);
    io_config.control_phase_bytes = 1;
    io_config.dc_bit_offset = 6;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.transaction_timeout_ms = 250;
    esp_lcd_panel_io_handle_t io = nullptr;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_config, &io), TAG, "OLED IO init failed");
    esp_lcd_panel_ssd1306_config_t vendor = {};
    vendor.height = CONFIG_APP_SSD1306_HEIGHT;
    esp_lcd_panel_dev_config_t config = {};
    config.reset_gpio_num = GPIO_NUM_NC;
    config.bits_per_pixel = 1;
    config.vendor_config = &vendor;
    esp_lcd_panel_handle_t panel = nullptr;
    esp_err_t err = esp_lcd_new_panel_ssd1306(io, &config, &panel);
    if (err == ESP_OK) err = esp_lcd_panel_reset(panel);
    if (err == ESP_OK) err = esp_lcd_panel_init(panel);
    // The LVGL port's monochrome conversion maps white pixels to cleared bits.
    if (err == ESP_OK) err = esp_lcd_panel_invert_color(panel, true);
#if defined(CONFIG_APP_SSD1306_64X48)
    // Common 64x48 modules expose the middle 64 columns and use alternate COM pins.
    const uint8_t com_pins = 0x12;
    if (err == ESP_OK) err = esp_lcd_panel_io_tx_param(io, 0xda, &com_pins, 1);
    if (err == ESP_OK) err = esp_lcd_panel_set_gap(panel, 32, 0);
#endif
    if (err == ESP_OK) err = esp_lcd_panel_disp_on_off(panel, true);
    if (err != ESP_OK) {
        if (panel != nullptr) esp_lcd_panel_del(panel);
        esp_lcd_panel_io_del(io);
        return err;
    }
    lvgl_port_lock(0);
    auto* previous = lv_display_get_default();
    lvgl_port_display_cfg_t display_config = {};
    display_config.io_handle = io;
    display_config.panel_handle = panel;
    display_config.hres = CONFIG_APP_SSD1306_WIDTH;
    display_config.vres = CONFIG_APP_SSD1306_HEIGHT;
    display_config.buffer_size = display_config.hres * display_config.vres;
    display_config.monochrome = true;
    display_config.color_format = LV_COLOR_FORMAT_I1;
    auto* display = lvgl_port_add_disp(&display_config);
    if (display == nullptr) {
        lvgl_port_unlock();
        esp_lcd_panel_del(panel);
        esp_lcd_panel_io_del(io);
        return ESP_ERR_NO_MEM;
    }
    auto* screen = lv_display_get_screen_active(display);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    label = lv_label_create(screen);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, CONFIG_APP_SSD1306_WIDTH);
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(label, "Espressynth");
    lv_obj_center(label);
    timer = lv_timer_create(update_preset, 250, nullptr);
    if (timer == nullptr) {
        lvgl_port_remove_disp(display);
        if (previous != nullptr) lv_display_set_default(previous);
        lvgl_port_unlock();
        label = nullptr;
        esp_lcd_panel_del(panel);
        esp_lcd_panel_io_del(io);
        return ESP_ERR_NO_MEM;
    }
    if (previous != nullptr) lv_display_set_default(previous);
    lv_refr_now(display);
    startup_shown_at = xTaskGetTickCount();
    lvgl_port_unlock();
    ESP_LOGI(TAG, "OLED ready: %dx%d on I2C%d, address 0x%02x",
             CONFIG_APP_SSD1306_WIDTH, CONFIG_APP_SSD1306_HEIGHT,
             CONFIG_APP_SSD1306_I2C_PORT, address);
    return ESP_OK;
}

inline void finish_startup_screen() {
    if (label == nullptr) return;
    // Also retain the startup text for three seconds when no primary display is enabled.
    const TickType_t elapsed = xTaskGetTickCount() - startup_shown_at;
    const TickType_t minimum = pdMS_TO_TICKS(3000);
    if (elapsed < minimum) vTaskDelay(minimum - elapsed);
    lvgl_port_lock(0);
    show_preset = true;
    // Force the first refresh even if the engine has no preset loaded yet.
    lv_label_set_text(label, "");
    update_preset(nullptr);
    lvgl_port_unlock();
}
} // namespace Feature::SSD1306
