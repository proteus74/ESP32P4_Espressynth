#pragma once

#include "sdkconfig.h"
#include <cstdint>
#include <cstdio>

#include "cJSON.h"
#if defined(CONFIG_APP_ENABLE_WS2812)
#include <atomic>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#include "features/io/gpio_manager.hpp"
#endif

// WS2812 LED string on a user-chosen GPIO, driven via RMT (espressif/led_strip). Pin and LED
// count are device-wide settings (Settings dialog, /littlefs/LedStripSettings.json) and only
// take effect at boot - init() is called once from Feature::Synth::init().
//
// The render task never touches the RMT peripheral: RgbLed nodes (nodes/RgbLedNode.h) only
// store 0xRRGGBB colors into s_pixels via set_pixel(). A Core-0 task pushes the buffer to the
// strip every 50 ms (20 Hz) and consumes each pixel with exchange(0), so a LED whose widget
// disappeared (preset change, widget deleted) goes dark on the next frame without any
// graph-rebuild hook.
namespace Feature::LedStrip {

constexpr uint16_t kMaxLeds = 256;
constexpr const char *kSettingsPath = "/littlefs/LedStripSettings.json";

struct Settings {
    int8_t pin = -1;    // -1 = not configured
    uint16_t count = 0; // 0 = not configured
};

inline Settings &settings() {
    static Settings s;
    return s;
}

#if defined(CONFIG_APP_ENABLE_WS2812)
constexpr bool kEnabled = true;
namespace detail {
inline const char *TAG = "LedStrip";
inline led_strip_handle_t s_strip = nullptr;
// Written by the render task, consumed by the strip task. Bit 24 marks "written since the
// last frame" so a deliberately black pixel still counts as alive.
inline std::atomic<uint32_t> s_pixels[kMaxLeds] = {};
inline uint16_t s_activeCount = 0; // LEDs actually driven (0 = driver not running), set once in init()
constexpr uint32_t kWrittenFlag = 1u << 24;
} // namespace detail
#else
constexpr bool kEnabled = false;
#endif

inline void load_settings_from_fs() {
    FILE *f = fopen(kSettingsPath, "rb");
    if (f == nullptr) return;
    char buf[128];
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    cJSON *root = cJSON_Parse(buf);
    if (root == nullptr) return;
    auto &s = settings();
    cJSON *pin = cJSON_GetObjectItemCaseSensitive(root, "pin");
    cJSON *count = cJSON_GetObjectItemCaseSensitive(root, "count");
    if (cJSON_IsNumber(pin)) s.pin = static_cast<int8_t>(pin->valueint);
    if (cJSON_IsNumber(count) && count->valueint >= 0)
        s.count = static_cast<uint16_t>(count->valueint > kMaxLeds ? kMaxLeds : count->valueint);
    cJSON_Delete(root);
}

inline void save_settings_to_fs() {
    const auto &s = settings();
    FILE *f = fopen(kSettingsPath, "wb");
    if (f == nullptr) return;
    fprintf(f, "{\"pin\":%d,\"count\":%u}", s.pin, static_cast<unsigned>(s.count));
    fclose(f);
}

#if defined(CONFIG_APP_ENABLE_WS2812)
// LEDs the running driver actually addresses - 0 if the string isn't configured/initialized.
// Settings changes only apply after a reboot, so this can differ from settings().count.
inline uint16_t active_count() { return detail::s_activeCount; }

// Render-task side: lock-free, out-of-range indices are ignored.
inline void set_pixel(int index, uint32_t rgb) {
    if (index < 0 || index >= detail::s_activeCount) return;
    detail::s_pixels[index].store((rgb & 0xFFFFFFu) | detail::kWrittenFlag, std::memory_order_relaxed);
}

inline void strip_task(void *) {
    using namespace detail;
    static uint32_t last[kMaxLeds];
    for (uint16_t i = 0; i < s_activeCount; ++i) last[i] = 0xFFFFFFFFu; // force the first refresh
    for (;;) {
        bool changed = false;
        for (uint16_t i = 0; i < s_activeCount; ++i) {
            const uint32_t v = s_pixels[i].exchange(0, std::memory_order_relaxed);
            const uint32_t rgb = (v & kWrittenFlag) ? (v & 0xFFFFFFu) : 0u;
            if (rgb == last[i]) continue;
            last[i] = rgb;
            changed = true;
            led_strip_set_pixel(s_strip, i, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
        }
        if (changed) led_strip_refresh(s_strip);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// Loads the settings and, if a valid string is configured, starts the RMT driver + strip
// task. A missing/invalid configuration only logs - the rest of the synth runs unaffected.
inline void init() {
    using namespace detail;
    load_settings_from_fs();
    const auto &s = settings();
    if (s.pin < 0 || s.count == 0) {
        ESP_LOGI(TAG, "LED strip not configured");
        return;
    }
    const gpio_num_t pin = static_cast<gpio_num_t>(s.pin);
    if (!GPIO_IS_VALID_OUTPUT_GPIO(pin) || Feature::IO::is_pin_reserved_for_gpio_settings(pin) ||
        Feature::IO::findGpioEntry(s.pin) != nullptr) {
        ESP_LOGW(TAG, "LED strip pin GPIO%d is invalid or already in use", s.pin);
        return;
    }

    led_strip_config_t stripConfig = {};
    stripConfig.strip_gpio_num = s.pin;
    stripConfig.max_leds = s.count;
    stripConfig.led_model = LED_MODEL_WS2812;
    stripConfig.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    led_strip_rmt_config_t rmtConfig = {};
    rmtConfig.clk_src = RMT_CLK_SRC_DEFAULT;
    rmtConfig.resolution_hz = 10 * 1000 * 1000;
    const esp_err_t err = led_strip_new_rmt_device(&stripConfig, &rmtConfig, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "led_strip_new_rmt_device failed: %s", esp_err_to_name(err));
        s_strip = nullptr;
        return;
    }
    led_strip_clear(s_strip);
    s_activeCount = s.count;
    xTaskCreatePinnedToCore(&strip_task, "led_strip", 3072, nullptr, 3, nullptr, 0); // core 0: core 1 belongs to the render task
    ESP_LOGI(TAG, "LED strip init ok: GPIO%d, %u LEDs", s.pin, static_cast<unsigned>(s.count));
}

#else
inline uint16_t active_count() { return 0; }
inline void set_pixel(int, uint32_t) {}
inline void init() {}
#endif

} // namespace Feature::LedStrip
