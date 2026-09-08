/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"

static const char *TAG = "gt911";

/* GT911 registers */
#define ESP_LCD_TOUCH_GT911_PRODUCT_ID_REG  (0x8140)
#define ESP_LCD_TOUCH_GT911_STATUS_REG      (0x814E)
#define ESP_LCD_TOUCH_GT911_POINT_1_REG     (0x814F)

/* GT911 reports at most 5 points, 8 bytes each */
#define ESP_LCD_TOUCH_GT911_MAX_POINTS      (5)
#define ESP_LCD_TOUCH_GT911_POINT_BYTES     (8)

static esp_err_t esp_lcd_touch_gt911_read_data(esp_lcd_touch_handle_t tp);
static bool esp_lcd_touch_gt911_get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength,
                                       uint8_t *point_num, uint8_t max_point_num);
static esp_err_t esp_lcd_touch_gt911_del(esp_lcd_touch_handle_t tp);

/* I2C read/write */
static esp_err_t touch_gt911_i2c_read(esp_lcd_touch_handle_t tp, uint16_t reg, uint8_t *data, uint8_t len);
static esp_err_t touch_gt911_i2c_write(esp_lcd_touch_handle_t tp, uint16_t reg, uint8_t data);

/* GT911 reset (also latches the I2C address) */
static esp_err_t touch_gt911_reset(esp_lcd_touch_handle_t tp);

esp_err_t esp_lcd_touch_new_i2c_gt911(const esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *config,
                                      esp_lcd_touch_handle_t *out_touch)
{
    esp_err_t ret = ESP_OK;
    esp_lcd_touch_handle_t gt911 = NULL;

    ESP_RETURN_ON_FALSE(io && config && out_touch, ESP_ERR_INVALID_ARG, TAG, "invalid arguments");

    gt911 = heap_caps_calloc(1, sizeof(esp_lcd_touch_t), MALLOC_CAP_DEFAULT);
    ESP_RETURN_ON_FALSE(gt911, ESP_ERR_NO_MEM, TAG, "no mem for GT911 controller");

    gt911->io = io;
    gt911->read_data = esp_lcd_touch_gt911_read_data;
    gt911->get_xy = esp_lcd_touch_gt911_get_xy;
    gt911->del = esp_lcd_touch_gt911_del;
    gt911->data.lock.owner = portMUX_FREE_VAL;
    memcpy(&gt911->config, config, sizeof(esp_lcd_touch_config_t));

    /* Reset pin */
    if (gt911->config.rst_gpio_num != GPIO_NUM_NC) {
        const gpio_config_t rst_gpio_config = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = BIT64(gt911->config.rst_gpio_num),
        };
        ESP_GOTO_ON_ERROR(gpio_config(&rst_gpio_config), err, TAG, "GPIO config failed");
    }

    /* INT pin is driven as an output during reset to latch the I2C address, and only then
       reconfigured as the interrupt input below. */
    if (gt911->config.int_gpio_num != GPIO_NUM_NC) {
        const gpio_config_t int_gpio_config = {
            .mode = GPIO_MODE_OUTPUT,
            .intr_type = GPIO_INTR_DISABLE,
            .pin_bit_mask = BIT64(gt911->config.int_gpio_num),
        };
        ESP_GOTO_ON_ERROR(gpio_config(&int_gpio_config), err, TAG, "GPIO config failed");
    }

    ESP_GOTO_ON_ERROR(touch_gt911_reset(gt911), err, TAG, "GT911 reset failed");

    /* Read the product ID ("911") - also proves the controller answers on this address */
    uint8_t product_id[4] = {0};
    if (touch_gt911_i2c_read(gt911, ESP_LCD_TOUCH_GT911_PRODUCT_ID_REG, product_id, sizeof(product_id)) == ESP_OK) {
        ESP_LOGI(TAG, "GT911 product ID: %c%c%c%c", product_id[0], product_id[1], product_id[2], product_id[3]);
    } else {
        ESP_LOGW(TAG, "Could not read GT911 product ID");
    }

    /* Interrupt input */
    if (gt911->config.int_gpio_num != GPIO_NUM_NC) {
        const gpio_config_t int_gpio_config = {
            .mode = GPIO_MODE_INPUT,
            .intr_type = (gt911->config.levels.interrupt ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE),
            .pin_bit_mask = BIT64(gt911->config.int_gpio_num),
        };
        ESP_GOTO_ON_ERROR(gpio_config(&int_gpio_config), err, TAG, "GPIO config failed");

        if (gt911->config.interrupt_callback) {
            esp_lcd_touch_register_interrupt_callback(gt911, gt911->config.interrupt_callback);
        }
    }

    *out_touch = gt911;

    return ESP_OK;

err:
    ESP_LOGE(TAG, "Error (0x%x)! Touch controller GT911 initialization failed!", ret);
    if (gt911) {
        esp_lcd_touch_gt911_del(gt911);
    }
    return ret;
}

static esp_err_t touch_gt911_reset(esp_lcd_touch_handle_t tp)
{
    /* The GT911 samples INT while RST is released: INT low selects I2C address 0x5D, which
       is what ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG() and this board's wiring use. */
    if (tp->config.rst_gpio_num != GPIO_NUM_NC) {
        ESP_RETURN_ON_ERROR(gpio_set_level(tp->config.rst_gpio_num, tp->config.levels.reset), TAG,
                            "GPIO set level error!");
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (tp->config.int_gpio_num != GPIO_NUM_NC) {
        ESP_RETURN_ON_ERROR(gpio_set_level(tp->config.int_gpio_num, 0), TAG, "GPIO set level error!");
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (tp->config.rst_gpio_num != GPIO_NUM_NC) {
        ESP_RETURN_ON_ERROR(gpio_set_level(tp->config.rst_gpio_num, !tp->config.levels.reset), TAG,
                            "GPIO set level error!");
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    /* The controller needs ~50 ms after reset before it answers on I2C */
    vTaskDelay(pdMS_TO_TICKS(50));

    return ESP_OK;
}

static esp_err_t esp_lcd_touch_gt911_read_data(esp_lcd_touch_handle_t tp)
{
    uint8_t status = 0;
    uint8_t points[ESP_LCD_TOUCH_GT911_MAX_POINTS * ESP_LCD_TOUCH_GT911_POINT_BYTES] = {0};

    ESP_RETURN_ON_ERROR(touch_gt911_i2c_read(tp, ESP_LCD_TOUCH_GT911_STATUS_REG, &status, 1), TAG, "I2C read error!");

    /* Bit 7 = "coordinates ready"; nothing to do until the controller sets it */
    if ((status & 0x80) == 0) {
        return ESP_OK;
    }

    uint8_t touch_cnt = status & 0x0F;
    if (touch_cnt > ESP_LCD_TOUCH_GT911_MAX_POINTS) {
        touch_cnt = 0; /* Bogus count - drop the frame but still clear the status below */
    }

    esp_err_t err = ESP_OK;
    if (touch_cnt > 0) {
        err = touch_gt911_i2c_read(tp, ESP_LCD_TOUCH_GT911_POINT_1_REG, points,
                                   touch_cnt * ESP_LCD_TOUCH_GT911_POINT_BYTES);
    }

    /* Always clear the status register, otherwise the controller stops reporting */
    ESP_RETURN_ON_ERROR(touch_gt911_i2c_write(tp, ESP_LCD_TOUCH_GT911_STATUS_REG, 0), TAG, "I2C write error!");
    ESP_RETURN_ON_ERROR(err, TAG, "I2C read error!");

    if (touch_cnt > CONFIG_ESP_LCD_TOUCH_MAX_POINTS) {
        touch_cnt = CONFIG_ESP_LCD_TOUCH_MAX_POINTS;
    }

    portENTER_CRITICAL(&tp->data.lock);
    tp->data.points = touch_cnt;
    for (uint8_t i = 0; i < touch_cnt; i++) {
        const uint8_t *point = &points[i * ESP_LCD_TOUCH_GT911_POINT_BYTES];
        /* point[0] = track id, [1..2] = X (LSB first), [3..4] = Y, [5..6] = size */
        tp->data.coords[i].x = (uint16_t)point[1] | ((uint16_t)point[2] << 8);
        tp->data.coords[i].y = (uint16_t)point[3] | ((uint16_t)point[4] << 8);
        tp->data.coords[i].strength = (uint16_t)point[5] | ((uint16_t)point[6] << 8);
    }
    portEXIT_CRITICAL(&tp->data.lock);

    return ESP_OK;
}

static bool esp_lcd_touch_gt911_get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength,
                                       uint8_t *point_num, uint8_t max_point_num)
{
    assert(tp != NULL);
    assert(x != NULL);
    assert(y != NULL);
    assert(point_num != NULL);
    assert(max_point_num > 0);

    portENTER_CRITICAL(&tp->data.lock);

    *point_num = (tp->data.points > max_point_num) ? max_point_num : tp->data.points;

    for (uint8_t i = 0; i < *point_num; i++) {
        x[i] = tp->data.coords[i].x;
        y[i] = tp->data.coords[i].y;
        if (strength) {
            strength[i] = tp->data.coords[i].strength;
        }
    }

    /* Points are consumed */
    tp->data.points = 0;

    portEXIT_CRITICAL(&tp->data.lock);

    return (*point_num > 0);
}

static esp_err_t esp_lcd_touch_gt911_del(esp_lcd_touch_handle_t tp)
{
    assert(tp != NULL);

    if (tp->config.int_gpio_num != GPIO_NUM_NC) {
        if (tp->config.interrupt_callback) {
            gpio_isr_handler_remove(tp->config.int_gpio_num);
        }
        gpio_reset_pin(tp->config.int_gpio_num);
    }

    if (tp->config.rst_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.rst_gpio_num);
    }

    free(tp);

    return ESP_OK;
}

static esp_err_t touch_gt911_i2c_read(esp_lcd_touch_handle_t tp, uint16_t reg, uint8_t *data, uint8_t len)
{
    assert(tp != NULL);
    assert(data != NULL);

    return esp_lcd_panel_io_rx_param(tp->io, reg, data, len);
}

static esp_err_t touch_gt911_i2c_write(esp_lcd_touch_handle_t tp, uint16_t reg, uint8_t data)
{
    assert(tp != NULL);

    return esp_lcd_panel_io_tx_param(tp->io, reg, &data, 1);
}
