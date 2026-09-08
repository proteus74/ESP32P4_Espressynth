/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief ESP LCD touch: GT911
 *
 * Local copy of the GT911 driver, written against the vendored esp_lcd_touch.h in this
 * folder (the same arrangement the GSL3680 driver uses) so the project does not pull in a
 * second esp_lcd_touch implementation through the component manager.
 */

#pragma once

#include "esp_lcd_types.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/* GT911 I2C addresses. Which one the controller answers on is latched from the INT line
   level while it comes out of reset (INT low -> 0x5D, INT high -> 0x14). */
#define ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS          (0x5D)
#define ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP   (0x14)

/**
 * @brief Panel IO configuration for a GT911 on an I2C bus.
 */
#define ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG()             \
    {                                                   \
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, \
        .control_phase_bytes = 1,                       \
        .dc_bit_offset = 0,                             \
        .lcd_cmd_bits = 16,                             \
        .flags =                                        \
        {                                               \
            .disable_control_phase = 1,                 \
        }                                               \
    }

/**
 * @brief Create a new GT911 touch driver.
 *
 * @param io LCD panel IO handle (I2C, 16-bit register addressing)
 * @param config Touch configuration
 * @param out_touch Touch instance handle
 */
esp_err_t esp_lcd_touch_new_i2c_gt911(const esp_lcd_panel_io_handle_t io,
                                      const esp_lcd_touch_config_t *config,
                                      esp_lcd_touch_handle_t *out_touch);

#ifdef __cplusplus
}
#endif
