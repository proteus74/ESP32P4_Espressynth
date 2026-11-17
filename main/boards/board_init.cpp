#include "board_init.hpp"
#include "board_config.hpp"
#include "pin_utils.hpp"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include <cstdio>

static const char* TAG = "BoardInit";
static i2c_master_bus_handle_t s_i2c_bus_handle[2] = {nullptr, nullptr};
#if defined(CONFIG_BOARD_CROWPANEL_5INCH)
static i2c_master_dev_handle_t s_stc8_dev = nullptr;
#endif

// Compile-time pin sanity checks for the "I2C Subsystem" Kconfig pins - they're free-form
// GPIO numbers (like the WM8960 pins), so a bad choice is caught here instead of at runtime.
#if defined(CONFIG_APP_ENABLE_I2C0)
namespace {
constexpr gpio_num_t I2C0_PIN_SDA = static_cast<gpio_num_t>(CONFIG_I2C0_PIN_SDA);
constexpr gpio_num_t I2C0_PIN_SCL = static_cast<gpio_num_t>(CONFIG_I2C0_PIN_SCL);
} // namespace
static_assert(I2C0_PIN_SDA != I2C0_PIN_SCL, "I2C0_PIN_SDA/SCL must be distinct pins");
BOARD_ASSERT_PIN_NOT_BOARD(I2C0_PIN_SDA);
BOARD_ASSERT_PIN_NOT_BOARD(I2C0_PIN_SCL);
#endif

#if defined(CONFIG_APP_ENABLE_I2C1)
namespace {
constexpr gpio_num_t I2C1_PIN_SDA = static_cast<gpio_num_t>(CONFIG_I2C1_PIN_SDA);
constexpr gpio_num_t I2C1_PIN_SCL = static_cast<gpio_num_t>(CONFIG_I2C1_PIN_SCL);
} // namespace
static_assert(I2C1_PIN_SDA != I2C1_PIN_SCL, "I2C1_PIN_SDA/SCL must be distinct pins");
BOARD_ASSERT_PIN_NOT_BOARD(I2C1_PIN_SDA);
BOARD_ASSERT_PIN_NOT_BOARD(I2C1_PIN_SCL);
#if defined(CONFIG_APP_ENABLE_I2C0)
static_assert(I2C1_PIN_SDA != I2C0_PIN_SDA && I2C1_PIN_SDA != I2C0_PIN_SCL &&
              I2C1_PIN_SCL != I2C0_PIN_SDA && I2C1_PIN_SCL != I2C0_PIN_SCL,
              "I2C1_PIN_SDA/SCL conflicts with I2C0_PIN_SDA/SCL");
#endif
#endif

namespace {

// Logs every address (7-bit, the 0x08-0x77 range i2cdetect/most tools scan - 0x00-0x07 and
// 0x78-0x7F are reserved by the I2C spec) that ACKs a zero-byte probe on the given bus. Run
// once per bus right after it's created, before any device on it has been touched, so an
// intermittently-vanishing device (see wm8960_manager.hpp's per-write I2C diagnostics) shows
// up here as a missing address instead of only as a downstream write failure.
void i2c_scan_bus(const char* bus_name, i2c_master_bus_handle_t bus_handle) {
    char found[128] = {};
    size_t found_len = 0;
    int device_count = 0;
    for (uint16_t addr = 0x08; addr <= 0x77; ++addr) {
        if (i2c_master_probe(bus_handle, addr, 20) == ESP_OK) {
            if (found_len < sizeof(found) - 1) {
                const int written = std::snprintf(found + found_len, sizeof(found) - found_len,
                                                  "%s0x%02X", device_count == 0 ? "" : ", ", addr);
                if (written > 0) {
                    const size_t remaining = sizeof(found) - found_len;
                    found_len += static_cast<size_t>(written) < remaining
                                     ? static_cast<size_t>(written)
                                     : remaining - 1;
                }
            }
            ++device_count;
        }
    }
    if (device_count == 0) {
        ESP_LOGW(TAG, "%s scan: no devices found", bus_name);
    } else {
        ESP_LOGI(TAG, "%s scan: %d device(s) found: %s", bus_name, device_count, found);
    }
}

} // namespace

namespace Board {

i2c_master_bus_handle_t get_i2c_bus_handle(int port) {
    if (port < 0 || port > 1) return nullptr;
    return s_i2c_bus_handle[port];
}

esp_err_t init() {
#if defined(CONFIG_APP_ENABLE_I2C0)
    {
        i2c_master_bus_config_t i2c_bus_config = {};
        i2c_bus_config.i2c_port = I2C_NUM_0;
        i2c_bus_config.sda_io_num = static_cast<gpio_num_t>(CONFIG_I2C0_PIN_SDA);
        i2c_bus_config.scl_io_num = static_cast<gpio_num_t>(CONFIG_I2C0_PIN_SCL);
        i2c_bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
        i2c_bus_config.glitch_ignore_cnt = 7;
        i2c_bus_config.intr_priority = 0;
        i2c_bus_config.trans_queue_depth = 0;
        i2c_bus_config.flags.enable_internal_pullup = 1;
        i2c_bus_config.flags.allow_pd = 0;

        esp_err_t ret = i2c_new_master_bus(&i2c_bus_config, &s_i2c_bus_handle[0]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize I2C0 bus: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "I2C0 master initialized (SDA:GPIO%d SCL:GPIO%d, %luHz)",
                 CONFIG_I2C0_PIN_SDA, CONFIG_I2C0_PIN_SCL,
                 static_cast<unsigned long>(i2c_bus_frequency_hz(0)));
        i2c_scan_bus("I2C0", s_i2c_bus_handle[0]);
    }
#endif

#if defined(CONFIG_APP_ENABLE_I2C1)
    {
        i2c_master_bus_config_t i2c_bus_config = {};
        i2c_bus_config.i2c_port = I2C_NUM_1;
        i2c_bus_config.sda_io_num = static_cast<gpio_num_t>(CONFIG_I2C1_PIN_SDA);
        i2c_bus_config.scl_io_num = static_cast<gpio_num_t>(CONFIG_I2C1_PIN_SCL);
        i2c_bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
        i2c_bus_config.glitch_ignore_cnt = 7;
        i2c_bus_config.intr_priority = 0;
        i2c_bus_config.trans_queue_depth = 0;
        i2c_bus_config.flags.enable_internal_pullup = 1;
        i2c_bus_config.flags.allow_pd = 0;

        esp_err_t ret = i2c_new_master_bus(&i2c_bus_config, &s_i2c_bus_handle[1]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize I2C1 bus: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "I2C1 master initialized (SDA:GPIO%d SCL:GPIO%d, %luHz)",
                 CONFIG_I2C1_PIN_SDA, CONFIG_I2C1_PIN_SCL,
                 static_cast<unsigned long>(i2c_bus_frequency_hz(1)));
        i2c_scan_bus("I2C1", s_i2c_bus_handle[1]);
    }
#endif

#if defined(CONFIG_BOARD_CROWPANEL_5INCH)
    // STC8H1K co-MCU on I2C0: backlight, touch reset, amp shutdown, battery. The ESP-Hosted
    // driver owns the C6 reset line on this board (CONFIG_ESP_HOSTED_HOST_RESET_GPIO).
    if (s_i2c_bus_handle[0] != nullptr) {
        i2c_device_config_t stc8_cfg = {};
        stc8_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        stc8_cfg.device_address = CurrentBoard::STC8_I2C_ADDR;
        stc8_cfg.scl_speed_hz = i2c_bus_frequency_hz(0);
        esp_err_t stc8_ret = i2c_master_bus_add_device(s_i2c_bus_handle[0], &stc8_cfg, &s_stc8_dev);
        if (stc8_ret != ESP_OK) {
            ESP_LOGE(TAG, "STC8 co-MCU device setup failed: %s", esp_err_to_name(stc8_ret));
            s_stc8_dev = nullptr;
        } else {
            ESP_LOGI(TAG, "STC8 co-MCU ready at 0x%02X", CurrentBoard::STC8_I2C_ADDR);
        }
    } else {
        ESP_LOGE(TAG, "STC8 co-MCU needs I2C0 (enable it in the I2C Subsystem Kconfig)");
    }
#endif

#if defined(CONFIG_BOARD_JC8012P4A1) || defined(CONFIG_BOARD_JC1060P470C_7_INCH) || \
    defined(CONFIG_WAVESHARE_P4_WIFI_DEV_KIT_BOARD) || defined(CONFIG_WAVESHARE_P4_NANO_BOARD)
    // Board-specific init: ESP32-C6 Reset line
    if (CurrentBoard::PIN_C6_RESET != GPIO_NUM_NC) {
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = (1ULL << CurrentBoard::PIN_C6_RESET);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&io_conf);
        gpio_set_level(CurrentBoard::PIN_C6_RESET, 1); // Keep C6 out of reset
        ESP_LOGI(TAG, "ESP32-C6 Reset line initialized high (GPIO%d)", CurrentBoard::PIN_C6_RESET);
    }
#endif

    return ESP_OK;
}

#if defined(CONFIG_BOARD_CROWPANEL_5INCH)
esp_err_t stc8_write_reg(uint8_t reg, uint8_t value) {
    if (s_stc8_dev == nullptr) return ESP_ERR_INVALID_STATE;
    const uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(s_stc8_dev, buf, sizeof(buf), 100);
}

esp_err_t stc8_read_reg(uint8_t reg, uint8_t* data, size_t len) {
    if (s_stc8_dev == nullptr) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(s_stc8_dev, &reg, 1, data, len, 100);
}

esp_err_t stc8_set_gpio(uint8_t gpio, bool level) {
    return stc8_write_reg(CurrentBoard::STC8_REG_SET_GPIO + gpio, level ? 1 : 0);
}

esp_err_t read_battery(BatteryStatus& out) {
    // Elecrow's Battery_info_t at register 0x00, read one register at a time like their driver:
    // u32 adc_voltage, u32 bat_voltage (mV), u8 bat_level (%), u8 bat_state, u8 led_state.
    uint8_t raw[11] = {};
    for (uint8_t i = 0; i < sizeof(raw); ++i) {
        const esp_err_t err = stc8_read_reg(CurrentBoard::STC8_REG_BATTERY + i, &raw[i], 1);
        if (err != ESP_OK) return err;
    }
    out.millivolts = static_cast<uint32_t>(raw[4]) | (static_cast<uint32_t>(raw[5]) << 8) |
                     (static_cast<uint32_t>(raw[6]) << 16) | (static_cast<uint32_t>(raw[7]) << 24);
    out.percent = raw[8] > 100 ? 100 : raw[8];
    out.state = raw[9];
    return ESP_OK;
}

esp_err_t set_backlight_percent(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    esp_err_t ret = stc8_set_gpio(CurrentBoard::STC8_GPIO_OUT_LCD_BL_POWER, percent > 0);
    if (ret != ESP_OK) return ret;
    const uint8_t duty = static_cast<uint8_t>(percent * CONFIG_CROWPANEL_BACKLIGHT_PWM_MAX / 100);
    return stc8_write_reg(CurrentBoard::STC8_REG_SET_PWM + CurrentBoard::STC8_PWM_LCD_BL, duty);
}
#endif

} // namespace Board
