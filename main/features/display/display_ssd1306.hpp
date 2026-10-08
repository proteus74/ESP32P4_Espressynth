#pragma once

#include "app_core.hpp"
#include "boards/board_init.hpp"
#include "eez_ui_bridge.hpp"
#include "features/display/PortDisplayLabels.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include <algorithm>
#include <cstdlib>
#include <cstdarg>
#include <cstdio>
#include <cstring>

// Plain framebuffer + 5x7 font straight to the panel; no LVGL needed for a tiny status OLED.
namespace Feature::SSD1306 {
constexpr int WIDTH = CONFIG_APP_SSD1306_WIDTH;
constexpr int HEIGHT = CONFIG_APP_SSD1306_HEIGHT;

// Classic 5x7 ASCII font (0x20..0x7e), one byte per column, LSB = top row.
inline constexpr uint8_t FONT[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5f, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00},
    {0x14, 0x7f, 0x14, 0x7f, 0x14}, {0x24, 0x2a, 0x7f, 0x2a, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62},
    {0x36, 0x49, 0x56, 0x20, 0x50}, {0x00, 0x08, 0x07, 0x03, 0x00}, {0x00, 0x1c, 0x22, 0x41, 0x00},
    {0x00, 0x41, 0x22, 0x1c, 0x00}, {0x2a, 0x1c, 0x7f, 0x1c, 0x2a}, {0x08, 0x08, 0x3e, 0x08, 0x08},
    {0x00, 0x80, 0x70, 0x30, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x00, 0x60, 0x60, 0x00},
    {0x20, 0x10, 0x08, 0x04, 0x02}, {0x3e, 0x51, 0x49, 0x45, 0x3e}, {0x00, 0x42, 0x7f, 0x40, 0x00},
    {0x72, 0x49, 0x49, 0x49, 0x46}, {0x21, 0x41, 0x49, 0x4d, 0x33}, {0x18, 0x14, 0x12, 0x7f, 0x10},
    {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3c, 0x4a, 0x49, 0x49, 0x31}, {0x41, 0x21, 0x11, 0x09, 0x07},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x46, 0x49, 0x49, 0x29, 0x1e}, {0x00, 0x00, 0x14, 0x00, 0x00},
    {0x00, 0x40, 0x34, 0x00, 0x00}, {0x00, 0x08, 0x14, 0x22, 0x41}, {0x14, 0x14, 0x14, 0x14, 0x14},
    {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x59, 0x09, 0x06}, {0x3e, 0x41, 0x5d, 0x59, 0x4e},
    {0x7c, 0x12, 0x11, 0x12, 0x7c}, {0x7f, 0x49, 0x49, 0x49, 0x36}, {0x3e, 0x41, 0x41, 0x41, 0x22},
    {0x7f, 0x41, 0x41, 0x41, 0x3e}, {0x7f, 0x49, 0x49, 0x49, 0x41}, {0x7f, 0x09, 0x09, 0x09, 0x01},
    {0x3e, 0x41, 0x41, 0x51, 0x73}, {0x7f, 0x08, 0x08, 0x08, 0x7f}, {0x00, 0x41, 0x7f, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3f, 0x01}, {0x7f, 0x08, 0x14, 0x22, 0x41}, {0x7f, 0x40, 0x40, 0x40, 0x40},
    {0x7f, 0x02, 0x1c, 0x02, 0x7f}, {0x7f, 0x04, 0x08, 0x10, 0x7f}, {0x3e, 0x41, 0x41, 0x41, 0x3e},
    {0x7f, 0x09, 0x09, 0x09, 0x06}, {0x3e, 0x41, 0x51, 0x21, 0x5e}, {0x7f, 0x09, 0x19, 0x29, 0x46},
    {0x26, 0x49, 0x49, 0x49, 0x32}, {0x03, 0x01, 0x7f, 0x01, 0x03}, {0x3f, 0x40, 0x40, 0x40, 0x3f},
    {0x1f, 0x20, 0x40, 0x20, 0x1f}, {0x3f, 0x40, 0x38, 0x40, 0x3f}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x03, 0x04, 0x78, 0x04, 0x03}, {0x61, 0x59, 0x49, 0x4d, 0x43}, {0x00, 0x7f, 0x41, 0x41, 0x41},
    {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x41, 0x7f}, {0x04, 0x02, 0x01, 0x02, 0x04},
    {0x40, 0x40, 0x40, 0x40, 0x40}, {0x00, 0x03, 0x07, 0x08, 0x00}, {0x20, 0x54, 0x54, 0x78, 0x40},
    {0x7f, 0x28, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x28}, {0x38, 0x44, 0x44, 0x28, 0x7f},
    {0x38, 0x54, 0x54, 0x54, 0x18}, {0x00, 0x08, 0x7e, 0x09, 0x02}, {0x18, 0xa4, 0xa4, 0x9c, 0x78},
    {0x7f, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7d, 0x40, 0x00}, {0x20, 0x40, 0x40, 0x3d, 0x00},
    {0x7f, 0x10, 0x28, 0x44, 0x00}, {0x00, 0x41, 0x7f, 0x40, 0x00}, {0x7c, 0x04, 0x78, 0x04, 0x78},
    {0x7c, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38}, {0xfc, 0x18, 0x24, 0x24, 0x18},
    {0x18, 0x24, 0x24, 0x18, 0xfc}, {0x7c, 0x08, 0x04, 0x04, 0x08}, {0x48, 0x54, 0x54, 0x54, 0x24},
    {0x04, 0x04, 0x3f, 0x44, 0x24}, {0x3c, 0x40, 0x40, 0x20, 0x7c}, {0x1c, 0x20, 0x40, 0x20, 0x1c},
    {0x3c, 0x40, 0x30, 0x40, 0x3c}, {0x44, 0x28, 0x10, 0x28, 0x44}, {0x4c, 0x90, 0x90, 0x90, 0x7c},
    {0x44, 0x64, 0x54, 0x4c, 0x44}, {0x00, 0x08, 0x36, 0x41, 0x00}, {0x00, 0x00, 0x77, 0x00, 0x00},
    {0x00, 0x41, 0x36, 0x08, 0x00}, {0x02, 0x01, 0x02, 0x04, 0x02},
};

inline esp_lcd_panel_handle_t panel = nullptr;
// SSD1306 page layout: byte = 8 vertical pixels, pages of WIDTH bytes.
inline uint8_t framebuffer[WIDTH * HEIGHT / 8] = {};
inline volatile bool show_preset = false;

inline void set_pixel(int x, int y, bool on) {
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
    uint8_t& byte = framebuffer[(y >> 3) * WIDTH + x];
    byte = on ? (byte | (1 << (y & 7))) : (byte & ~(1 << (y & 7)));
}

inline void fill_rect(int x, int y, int w, int h, bool on = true) {
    for (int dy = 0; dy < h; ++dy)
        for (int dx = 0; dx < w; ++dx) set_pixel(x + dx, y + dy, on);
}

inline void draw_line(int x0, int y0, int x1, int y1) {
    const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    for (int err = dx + dy;;) {
        set_pixel(x0, y0, true);
        if (x0 == x1 && y0 == y1) return;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// UTF-8 sequences count as one glyph and are drawn as '?' (the font is ASCII only).
inline bool is_glyph_start(char c) { return (static_cast<unsigned char>(c) & 0xc0) != 0x80; }

inline int glyph_count(const char* text) {
    int count = 0;
    for (; *text; ++text) count += is_glyph_start(*text);
    return count;
}

inline int text_width(int glyphs, int scale) { return glyphs > 0 ? glyphs * (5 * scale + 1) - 1 : 0; }

// Draws at most max_glyphs characters; on = false draws dark text on a lit area.
inline void draw_text(int x, int y, const char* text, int scale = 1, int max_glyphs = 255, bool on = true) {
    for (; *text && max_glyphs > 0; ++text) {
        if (!is_glyph_start(*text)) continue;
        unsigned char c = static_cast<unsigned char>(*text);
        if (c < 0x20 || c > 0x7e) c = '?';
        for (int col = 0; col < 5; ++col)
            for (int row = 0; row < 8; ++row)
                if ((FONT[c - 0x20][col] >> row) & 1) fill_rect(x + col * scale, y + row * scale, scale, scale, on);
        x += 5 * scale + 1;
        --max_glyphs;
    }
}

// Centered text; double size when it fits on one line, otherwise wrapped at 1x.
inline void draw_centered(const char* text) {
    const int len = glyph_count(text);
    const int scale = (HEIGHT >= 16 && text_width(len, 2) <= WIDTH) ? 2 : 1;
    const int per_line = (WIDTH + 1) / (5 * scale + 1);
    const int lines = std::min((len + per_line - 1) / per_line, HEIGHT / (8 * scale));
    int top = (HEIGHT - lines * 8 * scale) / 2;
    for (int line = 0; line < lines; ++line, top += 8 * scale) {
        const int chars = std::min(per_line, len - line * per_line);
        draw_text((WIDTH - text_width(chars, scale)) / 2, top, text, scale, chars);
        // Advance past the drawn glyphs, including their UTF-8 continuation bytes.
        for (int skipped = 0; *text; ++text)
            if (is_glyph_start(*text) && skipped++ == chars) break;
    }
}

inline void show_text(const char* text) {
    memset(framebuffer, 0, sizeof(framebuffer));
    draw_centered(text);
    esp_lcd_panel_draw_bitmap(panel, 0, 0, WIDTH, HEIGHT, framebuffer);
}

constexpr int LINES = HEIGHT / 8;
constexpr int COLUMNS = (WIDTH + 1) / 6;

inline void draw_line_fmt(int line, const char* format, ...) {
    if (line >= LINES) return;
    char text[64];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    draw_text(0, line * 8, text, 1, COLUMNS);
}

// Same values as the large display's status bar, one 1x line each.
inline void draw_status(const EezSynthStatus& status, float core0) {
    constexpr size_t mib = 1024 * 1024;
    const unsigned psram_free = static_cast<unsigned>((heap_caps_get_free_size(MALLOC_CAP_SPIRAM) * 10ULL + mib / 2) / mib);
    const unsigned psram_total = static_cast<unsigned>((heap_caps_get_total_size(MALLOC_CAP_SPIRAM) * 10ULL + mib / 2) / mib);
    char ip[24] = {};
    eez_network_ip(ip, sizeof(ip));
    draw_line_fmt(0, "%u %s", static_cast<unsigned>(status.preset_number), status.preset_name);
    draw_line_fmt(1, "%.2f BPM V%u/%u", static_cast<double>(status.master_clock_bpm),
                  status.active_voices, status.maximum_voices);
    draw_line_fmt(2, "C0 %3u%%  C1 %3u%%", static_cast<unsigned>(core0 + 0.5f),
                  static_cast<unsigned>(status.audio_core_load + 0.5f));
    draw_line_fmt(3, "PS %u.%u/%u.%uMB", psram_free / 10, psram_free % 10, psram_total / 10, psram_total % 10);
    draw_line_fmt(4, "RAM %u/%uKB", static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                  static_cast<unsigned>(heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024));
    draw_line_fmt(5, "Vol %u%% XRun %lu", static_cast<unsigned>(status.master_volume * 100.0f + 0.5f),
                  static_cast<unsigned long>(status.underruns));
    draw_line_fmt(6, "%s", status.midi_name[0] != '\0' ? status.midi_name : "Kein MIDI");
    draw_line_fmt(7, "%s", ip);
}

// Preset change: number large first, then the name, then back to the status screen.
constexpr int64_t PRESET_NUMBER_US = 1200000;
constexpr int64_t PRESET_NAME_US = 3000000;

inline void draw_preset_splash(const EezSynthStatus& status, bool number) {
    if (!number) {
        draw_centered(status.preset_name[0] ? status.preset_name : "Kein Preset");
        return;
    }
    char text[8];
    snprintf(text, sizeof(text), "%u", static_cast<unsigned>(status.preset_number));
    const int scale = HEIGHT >= 48 ? 4 : 2;
    draw_text((WIDTH - text_width(6, 1)) / 2, 0, "Preset");
    draw_text((WIDTH - text_width(glyph_count(text), scale)) / 2, (HEIGHT + 8 - 8 * scale) / 2, text, scale);
}

// One control card in a WIDTH x h band: inverted title row, then value, target device and port.
inline void draw_card(const KnobActivity& a, int y, int h) {
    fill_rect(0, y, WIDTH, 8);
    draw_text(1, y, a.name[0] ? a.name : a.sourceId, 1, COLUMNS, false);
    if (a.isEnvelope) {
        // Envelope points are normalized to 0..1000 on both axes.
        const int top = y + 10, bottom = y + h - 1;
        auto px = [](const KnobActivity::Point& p) { return p.x * (WIDTH - 1) / 1000; };
        auto py = [&](const KnobActivity::Point& p) { return bottom - p.y * (bottom - top) / 1000; };
        for (int i = 1; i < 6; ++i) draw_line(px(a.envelope[i - 1]), py(a.envelope[i - 1]), px(a.envelope[i]), py(a.envelope[i]));
        return;
    }
    char value[16];
    if (a.isButton) snprintf(value, sizeof(value), "%.4g", static_cast<double>(a.sourceValue));
    else snprintf(value, sizeof(value), "%u", static_cast<unsigned>(a.raw));
    char node_value[16] = "";
    if (a.targetId[0]) {
        if (a.targetValueRaw) snprintf(node_value, sizeof(node_value), "%.0f", static_cast<double>(a.targetValue));
        else snprintf(node_value, sizeof(node_value), "%.2f V", static_cast<double>(a.targetValue));
    }
    // Full height: 2x value with spacing; half height: four tight 8 px rows.
    const bool large = h >= 64;
    const int scale = large ? 2 : 1;
    const int value_y = y + (large ? 10 : 8);
    draw_text(0, value_y, value, scale, a.isButton ? COLUMNS / scale : 3);
    if (!a.isButton) {
        // 0..127 bar beside the value, like the arc on the large display.
        const int bar_x = text_width(3, scale) + 4, bar_w = WIDTH - bar_x, bar_h = 8 * scale - 2;
        fill_rect(bar_x, value_y, bar_w, bar_h);
        fill_rect(bar_x + 1, value_y + 1, bar_w - 2, bar_h - 2, false);
        fill_rect(bar_x + 2, value_y + 2, (bar_w - 4) * a.raw / 127, bar_h - 4);
    }
    const int device_y = value_y + 8 * scale + (large ? 6 : 0);
    const int port_y = device_y + (large ? 12 : 8);
    draw_text(0, device_y, a.device[0] ? a.device : "Nicht verbunden", 1, COLUMNS);
    const int node_glyphs = glyph_count(node_value);
    draw_text(0, port_y, portlabels::display(static_cast<NodeType>(a.targetType), a.port), 1,
              COLUMNS - node_glyphs - 1);
    draw_text(WIDTH - text_width(node_glyphs, 1), port_y, node_value);
}

inline bool same_control(const KnobActivity& a, const KnobActivity& b) {
    return a.isEnvelope == b.isEnvelope && a.controlIndex == b.controlIndex && strcmp(a.sourceId, b.sourceId) == 0 &&
           strcmp(a.targetId, b.targetId) == 0 && strcmp(a.port, b.port) == 0;
}

// Up to two most recently updated cards, in arrival order. A knob driving an envelope that is
// itself on screen is left out: the envelope card shows the curve instead of the knob.
inline int pick_visible(const KnobActivity* active, int count, int (&visible)[2]) {
    int shown = 0;
    for (int i = 0; i < count; ++i) {
        const KnobActivity& a = active[i];
        bool covered = false;
        for (int j = 0; j < count && !a.isEnvelope && !covered; ++j)
            covered = active[j].isEnvelope && strcmp(active[j].sourceId, a.targetId) == 0;
        if (covered) continue;
        if (shown < 2) { visible[shown++] = i; continue; }
        const int older = active[visible[0]].updatedUs <= active[visible[1]].updatedUs ? 0 : 1;
        if (a.updatedUs <= active[visible[older]].updatedUs) continue;
        if (older == 0) visible[0] = visible[1];
        visible[1] = i;
    }
    return shown;
}

inline void update_task(void*) {
    const int64_t started_us = esp_timer_get_time();
    static uint8_t sent[sizeof(framebuffer)] = {};
    // Recently touched controls, oldest first.
    static KnobActivity active[6] = {};
    static KnobActivity incoming = {};
    int active_count = 0;
    uint32_t preset_sequence = 0;
    int64_t preset_changed_us = 0;
    bool running = false;
    int64_t previous_us = started_us;
    uint32_t previous_idle = ulTaskGetIdleRunTimeCounterForCore(App::MAIN_CORE);
    float core0 = 0.0f;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        const int64_t now_us = esp_timer_get_time();
        // Core 0 load averaged over 500 ms, like the large display's status bar.
        if (now_us - previous_us >= 500000) {
            const uint32_t idle = ulTaskGetIdleRunTimeCounterForCore(App::MAIN_CORE);
            core0 = std::clamp(100.0f * (1.0f - static_cast<float>(idle - previous_idle) /
                                                   static_cast<float>(now_us - previous_us)), 0.0f, 100.0f);
            previous_us = now_us;
            previous_idle = idle;
        }
        EezSynthStatus status = {};
        eez_synth_status(&status);
        if (status.preset_change_sequence != preset_sequence) {
            if (running) preset_changed_us = now_us;
            preset_sequence = status.preset_change_sequence;
            active_count = 0;
        }
        while (oled_read_knob_activity(&incoming)) {
            if (incoming.presetSequence != preset_sequence) continue;
            int slot = 0;
            while (slot < active_count && !same_control(active[slot], incoming)) ++slot;
            if (slot == 6) { // full: drop the oldest
                std::move(active + 1, active + 6, active);
                slot = 5;
            } else if (slot == active_count) {
                ++active_count;
            }
            active[slot] = incoming;
        }
        int write = 0;
        for (int i = 0; i < active_count; ++i)
            if (now_us - active[i].updatedUs < active[i].durationUs()) active[write++] = active[i];
        active_count = write;
        // Keep the startup text for at least three seconds.
        if (!show_preset || now_us - started_us < 3000000) continue;
        running = true;

        int visible[2] = {};
        const int cards = pick_visible(active, active_count, visible);
        memset(framebuffer, 0, sizeof(framebuffer));
        if (cards == 2) {
            draw_card(active[visible[0]], 0, HEIGHT / 2);
            draw_card(active[visible[1]], HEIGHT / 2, HEIGHT / 2);
        } else if (cards == 1) {
            draw_card(active[visible[0]], 0, HEIGHT);
        } else if (preset_changed_us != 0 && now_us - preset_changed_us < PRESET_NAME_US) {
            draw_preset_splash(status, now_us - preset_changed_us < PRESET_NUMBER_US);
        } else {
            draw_status(status, core0);
        }
        if (memcmp(framebuffer, sent, sizeof(sent)) == 0) continue;
        memcpy(sent, framebuffer, sizeof(sent));
        esp_lcd_panel_draw_bitmap(panel, 0, 0, WIDTH, HEIGHT, framebuffer);
    }
}

inline esp_err_t init() {
    constexpr const char* TAG = "SSD1306";
    auto bus = Board::get_i2c_bus_handle(CONFIG_APP_SSD1306_I2C_PORT);
    ESP_RETURN_ON_FALSE(bus != nullptr, ESP_ERR_INVALID_STATE, TAG, "I2C port is disabled");
    uint8_t address = 0x3c;
    if (i2c_master_probe(bus, address, 100) != ESP_OK) address = 0x3d;
    ESP_RETURN_ON_ERROR(i2c_master_probe(bus, address, 100), TAG, "SSD1306 not found at 0x3c/0x3d");
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
    vendor.height = HEIGHT;
    esp_lcd_panel_dev_config_t config = {};
    config.reset_gpio_num = GPIO_NUM_NC;
    config.bits_per_pixel = 1;
    config.vendor_config = &vendor;
    esp_err_t err = esp_lcd_new_panel_ssd1306(io, &config, &panel);
    if (err == ESP_OK) err = esp_lcd_panel_reset(panel);
    if (err == ESP_OK) err = esp_lcd_panel_init(panel);
    // No reset line: the panel keeps inverse mode across ESP reboots unless cleared explicitly.
    if (err == ESP_OK) err = esp_lcd_panel_invert_color(panel, false);
#if defined(CONFIG_APP_SSD1306_64X48)
    // Common 64x48 modules expose the middle 64 columns and use alternate COM pins.
    const uint8_t com_pins = 0x12;
    if (err == ESP_OK) err = esp_lcd_panel_io_tx_param(io, 0xda, &com_pins, 1);
    if (err == ESP_OK) err = esp_lcd_panel_set_gap(panel, 32, 0);
#endif
    if (err == ESP_OK) err = esp_lcd_panel_disp_on_off(panel, true);
    if (err == ESP_OK) show_text("Espressynth");
    if (err == ESP_OK && xTaskCreatePinnedToCore(update_task, "oled", 4096, nullptr, 2, nullptr, App::MAIN_CORE) != pdPASS)
        err = ESP_ERR_NO_MEM;
    if (err != ESP_OK) {
        if (panel != nullptr) esp_lcd_panel_del(panel);
        panel = nullptr;
        esp_lcd_panel_io_del(io);
        return err;
    }
    ESP_LOGI(TAG, "OLED ready: %dx%d on I2C%d, address 0x%02x", WIDTH, HEIGHT,
             CONFIG_APP_SSD1306_I2C_PORT, address);
    return ESP_OK;
}

inline void finish_startup_screen() { show_preset = true; }
} // namespace Feature::SSD1306
