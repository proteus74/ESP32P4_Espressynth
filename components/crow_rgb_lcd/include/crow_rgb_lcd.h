#pragma once
// Patched copy of the ESP-IDF RGB LCD driver, see crow_rgb_lcd.c. Same config/handle types as
// esp_lcd_panel_rgb.h, only the entry points are renamed.
#include "esp_lcd_panel_rgb.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t crow_lcd_new_rgb_panel(const esp_lcd_rgb_panel_config_t *rgb_panel_config, esp_lcd_panel_handle_t *ret_panel);
esp_err_t crow_lcd_rgb_panel_get_frame_buffer(esp_lcd_panel_handle_t panel, uint32_t fb_num, void **fb0, ...);
/** Request a scan-out restart (LCD + DMA) in the next vertical blank. ISR-safe flag set only. */
esp_err_t crow_lcd_rgb_panel_restart(esp_lcd_panel_handle_t panel);

#ifdef __cplusplus
}
#endif
