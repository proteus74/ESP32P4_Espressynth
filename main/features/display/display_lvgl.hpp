#pragma once

#include "app_core.hpp"
#include "boards/board_config.hpp"
#include "boards/board_init.hpp"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
#if defined(CONFIG_APP_DISPLAY_OUTPUT_RGB)
#include "crow_rgb_lcd.h"
#include "driver/ppa.h" // rgb_flush_dma()
#endif
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_netif_ip_addr.h"
#include "esp_netif_types.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "eez_ui_bridge.hpp"
#if defined(CONFIG_APP_DISPLAY_OUTPUT_SPI)
#include "display_spi.hpp"
#endif
#if defined(CONFIG_APP_DISPLAY_MIPI_JD9165)
    #include "esp_lcd_jd9165.h"
    #include "features/display/jd9165_panel_init.h"
#else
    #include "esp_lcd_jd9365.h"
    #include "features/display/jd9365_init.h"
#endif
#if defined(CONFIG_BOARD_JC1060P470C_7_INCH) || defined(CONFIG_BOARD_CROWPANEL_5INCH)
    // JC1060P470C 7" (JD9165) and CrowPanel 5" (RGB): GT911 touch, natively landscape (no rotation).
    #include "esp_lcd_touch_gt911.h"
#else
    // JC8012P4A1 and friends: JD9365 panel, GSL3680 touch, portrait panel driven landscape.
    #include "esp_lcd_gsl3680.h"
    // The rotating flush path below offloads the per-frame rotation to the ESP32-P4's PPA
    // (Pixel Processing Accelerator) hardware instead of LVGL's software rotator
    // (lv_draw_sw_rotate()), which was pegging taskLVGL near 100% transposing the whole
    // 1280x800 RGB565 frame on the CPU every flush.
    #include "driver/ppa.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
// Private on purpose (see draw/lv_draw_buf.h's lv_draw_buf_get_image_handlers() doc comment:
// "Custom callback can be set on the returned value") - this is LVGL's own documented mechanism
// for redirecting decoded-image buffers to a different allocator; see the comment above
// image_cache_buf_malloc() below for why this project needs it.
#include "draw/lv_draw_buf_private.h"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_timer.h"
#include "lwip/ip4_addr.h"

// Boot splash: /www/static/img/splash.jpg, decoded by the P4's hardware JPEG engine (LVGL
// itself only has a PNG decoder here). Needs the www partition (web server feature).
#if defined(CONFIG_APP_ENABLE_WEBSERVER) && defined(CONFIG_SOC_JPEG_DECODE_SUPPORTED)
    #define APP_BOOT_SPLASH 1
    #include "driver/jpeg_decode.h"
    #include "misc/cache/instance/lv_image_cache.h"
#else
    #define APP_BOOT_SPLASH 0
#endif

#if defined(CONFIG_APP_ENABLE_WIFI)
    #include "features/wifi/wifi_manager.hpp"
#endif
#if defined(CONFIG_APP_ENABLE_ETHERNET)
    #include "features/ethernet/ethernet_manager.hpp"
#endif
#if defined(CONFIG_APP_ENABLE_SYNTH)
    #include "features/synth/synth_manager.hpp"
#endif
#if defined(CONFIG_APP_ENABLE_USB_HOST) && defined(CONFIG_USB_HOST_CLASS_MIDI)
    #include "features/usb/host/usb_midi_host.hpp"
#endif
#if defined(CONFIG_APP_ENABLE_BLUETOOTH) && defined(CONFIG_BLE_ENABLE_MIDI_HOST)
    #include "features/ble/ble_midi_host.hpp"
#endif

// USB and BLE HID (keyboard/mouse) both feed the same LVGL input devices below. Every
// block gated on this condition compiles in if either transport's HID host is enabled:
// "#if (USB_HOST + USB_HOST_CLASS_HID) || (BLUETOOTH + BLE_ENABLE_HID_HOST)"

namespace Feature::LVGL {

namespace {

#if defined(CONFIG_BOARD_JC1060P470C_7_INCH) || defined(CONFIG_BOARD_CROWPANEL_5INCH)
// LVGL renders straight into the panel's orientation here, so the panel driver's own
// double buffer is all that's needed.
constexpr size_t FRAME_BUFFER_COUNT = 2;
#else
// The rotating flush path cycles through the DSI frame buffers itself.
constexpr size_t FRAME_BUFFER_COUNT = 3;
#endif
static esp_lcd_panel_handle_t s_panel = nullptr;
#if defined(CONFIG_APP_ENABLE_MIPI_DSI) && !defined(CONFIG_BOARD_JC1060P470C_7_INCH)
static SemaphoreHandle_t s_dpi_vsync = nullptr;
static void* s_frame_buffers[FRAME_BUFFER_COUNT] = {};
static ppa_client_handle_t s_ppa_srm_client = nullptr;
static uint8_t* s_lvgl_frame_buffer = nullptr;
static uint8_t s_next_frame_buffer = 1;
#endif
static esp_lcd_touch_handle_t s_touch_handle = nullptr;
#if APP_BOOT_SPLASH
static std::atomic<bool> s_splash_frame_pending{false};
static std::atomic<TickType_t> s_splash_visible_since{0};
static lv_obj_t* s_splash_background = nullptr;
static lv_obj_t* s_splash_image = nullptr;
#endif
static lv_obj_t* s_ip_label = nullptr;
static lv_obj_t* s_memory_label = nullptr;
static lv_obj_t* s_cpu_label = nullptr;
static lv_obj_t* s_midi_label = nullptr;
static lv_obj_t* s_underrun_label = nullptr;
static lv_obj_t* s_preset_label = nullptr;
static lv_obj_t* s_preset_index_label = nullptr;
static lv_obj_t* s_activity_panel = nullptr;
static lv_obj_t* s_activity_label = nullptr;
static lv_timer_t* s_ui_timer = nullptr;
static uint32_t s_seen_midi_sequence = 0;
static uint32_t s_seen_preset_change_sequence = 0;
static int64_t s_activity_until_us = 0;
static constexpr size_t kPresetCapacity = 32;
static constexpr size_t kPresetNameCapacity = 64;
static char s_preset_names[kPresetCapacity][kPresetNameCapacity] = {};
static size_t s_preset_count = 0;
static size_t s_preset_index = 0;
static bool s_preset_list_loaded = false;

#if (defined(CONFIG_APP_ENABLE_USB_HOST) && defined(CONFIG_USB_HOST_CLASS_HID)) || \
    (defined(CONFIG_APP_ENABLE_BLUETOOTH) && defined(CONFIG_BLE_ENABLE_HID_HOST))
// USB and BLE HID mouse reports arrive as relative deltas from other tasks, so the
// accumulated pointer state is atomic.
std::atomic<int32_t> s_mouse_x{0};
std::atomic<int32_t> s_mouse_y{0};
std::atomic<bool> s_mouse_pressed{false};
int32_t s_mouse_max_x = 0;
int32_t s_mouse_max_y = 0;
lv_obj_t* s_mouse_cursor = nullptr;

void mouse_read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    (void)indev;
    data->point.x = static_cast<int32_t>(s_mouse_x.load(std::memory_order_relaxed));
    data->point.y = static_cast<int32_t>(s_mouse_y.load(std::memory_order_relaxed));
    data->state = s_mouse_pressed.load(std::memory_order_relaxed)
                      ? LV_INDEV_STATE_PRESSED
                      : LV_INDEV_STATE_RELEASED;
}

// Physical keyboard (USB or BLE) key events, mapped to LVGL key codes by app_events.hpp
// and fed in one at a time -- same single-active-value pattern as the mouse above.
std::atomic<uint32_t> s_keypad_key{0};
std::atomic<bool> s_keypad_pressed{false};
lv_group_t* s_input_group = nullptr;

void keypad_read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    (void)indev;
    data->key = s_keypad_key.load(std::memory_order_relaxed);
    data->state = s_keypad_pressed.load(std::memory_order_relaxed)
                      ? LV_INDEV_STATE_PRESSED
                      : LV_INDEV_STATE_RELEASED;
}
#endif

#if defined(CONFIG_APP_ENABLE_SYNTH)
void refresh_preset_list()
{
    s_preset_count = 1;
    snprintf(s_preset_names[0], sizeof(s_preset_names[0]), "Current");

    DIR* dir = opendir(presetpaths::kDir);
    if (dir == nullptr) return;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr && s_preset_count < kPresetCapacity)
    {
        const char* name = entry->d_name;
        const size_t length = strlen(name);
        if (name[0] == '.' || length < 6 || strcmp(name, "current.json") == 0 ||
            strcmp(name + length - 5, ".json") != 0) continue;
        char preset_name[kPresetNameCapacity] = {};
        snprintf(preset_name, sizeof(preset_name), "%.*s", static_cast<int>(length - 5), name);
        if (!presetpaths::isSafeName(preset_name)) continue;
        snprintf(s_preset_names[s_preset_count], sizeof(s_preset_names[0]), "%s", preset_name);
        ++s_preset_count;
    }
    closedir(dir);
    s_preset_list_loaded = true;

    char active_path[300] = {};
    char active_name[kPresetNameCapacity] = {};
    presetpaths::resolveActivePath(active_path, sizeof(active_path), active_name, sizeof(active_name));
    s_preset_index = 0;
    for (size_t i = 1; i < s_preset_count; ++i)
    {
        if (strcmp(active_name, s_preset_names[i]) == 0)
        {
            s_preset_index = i;
            break;
        }
    }
}

void update_preset_labels()
{
    if (s_preset_label == nullptr || s_preset_count == 0) return;
    lv_label_set_text(s_preset_label, s_preset_names[s_preset_index]);
    lv_label_set_text_fmt(s_preset_index_label, "%u / %u", static_cast<unsigned>(s_preset_index + 1),
                          static_cast<unsigned>(s_preset_count));
}

void select_preset(int direction)
{
    if (s_preset_count == 0) return;
    size_t target_index = s_preset_index;
    if (direction < 0)
        target_index = target_index == 0 ? s_preset_count - 1 : target_index - 1;
    else
        target_index = (target_index + 1) % s_preset_count;

    if (target_index == 0)
        Feature::Synth::request_current_preset();
    else
        Feature::Synth::request_preset_name(s_preset_names[target_index]);
}

void on_previous_preset(lv_event_t*) { select_preset(-1); }
void on_next_preset(lv_event_t*) { select_preset(1); }
void on_panic(lv_event_t*) { Feature::Synth::engine().requestPanic(); }
#else
void on_previous_preset(lv_event_t*) {}
void on_next_preset(lv_event_t*) {}
void on_panic(lv_event_t*) {}
#endif

void update_network_label()
{
    if (s_ip_label == nullptr) return;
    char ip_text[24] = "--";
    esp_netif_ip_info_t info{};
#if defined(CONFIG_APP_ENABLE_ETHERNET)
    if (Feature::Ethernet::get_ip_info(info) == ESP_OK && info.ip.addr != 0)
        ip4addr_ntoa_r(reinterpret_cast<const ip4_addr_t*>(&info.ip), ip_text, sizeof(ip_text));
#endif
#if defined(CONFIG_APP_ENABLE_WIFI)
    if (strcmp(ip_text, "--") == 0 && Feature::WiFi::get_ip_info(info) == ESP_OK && info.ip.addr != 0)
        ip4addr_ntoa_r(reinterpret_cast<const ip4_addr_t*>(&info.ip), ip_text, sizeof(ip_text));
#endif
    lv_label_set_text_fmt(s_ip_label, "IP  %s", ip_text);
}

void update_midi_label()
{
    if (s_midi_label == nullptr) return;
    const char* text = "Kein MIDI";
#if defined(CONFIG_APP_ENABLE_USB_HOST) && defined(CONFIG_USB_HOST_CLASS_MIDI)
    if (Feature::MIDI::is_connected()) text = "USB MIDI";
#endif
#if defined(CONFIG_APP_ENABLE_BLUETOOTH) && defined(CONFIG_BLE_ENABLE_MIDI_HOST)
    if (Feature::BLE::MIDI::connected_count() > 0) text = "BLE MIDI";
#endif
    lv_label_set_text(s_midi_label, text);
}

void update_runtime_labels()
{
    update_network_label();
    update_midi_label();
#if defined(CONFIG_APP_ENABLE_SYNTH)
    const uint32_t preset_sequence = Feature::Synth::engine().presetChangeSequence();
    if (preset_sequence != 0 && preset_sequence != s_seen_preset_change_sequence)
    {
        s_seen_preset_change_sequence = preset_sequence;
        static char selected_name[kPresetNameCapacity] = {};
        Feature::Synth::engine().readCurrentPresetName(selected_name, sizeof(selected_name));
        refresh_preset_list();
        s_preset_index = 0;
        for (size_t i = 1; i < s_preset_count; ++i)
        {
            if (strcmp(selected_name, s_preset_names[i]) == 0)
            {
                s_preset_index = i;
                break;
            }
        }
        update_preset_labels();
    }
    if (!s_preset_list_loaded)
    {
        refresh_preset_list();
        update_preset_labels();
    }
    const unsigned psram_free = static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    const unsigned psram_total = static_cast<unsigned>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024);
    const unsigned internal_free = static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    const unsigned internal_total = static_cast<unsigned>(heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024);
    lv_label_set_text_fmt(s_memory_label, "PSRAM  %u / %u KB\nSystem  %u / %u KB", psram_free, psram_total,
                          internal_free, internal_total);

    static int64_t previous_sample_us = 0;
    static uint32_t previous_idle_runtime = 0;
    const int64_t now_us = esp_timer_get_time();
    const uint32_t idle_runtime = ulTaskGetIdleRunTimeCounterForCore(0);
    float core0 = 0.0f;
    if (previous_sample_us != 0 && now_us > previous_sample_us)
    {
        core0 = 100.0f * (1.0f - static_cast<float>(idle_runtime - previous_idle_runtime) /
                                   static_cast<float>(now_us - previous_sample_us));
        if (core0 < 0.0f) core0 = 0.0f;
        if (core0 > 100.0f) core0 = 100.0f;
    }
    previous_sample_us = now_us;
    previous_idle_runtime = idle_runtime;
    const float core1 = Feature::Synth::engine().audioCoreLoadPercent();
    const unsigned core0_tenths = static_cast<unsigned>(core0 * 10.0f + 0.5f);
    const unsigned core1_tenths = static_cast<unsigned>(core1 * 10.0f + 0.5f);
    lv_label_set_text_fmt(s_cpu_label, "Core 0  %u.%u %%\nCore 1  %u.%u %%",
                          core0_tenths / 10, core0_tenths % 10,
                          core1_tenths / 10, core1_tenths % 10);
    const SynthEngine::MidiVoiceUsageSnapshot voice_usage = Feature::Synth::engine().midiVoiceUsage();
    // Real DMA-ring underrun count (Feature::Synth::dma_underrun_count()), not
    // SynthEngine::renderOverrunCount() - see that function's comment in synth_manager.hpp.
    lv_label_set_text_fmt(s_underrun_label, "Voices  %u/%u\nUnderruns  %lu",
                          static_cast<unsigned>(voice_usage.active),
                          static_cast<unsigned>(voice_usage.maximum),
                          static_cast<unsigned long>(Feature::Synth::dma_underrun_count()));

    SynthEngine::MidiControlSnapshot activity{};
    if (Feature::Synth::engine().readMidiControlSnapshot(activity) && activity.sequence != s_seen_midi_sequence)
    {
        s_seen_midi_sequence = activity.sequence;
        s_activity_until_us = now_us + 2'000'000;
        const int32_t value_hundredths = activity.value >= 0.0f
                                             ? static_cast<int32_t>(activity.value * 100.0f + 0.5f)
                                             : static_cast<int32_t>(activity.value * 100.0f - 0.5f);
        const int32_t value_whole = value_hundredths / 100;
        const unsigned value_fraction = static_cast<unsigned>(
            value_hundredths < 0 ? -value_hundredths : value_hundredths) % 100;
        const unsigned raw = static_cast<unsigned>(activity.raw + 0.5f);
        switch (activity.kind)
        {
        case SynthEngine::MidiControlSnapshot::Kind::Button:
            lv_label_set_text_fmt(s_activity_label, "CC-Button\n%s\nCC %u  CH %u\nWert  %d.%02u  (MIDI %u)",
                                  activity.widgetLabel, static_cast<unsigned>(activity.cc),
                                  static_cast<unsigned>(activity.channel + 1), static_cast<int>(value_whole),
                                  value_fraction, raw);
            break;
        case SynthEngine::MidiControlSnapshot::Kind::UpDownButton:
            lv_label_set_text_fmt(s_activity_label, "CC-Up/Down\n%s\n%s  CC %u  CH %u\nWert  %d.%02u  (MIDI %u)",
                                  activity.widgetLabel, activity.direction == 1 ? "Hoch" : "Runter",
                                  static_cast<unsigned>(activity.cc), static_cast<unsigned>(activity.channel + 1),
                                  static_cast<int>(value_whole), value_fraction, raw);
            break;
        case SynthEngine::MidiControlSnapshot::Kind::Switch:
            lv_label_set_text_fmt(s_activity_label, "MIDI-Switch\n%s\nCC %u  CH %u\n%s  (MIDI %u)",
                                  activity.widgetLabel, static_cast<unsigned>(activity.cc),
                                  static_cast<unsigned>(activity.channel + 1),
                                  activity.value >= 0.5f ? "In 2" : "In 1", raw);
            break;
        case SynthEngine::MidiControlSnapshot::Kind::Knob:
        default:
            lv_label_set_text_fmt(s_activity_label, "MIDI-Knopf\n%s\nCC %u  CH %u\nWert  %d.%02u  (MIDI %u)",
                                  activity.widgetLabel, static_cast<unsigned>(activity.cc),
                                  static_cast<unsigned>(activity.channel + 1), static_cast<int>(value_whole),
                                  value_fraction, raw);
            break;
        }
        lv_obj_clear_flag(s_activity_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_activity_panel);
    }
    if (s_activity_panel != nullptr && s_activity_until_us != 0 && now_us >= s_activity_until_us)
        lv_obj_add_flag(s_activity_panel, LV_OBJ_FLAG_HIDDEN);
#endif
}

void ui_update_timer(lv_timer_t*) { update_runtime_labels(); }

lv_obj_t* create_card(lv_obj_t* parent, int x, int y, int width, int height, const char* title,
                      lv_obj_t** value_out)
{
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, width, height);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x182231), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x33465D), 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* title_label = lv_label_create(card);
    lv_label_set_text(title_label, title);
    lv_obj_set_pos(title_label, 14, 10);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0x8EA5BE), 0);
    *value_out = lv_label_create(card);
    lv_obj_set_pos(*value_out, 14, 36);
    lv_obj_set_style_text_color(*value_out, lv_color_hex(0xF3F7FC), 0);
    return card;
}

#if defined(CONFIG_APP_ENABLE_MIPI_DSI) && !defined(CONFIG_BOARD_JC1060P470C_7_INCH)
void calibrate_touch_coordinates(esp_lcd_touch_handle_t touch, uint16_t* x,
                                 uint16_t* y, uint16_t* strength,
                                 uint8_t* point_count, uint8_t max_points) {
    (void)touch;
    (void)strength;
    const uint8_t count = (*point_count < max_points) ? *point_count : max_points;
    for (uint8_t i = 0; i < count; ++i) {
        const uint16_t raw_x = (x[i] < 9) ? 9 : ((x[i] > 789) ? 789 : x[i]);
        const uint16_t raw_y = (y[i] < 15) ? 15 : ((y[i] > 1273) ? 1273 : y[i]);
        const uint16_t portrait_x = static_cast<uint16_t>(
            (789 - raw_x) * (CurrentBoard::DISPLAY_NATIVE_WIDTH - 1) / (789 - 9));
        const uint16_t portrait_y = static_cast<uint16_t>(
            (raw_y - 15) * (CurrentBoard::DISPLAY_NATIVE_HEIGHT - 1) / (1273 - 15));

        // The rendered framebuffer is rotated into the panel's native portrait
        // orientation. Convert the portrait touch point back to LVGL landscape.
        x[i] = portrait_y;
        y[i] = static_cast<uint16_t>(CurrentBoard::DISPLAY_NATIVE_WIDTH - 1 - portrait_x);
    }
}
#endif

// LVGL's builtin allocator (CONFIG_LV_MEM_SIZE_KILOBYTES) is a fixed static array in plain
// internal RAM (src/stdlib/builtin/lv_mem_core_builtin.c: LV_ATTRIBUTE_LARGE_RAM_ARRAY is a
// no-op unless CONFIG_LV_ATTRIBUTE_LARGE_RAM_ARRAY names a PSRAM section, which nothing in this
// project does) - only 64 KiB, meant for everyday widget/style allocations. Decoded image
// buffers go through a *separate* handler LVGL keeps architecturally apart for exactly this
// reason (lv_draw_buf_get_image_handlers()), but by default it still calls the same tiny
// builtin allocator. A decoded 1024x600 ARGB8888 PNG (the boot splash) needs ~2.4 MB, so it
// always failed there - confirmed on real hardware via lv_lodepng's own log: "decoder_open:
// error 83: memory allocation failed" on every single redraw attempt, the image silently never
// drawing (no ESP_LOG at all - LV_USE_LOG was off until this was diagnosed). Redirect just this
// one handler to PSRAM (32 MB available) via heap_caps_malloc(). Lodepng's separate file and
// inflate allocations use the 4 MB PSRAM-backed LVGL pool configured in CMakeLists.txt.
void* image_cache_buf_malloc(size_t size, lv_color_format_t color_format) {
    (void)color_format;
    void* buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == nullptr) {
        buf = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return buf;
}

void image_cache_buf_free(void* draw_buf) {
    heap_caps_free(draw_buf);
}

void touch_read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    (void)indev;
    data->state = LV_INDEV_STATE_RELEASED;
    if (s_touch_handle == nullptr) {
        return;
    }

    uint16_t x = 0;
    uint16_t y = 0;
    uint8_t point_count = 0;
    if (esp_lcd_touch_read_data(s_touch_handle) == ESP_OK &&
        esp_lcd_touch_get_coordinates(s_touch_handle, &x, &y, nullptr,
                                      &point_count, 1) && point_count > 0) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
        static int last_logged_x = -1;
        static int last_logged_y = -1;
        if (last_logged_x != x || last_logged_y != y) {
            // ESP_LOGI("FeatureLVGL", "Touch position: x=%u y=%u", x, y);
            last_logged_x = x;
            last_logged_y = y;
        }
    }
}

#if defined(CONFIG_APP_ENABLE_MIPI_DSI) && !defined(CONFIG_BOARD_JC1060P470C_7_INCH)
IRAM_ATTR bool on_dpi_vsync(esp_lcd_panel_handle_t panel,
                            esp_lcd_dpi_panel_event_data_t* event_data,
                            void* user_ctx) {
    (void)panel;
    (void)event_data;
    (void)user_ctx;
    if (s_dpi_vsync == nullptr) {
        return false;
    }
    BaseType_t high_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_dpi_vsync, &high_task_woken);
    return high_task_woken == pdTRUE;
}
#endif

#if defined(CONFIG_APP_DISPLAY_OUTPUT_RGB)

// CrowPanel RGB pipeline (ESP32-P4 rev < 3, see components/crow_rgb_lcd):
//  - LVGL renders into two strip buffers in internal RAM; this flush callback copies them straight
//    into the driver's single PSRAM frame buffer, which the LCD DMA streams out continuously (no
//    bounce buffers: their refill ISR costs a whole CPU core on the P4).
//  - esp_lcd_panel_draw_bitmap() is bypassed on purpose: in stream mode it re-links the running
//    GDMA descriptor chain on every call, which only adds risk to an already fragile scan-out.
//  - The PSRAM traffic of a redraw can still knock the scan-out out of sync (this chip revision
//    has no LCD underrun detection), so every finished frame requests a restart in the next
//    vertical blank. The reduced pixel clock lowers PSRAM scan-out demand.
constexpr int RGB_DRAW_BUFFER_LINES = 8; // 2 strips x 8 lines x 800 px x 2 B = 25.6 KB internal RAM (was 16 lines = 51 KB)
static uint8_t* s_rgb_frame_buffer = nullptr;
// Copies each strip into the PSRAM frame buffer by DMA (PPA SRM at scale 1, no rotation).
// A CPU memcpy would stream the whole redraw through the L2 cache both cores share and evict
// the audio render task's code/data on core 1 - measured as render blocks taking up to 2x
// longer (Audio-Overrun) while knob overlays redraw. nullptr = CPU fallback.
static ppa_client_handle_t s_rgb_ppa_client = nullptr;

bool rgb_flush_dma(const lv_area_t* area, uint8_t* px_map) {
    if (s_rgb_ppa_client == nullptr) return false;
    ppa_srm_oper_config_t copy = {};
    copy.in.buffer = px_map;
    copy.in.pic_w = lv_area_get_width(area);
    copy.in.pic_h = lv_area_get_height(area);
    copy.in.block_w = copy.in.pic_w;
    copy.in.block_h = copy.in.pic_h;
    copy.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    copy.out.buffer = s_rgb_frame_buffer;
    copy.out.buffer_size = CurrentBoard::DISPLAY_NATIVE_WIDTH * CurrentBoard::DISPLAY_NATIVE_HEIGHT * sizeof(uint16_t);
    copy.out.pic_w = CurrentBoard::DISPLAY_NATIVE_WIDTH;
    copy.out.pic_h = CurrentBoard::DISPLAY_NATIVE_HEIGHT;
    copy.out.block_offset_x = area->x1;
    copy.out.block_offset_y = area->y1;
    copy.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    copy.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    copy.scale_x = 1.0f;
    copy.scale_y = 1.0f;
    copy.mode = PPA_TRANS_MODE_BLOCKING;
    const esp_err_t err = ppa_do_scale_rotate_mirror(s_rgb_ppa_client, &copy);
    if (err != ESP_OK) {
        ESP_LOGW("FeatureLVGL", "PPA flush failed (%s), using CPU copy", esp_err_to_name(err));
        s_rgb_ppa_client = nullptr; // chisle: leaks the client, but this path is a one-time fallback
        return false;
    }
    return true;
}

static volatile bool s_rgb_restart_pending = false;
static volatile uint32_t s_rgb_last_flush_tick = 0;
static uint32_t s_rgb_last_restart_tick = 0;
constexpr uint32_t RGB_RESTART_QUIET_MS = 120;  // redraws idle this long -> resync now
constexpr uint32_t RGB_RESTART_MAX_MS = 600;    // during endless bursts still resync this often

void rgb_restart_timer_cb(lv_timer_t*) {
    if (!s_rgb_restart_pending) return;
    const uint32_t now = lv_tick_get();
    const bool quiet = lv_tick_elaps(s_rgb_last_flush_tick) >= RGB_RESTART_QUIET_MS;
    const bool overdue = lv_tick_elaps(s_rgb_last_restart_tick) >= RGB_RESTART_MAX_MS;
    if (!quiet && !overdue) return;
    s_rgb_restart_pending = false;
    s_rgb_last_restart_tick = now;
    crow_lcd_rgb_panel_restart(s_panel);
}

void rgb_flush_cb(lv_display_t* display, const lv_area_t* area, uint8_t* px_map) {
    constexpr int width = CurrentBoard::DISPLAY_NATIVE_WIDTH;
    constexpr size_t bytes_per_pixel = sizeof(uint16_t);
    const size_t row_bytes = static_cast<size_t>(lv_area_get_width(area)) * bytes_per_pixel;
    // CPU fallback. The LCD DMA reads PSRAM directly, so each freshly written row is pushed
    // out of the cache.
    if (!rgb_flush_dma(area, px_map)) {
        for (int32_t y = area->y1; y <= area->y2; ++y) {
            uint8_t* dst = s_rgb_frame_buffer + (static_cast<size_t>(y) * width + area->x1) * bytes_per_pixel;
            std::memcpy(dst, px_map + static_cast<size_t>(y - area->y1) * row_bytes, row_bytes);
            esp_cache_msync(dst, row_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        }
    }
#if APP_BOOT_SPLASH
    if (lv_display_flush_is_last(display) &&
        s_splash_frame_pending.exchange(false, std::memory_order_acq_rel)) {
        s_splash_visible_since.store(xTaskGetTickCount(), std::memory_order_release);
    }
#endif
    if (lv_display_flush_is_last(display)) {
        // Redraw traffic can knock the P4 scan-out out of sync. Resync is requested by
        // rgb_restart_timer_cb() once redraws settle: a restart per frame resets the LCD timing
        // generator and makes the panel flicker during bursts (many sliders moving at once).
        s_rgb_last_flush_tick = lv_tick_get();
        s_rgb_restart_pending = true;
    }
    lv_display_flush_ready(display);
}

// Must be called with the LVGL port lock held (lvgl_port_init() already done).
lv_display_t* create_rgb_display(esp_lcd_panel_handle_t panel) {
    void* frame_buffer = nullptr;
    if (crow_lcd_rgb_panel_get_frame_buffer(panel, 1, &frame_buffer) != ESP_OK || frame_buffer == nullptr) {
        return nullptr;
    }
    s_rgb_frame_buffer = static_cast<uint8_t*>(frame_buffer);
    ppa_client_config_t ppa_client_config = {};
    ppa_client_config.oper_type = PPA_OPERATION_SRM;
    if (ppa_register_client(&ppa_client_config, &s_rgb_ppa_client) != ESP_OK) {
        s_rgb_ppa_client = nullptr;
        ESP_LOGW("FeatureLVGL", "PPA client unavailable, RGB flush uses CPU copy");
    }

    const size_t draw_buffer_bytes = static_cast<size_t>(CurrentBoard::DISPLAY_NATIVE_WIDTH) *
                                     RGB_DRAW_BUFFER_LINES * sizeof(uint16_t);
    void* draw_buffer_a = heap_caps_aligned_alloc(64, draw_buffer_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    void* draw_buffer_b = heap_caps_aligned_alloc(64, draw_buffer_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (draw_buffer_a == nullptr || draw_buffer_b == nullptr) {
        heap_caps_free(draw_buffer_a);
        heap_caps_free(draw_buffer_b);
        return nullptr;
    }

    lv_display_t* display = lv_display_create(CurrentBoard::DISPLAY_NATIVE_WIDTH,
                                              CurrentBoard::DISPLAY_NATIVE_HEIGHT);
    if (display == nullptr) {
        heap_caps_free(draw_buffer_a);
        heap_caps_free(draw_buffer_b);
        return nullptr;
    }
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_buffer_a, draw_buffer_b, draw_buffer_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, rgb_flush_cb);
    lv_timer_create(rgb_restart_timer_cb, 40, nullptr);
    return display;
}
#endif // CONFIG_APP_DISPLAY_OUTPUT_RGB

#if defined(CONFIG_APP_ENABLE_MIPI_DSI)
#if defined(CONFIG_BOARD_JC1060P470C_7_INCH)

#if APP_BOOT_SPLASH
void on_direct_flush_finished(lv_event_t* event) {
    auto* display = static_cast<lv_display_t*>(lv_event_get_target(event));
    // The DSI port returns from the final flush only after the buffer switch.
    if (lv_display_flush_is_last(display) &&
        s_splash_frame_pending.exchange(false, std::memory_order_acq_rel)) {
        s_splash_visible_since.store(xTaskGetTickCount(), std::memory_order_release);
    }
}
#endif

lv_display_t* create_direct_display(esp_lcd_panel_handle_t panel) {
    // Reuse the two DPI framebuffers in PSRAM. DIRECT redraws/synchronizes only dirty
    // areas; screen changes render in one pass, without copying visible half-frames.
    // The port flips on the final flush and waits for on_frame_buf_complete before
    // LVGL can reuse the old front buffer. No extra strip buffers are allocated.
    lvgl_port_display_cfg_t config = {};
    config.panel_handle = panel;
    config.hres = CurrentBoard::DISPLAY_NATIVE_WIDTH;
    config.vres = CurrentBoard::DISPLAY_NATIVE_HEIGHT;
    config.buffer_size = config.hres * config.vres;
    config.double_buffer = true;
    config.color_format = LV_COLOR_FORMAT_RGB565;
    config.flags.direct_mode = true;
    lvgl_port_display_dsi_cfg_t dsi_config = {};
    dsi_config.flags.avoid_tearing = true;
    lv_display_t* display = lvgl_port_add_disp_dsi(&config, &dsi_config);
#if APP_BOOT_SPLASH
    if (display != nullptr) {
        lv_display_add_event_cb(display, on_direct_flush_finished, LV_EVENT_FLUSH_FINISH, nullptr);
    }
#endif
    return display;
}

#else

void flush_cb(lv_display_t* display, const lv_area_t* area, uint8_t* px_map) {
    (void)display;
    (void)area;

    // The BSP uses full-screen buffers and changes the active buffer only at
    // VSYNC. This prevents the DSI engine from scanning while LVGL is writing
    // the same buffer, which otherwise produces horizontal tearing/stripes.
    void* next_frame_buffer = s_frame_buffers[s_next_frame_buffer];

    // Hardware rotation via the PPA SRM engine, replacing lv_draw_sw_rotate(). Angle
    // PPA_SRM_ROTATION_ANGLE_270 (counter-clockwise) produces the identical pixel mapping
    // the old LV_DISPLAY_ROTATION_270 software rotate did (270 deg CCW == 90 deg CW):
    // src(row=y, col=x) in the 1280x800 landscape render buffer lands at
    // dst(row=x, col=DISPLAY_NATIVE_WIDTH-1-y) in the 800x1280 portrait panel buffer.
    ppa_srm_oper_config_t rotate_config = {};
    rotate_config.in.buffer = px_map;
    rotate_config.in.pic_w = CurrentBoard::DISPLAY_NATIVE_HEIGHT;
    rotate_config.in.pic_h = CurrentBoard::DISPLAY_NATIVE_WIDTH;
    rotate_config.in.block_w = CurrentBoard::DISPLAY_NATIVE_HEIGHT;
    rotate_config.in.block_h = CurrentBoard::DISPLAY_NATIVE_WIDTH;
    rotate_config.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    rotate_config.out.buffer = next_frame_buffer;
    rotate_config.out.buffer_size = CurrentBoard::DISPLAY_NATIVE_WIDTH *
                                    CurrentBoard::DISPLAY_NATIVE_HEIGHT * sizeof(lv_color16_t);
    rotate_config.out.pic_w = CurrentBoard::DISPLAY_NATIVE_WIDTH;
    rotate_config.out.pic_h = CurrentBoard::DISPLAY_NATIVE_HEIGHT;
    rotate_config.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    rotate_config.rotation_angle = PPA_SRM_ROTATION_ANGLE_270;
    rotate_config.scale_x = 1.0f;
    rotate_config.scale_y = 1.0f;
    rotate_config.mode = PPA_TRANS_MODE_BLOCKING;
    const esp_err_t rotate_result = ppa_do_scale_rotate_mirror(s_ppa_srm_client, &rotate_config);
    if (rotate_result != ESP_OK) {
        ESP_LOGE("FeatureLVGL", "PPA rotate failed: %s", esp_err_to_name(rotate_result));
    }

    if (s_dpi_vsync != nullptr) {
        while (xSemaphoreTake(s_dpi_vsync, 0) == pdTRUE) {
        }
    }

    const esp_err_t draw_result = esp_lcd_panel_draw_bitmap(
        s_panel, 0, 0, CurrentBoard::DISPLAY_NATIVE_WIDTH,
        CurrentBoard::DISPLAY_NATIVE_HEIGHT, next_frame_buffer);
    if (draw_result != ESP_OK) {
        ESP_LOGE("FeatureLVGL", "Display frame switch failed: %s",
                 esp_err_to_name(draw_result));
    } else if (s_dpi_vsync != nullptr &&
               xSemaphoreTake(s_dpi_vsync, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW("FeatureLVGL", "Timed out waiting for display VSYNC");
    }

    s_next_frame_buffer = static_cast<uint8_t>((s_next_frame_buffer + 1) % FRAME_BUFFER_COUNT);
#if APP_BOOT_SPLASH
    if (lv_display_flush_is_last(display) &&
        s_splash_frame_pending.exchange(false, std::memory_order_acq_rel)) {
        s_splash_visible_since.store(xTaskGetTickCount(), std::memory_order_release);
    }
#endif
    lv_display_flush_ready(display);
}

#endif

#endif // CONFIG_APP_ENABLE_MIPI_DSI

#if APP_BOOT_SPLASH
// Boot splash screen: /www/static/img/splash.jpg, scaled-to-fit ("Stretch to Uniform" -
// aspect ratio preserved, never distorted or cropped) while remaining features initialize.
// The file ships in the www image (data/static/img/splash.jpg, `idf.py www-flash`); this code
// only reads it. app_startup.hpp mounts /www before LVGL for that reason. The P4's hardware
// JPEG engine decodes it once into an RGB565 PSRAM buffer that the LVGL image object points at;
// the buffer is freed again when the splash fades out (finish_startup_screen()).
constexpr const char* kSplashPath = "/www/static/img/splash.jpg";
constexpr uint32_t kSplashMinimumVisibleMs = 3000;
constexpr uint32_t kSplashFadeMs = 250;
constexpr uint32_t kSplashFirstFrameTimeoutMs = 5000;
static lv_image_dsc_t s_splash_dsc = {};
static uint8_t* s_splash_pixels = nullptr;

// Hardware-decodes kSplashPath into s_splash_dsc (RGB565). False on any error - the caller
// then just skips the splash.
bool decode_splash_jpeg() {
    constexpr const char* TAG = "FeatureLVGL";
    FILE* file = fopen(kSplashPath, "rb");
    if (file == nullptr) {
        ESP_LOGW(TAG, "Splash: %s not found (errno=%d) - skipping splash screen", kSplashPath, errno);
        return false;
    }
    fseek(file, 0, SEEK_END);
    const long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    const jpeg_decode_memory_alloc_cfg_t in_cfg = {.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER};
    size_t in_capacity = 0;
    auto* jpeg = file_size > 0 ? static_cast<uint8_t*>(jpeg_alloc_decoder_mem(file_size, &in_cfg, &in_capacity))
                               : nullptr;
    const bool read_ok = jpeg != nullptr && fread(jpeg, 1, file_size, file) == static_cast<size_t>(file_size);
    fclose(file);

    jpeg_decode_picture_info_t info = {};
    jpeg_decoder_handle_t engine = nullptr;
    uint8_t* pixels = nullptr;
    uint32_t stride = 0;
    bool ok = read_ok && jpeg_decoder_get_info(jpeg, file_size, &info) == ESP_OK;
    if (ok) {
        // 4:2:0/4:2:2 output is padded to whole 16x16 MCUs - size the buffer for that.
        const uint32_t padded_w = (info.width + 15) & ~15u;
        const uint32_t padded_h = (info.height + 15) & ~15u;
        stride = padded_w * sizeof(uint16_t);
        const jpeg_decode_memory_alloc_cfg_t out_cfg = {.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
        size_t out_capacity = 0;
        pixels = static_cast<uint8_t*>(jpeg_alloc_decoder_mem(stride * padded_h, &out_cfg, &out_capacity));
        const jpeg_decode_engine_cfg_t engine_cfg = {.intr_priority = 0, .timeout_ms = 1000, .flags = {}};
        ok = pixels != nullptr && jpeg_new_decoder_engine(&engine_cfg, &engine) == ESP_OK;
        if (ok) {
            const jpeg_decode_cfg_t decode_cfg = {
                .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
                .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR, // little-endian RGB565 = LVGL's layout
                .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
            };
            uint32_t out_size = 0;
            const int64_t start_us = esp_timer_get_time();
            ok = jpeg_decoder_process(engine, &decode_cfg, jpeg, file_size, pixels, out_capacity, &out_size) == ESP_OK;
            ESP_LOGI(TAG, "Splash: HW-decoded %s (%ld bytes, %lux%lu) in %lld us", kSplashPath, file_size,
                     static_cast<unsigned long>(info.width), static_cast<unsigned long>(info.height),
                     static_cast<long long>(esp_timer_get_time() - start_us));
            jpeg_del_decoder_engine(engine);
        }
    }
    free(jpeg);
    if (!ok) {
        ESP_LOGW(TAG, "Splash: JPEG decode of %s failed - skipping splash screen", kSplashPath);
        free(pixels);
        return false;
    }

    s_splash_pixels = pixels;
    s_splash_dsc = {};
    s_splash_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_splash_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_splash_dsc.header.w = info.width;
    s_splash_dsc.header.h = info.height;
    s_splash_dsc.header.stride = stride;
    s_splash_dsc.data_size = stride * info.height;
    s_splash_dsc.data = pixels;
    return true;
}

// Creates the splash while caller holds lvgl_port_lock(). Rendering starts after init()
// releases that lock; remaining App::init() work then continues in parallel with taskLVGL.
void show_splash_screen(lv_display_t* display) {
    constexpr const char* TAG = "FeatureLVGL";
    if (!decode_splash_jpeg()) return;
    const int32_t screen_w = lv_display_get_horizontal_resolution(display);
    const int32_t screen_h = lv_display_get_vertical_resolution(display);
    ESP_LOGI(TAG, "Splash: showing '%s', screen %ldx%ld", kSplashPath, static_cast<long>(screen_w),
             static_cast<long>(screen_h));

    s_splash_background = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_splash_background, screen_w, screen_h);
    lv_obj_set_style_bg_color(s_splash_background, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_splash_background, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_splash_background, 0, 0);
    lv_obj_set_style_radius(s_splash_background, 0, 0);
    lv_obj_set_style_pad_all(s_splash_background, 0, 0);
    lv_obj_clear_flag(s_splash_background, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_splash_background, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(s_splash_background);

    s_splash_image = lv_image_create(s_splash_background);
    ESP_LOGI(TAG, "Splash: image object %p created on bg %p", static_cast<void*>(s_splash_image),
             static_cast<void*>(s_splash_background));
    lv_image_set_src(s_splash_image, &s_splash_dsc);

    // Stretched to fill the whole screen, aspect ratio NOT preserved (wanted: no black bars).
    // An explicit screen-sized object (not LV_SIZE_CONTENT) also keeps the first render, which
    // happens before any layout pass, from seeing a zero-sized image.
    lv_obj_set_size(s_splash_image, screen_w, screen_h);
    lv_image_set_inner_align(s_splash_image, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_center(s_splash_image);
    lv_obj_update_layout(s_splash_image);
    ESP_LOGI(TAG, "Splash: obj size after src+scale = %ldx%ld, pos (%ld,%ld)",
             static_cast<long>(lv_obj_get_width(s_splash_image)),
             static_cast<long>(lv_obj_get_height(s_splash_image)),
             static_cast<long>(lv_obj_get_x(s_splash_image)),
             static_cast<long>(lv_obj_get_y(s_splash_image)));

    s_splash_visible_since.store(0, std::memory_order_release);
    s_splash_frame_pending.store(true, std::memory_order_release);
    lv_obj_invalidate(s_splash_background);
}
#endif // APP_BOOT_SPLASH

[[maybe_unused]] void create_ui() {
    lv_obj_t* screen = lv_screen_active();
    lv_display_t* display = lv_obj_get_display(screen);
    const int screen_width = lv_display_get_horizontal_resolution(display);
    const int screen_height = lv_display_get_vertical_resolution(display);
    const int margin = 24;
    const int gap = 14;
    const int card_width = (screen_width - margin * 2 - gap * 3) / 4;

    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0D131C), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t* title = lv_label_create(screen);
    lv_label_set_text(title, "ESPRESSYNTH");
    lv_obj_set_pos(title, margin, 18);
    lv_obj_set_style_text_color(title, lv_color_hex(0xF3F7FC), 0);

    lv_obj_t* board = lv_label_create(screen);
    lv_label_set_text(board, CurrentBoard::BOARD_NAME);
    lv_obj_set_pos(board, margin, 43);
    lv_obj_set_style_text_color(board, lv_color_hex(0x8EA5BE), 0);

    s_ip_label = lv_label_create(screen);
    lv_obj_align(s_ip_label, LV_ALIGN_TOP_RIGHT, -margin, 24);
    lv_obj_set_style_text_color(s_ip_label, lv_color_hex(0x70D6A3), 0);

    const int metrics_y = 78;
    create_card(screen, margin, metrics_y, card_width, 88, "SPEICHER", &s_memory_label);
    create_card(screen, margin + card_width + gap, metrics_y, card_width, 88, "CPU", &s_cpu_label);
    create_card(screen, margin + (card_width + gap) * 2, metrics_y, card_width, 88, "MIDI", &s_midi_label);
    create_card(screen, margin + (card_width + gap) * 3, metrics_y, card_width, 88, "AUDIO", &s_underrun_label);

    const int panel_y = metrics_y + 112;
    const int panel_height = screen_height - panel_y - margin;
    lv_obj_t* preset_panel = lv_obj_create(screen);
    lv_obj_set_pos(preset_panel, margin, panel_y);
    lv_obj_set_size(preset_panel, screen_width - margin * 2, panel_height);
    lv_obj_set_style_bg_color(preset_panel, lv_color_hex(0x141D29), 0);
    lv_obj_set_style_bg_opa(preset_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(preset_panel, 1, 0);
    lv_obj_set_style_border_color(preset_panel, lv_color_hex(0x33465D), 0);
    lv_obj_set_style_radius(preset_panel, 14, 0);
    lv_obj_clear_flag(preset_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* preset_title = lv_label_create(preset_panel);
    lv_label_set_text(preset_title, "AKTIVES PRESET");
    lv_obj_align(preset_title, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_text_color(preset_title, lv_color_hex(0x8EA5BE), 0);

    lv_obj_t* previous = lv_button_create(preset_panel);
    lv_obj_set_size(previous, 100, 80);
    lv_obj_align(previous, LV_ALIGN_LEFT_MID, 70, 5);
    lv_obj_add_event_cb(previous, on_previous_preset, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* previous_label = lv_label_create(previous);
    lv_label_set_text_static(previous_label, LV_SYMBOL_LEFT);
    lv_obj_center(previous_label);

    s_preset_label = lv_label_create(preset_panel);
    lv_label_set_text_static(s_preset_label, "Current");
    lv_obj_align(s_preset_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(s_preset_label, lv_color_hex(0xF3F7FC), 0);

    s_preset_index_label = lv_label_create(preset_panel);
    lv_label_set_text_static(s_preset_index_label, "1 / 1");
    lv_obj_align(s_preset_index_label, LV_ALIGN_CENTER, 0, 38);
    lv_obj_set_style_text_color(s_preset_index_label, lv_color_hex(0x8EA5BE), 0);

    lv_obj_t* next = lv_button_create(preset_panel);
    lv_obj_set_size(next, 100, 80);
    lv_obj_align(next, LV_ALIGN_RIGHT_MID, -70, 5);
    lv_obj_add_event_cb(next, on_next_preset, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* next_label = lv_label_create(next);
    lv_label_set_text_static(next_label, LV_SYMBOL_RIGHT);
    lv_obj_center(next_label);

    lv_obj_t* panic = lv_button_create(screen);
    lv_obj_set_size(panic, 150, 58);
    lv_obj_align(panic, LV_ALIGN_BOTTOM_RIGHT, -margin, -margin);
    lv_obj_set_style_bg_color(panic, lv_color_hex(0xB9364A), 0);
    lv_obj_add_event_cb(panic, on_panic, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* panic_label = lv_label_create(panic);
    lv_label_set_text_static(panic_label, "PANIC");
    lv_obj_center(panic_label);

    s_activity_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_activity_panel, 390, 170);
    lv_obj_align(s_activity_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_activity_panel, lv_color_hex(0x243A53), 0);
    lv_obj_set_style_bg_opa(s_activity_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_activity_panel, 2, 0);
    lv_obj_set_style_border_color(s_activity_panel, lv_color_hex(0x70D6A3), 0);
    lv_obj_set_style_radius(s_activity_panel, 14, 0);
    s_activity_label = lv_label_create(s_activity_panel);
    lv_obj_set_width(s_activity_label, 360);
    lv_label_set_long_mode(s_activity_label, LV_LABEL_LONG_WRAP);
    lv_obj_center(s_activity_label);
    lv_obj_set_style_text_align(s_activity_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_activity_label, lv_color_hex(0xF3F7FC), 0);
    lv_obj_add_flag(s_activity_panel, LV_OBJ_FLAG_HIDDEN);

#if defined(CONFIG_APP_ENABLE_SYNTH)
    refresh_preset_list();
    update_preset_labels();
#endif
    update_runtime_labels();
    s_ui_timer = lv_timer_create(ui_update_timer, 250, nullptr);
}

} // namespace

/**
 * @brief Finish boot splash after all synchronous features have initialized.
 *
 * Waits only for any part of the three-second minimum that has not elapsed since the first
 * displayed splash frame, then fades into the complete LVGL UI.
 */
#if defined(CONFIG_APP_DISPLAY_OUTPUT_RGB)
/**
 * @brief Resync the RGB scan-out in the next vertical blank (see crow_lcd_rgb_panel_restart()).
 *        Bringing up the ESP-Hosted SDIO link can knock the scan-out out of sync without any
 *        redraw following that would trigger the per-flush resync.
 */
inline void resync_display() {
    if (s_panel != nullptr) crow_lcd_rgb_panel_restart(s_panel);
}
#endif

inline void finish_startup_screen() {
#if APP_BOOT_SPLASH
    constexpr const char* TAG = "FeatureLVGL";
    if (s_splash_background == nullptr) {
        lvgl_port_lock(0);
        Feature::EEZUI::start();
        lvgl_port_unlock();
        return;
    }

    const TickType_t frame_wait_started = xTaskGetTickCount();
    const TickType_t frame_timeout = pdMS_TO_TICKS(kSplashFirstFrameTimeoutMs);
    while (s_splash_frame_pending.load(std::memory_order_acquire) &&
           (xTaskGetTickCount() - frame_wait_started) < frame_timeout) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    const TickType_t visible_since = s_splash_visible_since.load(std::memory_order_acquire);
    if (s_splash_frame_pending.load(std::memory_order_acquire) || visible_since == 0) {
        ESP_LOGW(TAG, "Splash: first frame did not flush within %lu ms",
                 static_cast<unsigned long>(kSplashFirstFrameTimeoutMs));
    } else {
        const TickType_t minimum_ticks = pdMS_TO_TICKS(kSplashMinimumVisibleMs);
        const TickType_t visible_ticks = xTaskGetTickCount() - visible_since;
        const TickType_t remaining_ticks =
            visible_ticks < minimum_ticks ? minimum_ticks - visible_ticks : 0;
        ESP_LOGI(TAG, "Splash: startup ready after %lu ms visible; holding %lu ms more",
                 static_cast<unsigned long>(visible_ticks * portTICK_PERIOD_MS),
                 static_cast<unsigned long>(remaining_ticks * portTICK_PERIOD_MS));
        if (remaining_ticks > 0) {
            vTaskDelay(remaining_ticks);
        }
    }

    lvgl_port_lock(0);
    Feature::EEZUI::start();
    // Paint populated labels behind the still-opaque splash before its first
    // transparent frame can reveal the generated EEZ placeholder texts.
    lv_refr_now(nullptr);
    lv_obj_fade_out(s_splash_background, kSplashFadeMs, 0);
    lvgl_port_unlock();

    vTaskDelay(pdMS_TO_TICKS(kSplashFadeMs + 20));

    lvgl_port_lock(0);
    lv_image_cache_drop(lv_image_get_src(s_splash_image));
    lv_obj_delete(s_splash_background); // also deletes the child image object
    s_splash_background = nullptr;
    s_splash_image = nullptr;
    free(s_splash_pixels); // ~1.2 MB decoded RGB565, see decode_splash_jpeg()
    s_splash_pixels = nullptr;
    s_splash_dsc = {};
    lvgl_port_unlock();

    ESP_LOGI(TAG, "Splash: fade complete; LVGL status UI ready");
#else
    lvgl_port_lock(0);
    Feature::EEZUI::start();
    lvgl_port_unlock();
#endif
}

/**
 * @brief Initialize the selected MIPI/SPI panel and LVGL screen.
 */
inline esp_err_t init() {
    constexpr const char* TAG = "FeatureLVGL";

#if defined(CONFIG_APP_DISPLAY_OUTPUT_SPI)
    ESP_RETURN_ON_ERROR(Feature::SPIDisplay::init(), TAG, "SPI display initialization failed");
    esp_lcd_panel_handle_t panel = Feature::SPIDisplay::panel;
#elif defined(CONFIG_APP_DISPLAY_OUTPUT_RGB)
    ESP_LOGI(TAG, "Initializing RGB display for %s (%dx%d)", CurrentBoard::BOARD_NAME,
             CurrentBoard::DISPLAY_NATIVE_WIDTH, CurrentBoard::DISPLAY_NATIVE_HEIGHT);

    esp_lcd_rgb_panel_config_t rgb_panel_config = {};
    rgb_panel_config.clk_src = LCD_CLK_SRC_DEFAULT;
    rgb_panel_config.timings.pclk_hz = CurrentBoard::DISPLAY_PIXEL_CLOCK_HZ;
    rgb_panel_config.timings.h_res = CurrentBoard::DISPLAY_NATIVE_WIDTH;
    rgb_panel_config.timings.v_res = CurrentBoard::DISPLAY_NATIVE_HEIGHT;
    rgb_panel_config.timings.hsync_pulse_width = CurrentBoard::DISPLAY_HSYNC_PULSE_WIDTH;
    rgb_panel_config.timings.hsync_back_porch = CurrentBoard::DISPLAY_HSYNC_BACK_PORCH;
    rgb_panel_config.timings.hsync_front_porch = CurrentBoard::DISPLAY_HSYNC_FRONT_PORCH;
    rgb_panel_config.timings.vsync_pulse_width = CurrentBoard::DISPLAY_VSYNC_PULSE_WIDTH;
    rgb_panel_config.timings.vsync_back_porch = CurrentBoard::DISPLAY_VSYNC_BACK_PORCH;
    rgb_panel_config.timings.vsync_front_porch = CurrentBoard::DISPLAY_VSYNC_FRONT_PORCH;
    rgb_panel_config.timings.flags.pclk_active_neg = true;
    rgb_panel_config.timings.flags.pclk_idle_high = true;
    rgb_panel_config.data_width = 16;
    rgb_panel_config.in_color_format = LCD_COLOR_FMT_RGB565;
    rgb_panel_config.out_color_format = LCD_COLOR_FMT_RGB565;
    rgb_panel_config.num_fbs = 1;
    // No bounce buffers: on the P4 the refill ISR (a CPU memcpy of the whole frame out of PSRAM,
    // ~36 MB/s) eats an entire core. The LCD DMA streams straight from PSRAM instead, with top AXI
    // priority (see components/crow_rgb_lcd) so redraw traffic can't starve it.
    rgb_panel_config.bounce_buffer_size_px = 0;
    rgb_panel_config.dma_burst_size = 64;
    rgb_panel_config.hsync_gpio_num = CurrentBoard::PIN_RGB_HSYNC;
    rgb_panel_config.vsync_gpio_num = CurrentBoard::PIN_RGB_VSYNC;
    rgb_panel_config.de_gpio_num = CurrentBoard::PIN_RGB_DE;
    rgb_panel_config.pclk_gpio_num = CurrentBoard::PIN_RGB_PCLK;
    rgb_panel_config.disp_gpio_num = GPIO_NUM_NC;
    for (int i = 0; i < 16; ++i) {
        rgb_panel_config.data_gpio_nums[i] = CurrentBoard::PIN_RGB_DATA[i];
    }
    rgb_panel_config.flags.fb_in_psram = true;

    esp_lcd_panel_handle_t panel = nullptr;
    ESP_RETURN_ON_ERROR(crow_lcd_new_rgb_panel(&rgb_panel_config, &panel), TAG,
                        "RGB panel creation failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "RGB panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "RGB panel initialization failed");
    s_panel = panel;
    // Backlight sits behind the STC8 co-MCU: enable only once the panel is scanning.
    ESP_RETURN_ON_ERROR(Board::set_backlight_percent(100), TAG, "Backlight enable failed");
#else
    ESP_LOGI(TAG, "Initializing MIPI-DSI display for %s (%dx%d native)",
             CurrentBoard::BOARD_NAME,
             CurrentBoard::DISPLAY_NATIVE_WIDTH, CurrentBoard::DISPLAY_NATIVE_HEIGHT);

    if (CurrentBoard::PIN_BACKLIGHT != GPIO_NUM_NC) {
        gpio_config_t backlight_config = {};
        // Non-const copy: with PIN_BACKLIGHT == -1 (Waveshare default) a constant shift
        // trips -Werror=shift-count-negative although this branch never runs.
        gpio_num_t backlight_pin = CurrentBoard::PIN_BACKLIGHT;
        backlight_config.pin_bit_mask = 1ULL << backlight_pin;
        backlight_config.mode = GPIO_MODE_OUTPUT;
        ESP_RETURN_ON_ERROR(gpio_config(&backlight_config), TAG, "Backlight GPIO setup failed");
        // The backlight is active high on both Guition panels. The known-good drivers
        // enable it before starting the DSI link.
        ESP_RETURN_ON_ERROR(gpio_set_level(CurrentBoard::PIN_BACKLIGHT, 1), TAG,
                            "Backlight enable setup failed");
    }

    esp_ldo_channel_handle_t dsi_phy_ldo = nullptr;
    esp_ldo_channel_config_t ldo_config = {};
    ldo_config.chan_id = CurrentBoard::DISPLAY_DSI_LDO_CHANNEL;
    ldo_config.voltage_mv = CurrentBoard::DISPLAY_DSI_LDO_VOLTAGE_MV;
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_config, &dsi_phy_ldo), TAG,
                        "MIPI-DSI PHY LDO setup failed");

    esp_lcd_dsi_bus_handle_t dsi_bus = nullptr;
    esp_lcd_dsi_bus_config_t bus_config = {};
    bus_config.bus_id = 0;
    bus_config.num_data_lanes = CurrentBoard::DISPLAY_DSI_LANES;
    // Both Guition panels run on an ESP32-P4 revision where the generic PLLREF default
    // maps to the newer XTAL source. Use the legacy-compatible P4 source.
    bus_config.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
#if defined(CONFIG_BOARD_JC1060P470C_7_INCH)
    bus_config.lane_bit_rate_mbps = CurrentBoard::DISPLAY_DSI_LANE_BITRATE_MBPS;
#else
    bus_config.lane_bit_rate_mbps = 1500;
#endif
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &dsi_bus), TAG,
                        "MIPI-DSI bus setup failed");

    esp_lcd_panel_io_handle_t dbi_io = nullptr;
    esp_lcd_dbi_io_config_t dbi_config = {};
    dbi_config.virtual_channel = 0;
    dbi_config.lcd_cmd_bits = 8;
    dbi_config.lcd_param_bits = 8;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_config, &dbi_io), TAG,
                        "MIPI-DSI command IO setup failed");

    esp_lcd_dpi_panel_config_t dpi_config = {};
    dpi_config.virtual_channel = 0;
    dpi_config.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi_config.in_color_format = LCD_COLOR_FMT_RGB565;
    dpi_config.out_color_format = LCD_COLOR_FMT_RGB565;
    dpi_config.num_fbs = FRAME_BUFFER_COUNT;
    dpi_config.video_timing.h_size = CurrentBoard::DISPLAY_NATIVE_WIDTH;
    dpi_config.video_timing.v_size = CurrentBoard::DISPLAY_NATIVE_HEIGHT;
#if defined(CONFIG_BOARD_JC1060P470C_7_INCH)
    dpi_config.dpi_clock_freq_mhz = CurrentBoard::DISPLAY_DPI_CLOCK_MHZ;
    dpi_config.video_timing.hsync_back_porch = CurrentBoard::DISPLAY_HSYNC_BACK_PORCH;
    dpi_config.video_timing.hsync_pulse_width = CurrentBoard::DISPLAY_HSYNC_PULSE_WIDTH;
    dpi_config.video_timing.hsync_front_porch = CurrentBoard::DISPLAY_HSYNC_FRONT_PORCH;
    dpi_config.video_timing.vsync_back_porch = CurrentBoard::DISPLAY_VSYNC_BACK_PORCH;
    dpi_config.video_timing.vsync_pulse_width = CurrentBoard::DISPLAY_VSYNC_PULSE_WIDTH;
    dpi_config.video_timing.vsync_front_porch = CurrentBoard::DISPLAY_VSYNC_FRONT_PORCH;
#else
    dpi_config.dpi_clock_freq_mhz = 60;
    dpi_config.video_timing.hsync_back_porch = 20;
    dpi_config.video_timing.hsync_pulse_width = 20;
    dpi_config.video_timing.hsync_front_porch = 40;
    dpi_config.video_timing.vsync_back_porch = 8;
    dpi_config.video_timing.vsync_pulse_width = 4;
    dpi_config.video_timing.vsync_front_porch = 20;

#endif
#if defined(CONFIG_APP_DISPLAY_MIPI_JD9165)
    jd9165_vendor_config_t vendor_config = {};
    vendor_config.init_cmds = jd9165_jc1060p470c_init_cmds;
    vendor_config.init_cmds_size = jd9165_jc1060p470c_init_cmds_size;
#else
    jd9365_vendor_config_t vendor_config = {};
    vendor_config.init_cmds = jd9365_init_cmds;
    vendor_config.init_cmds_size = jd9365_init_cmds_size;
#endif
    vendor_config.mipi_config.dsi_bus = dsi_bus;
    vendor_config.mipi_config.dpi_config = &dpi_config;
    vendor_config.mipi_config.lane_num = static_cast<uint8_t>(CurrentBoard::DISPLAY_DSI_LANES);

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.reset_gpio_num = CurrentBoard::PIN_LCD_RST;
    panel_config.vendor_config = &vendor_config;

    esp_lcd_panel_handle_t panel = nullptr;
#if defined(CONFIG_APP_DISPLAY_MIPI_JD9165)
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9165(dbi_io, &panel_config, &panel), TAG,
                        "JD9165 panel creation failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "JD9165 panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "JD9165 panel initialization failed");
    // The JD9165 init table already ends with sleep-out + display-on.
#else
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9365(dbi_io, &panel_config, &panel), TAG,
                        "JD9365 panel creation failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "JD9365 panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "JD9365 panel initialization failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG,
                        "JD9365 display enable failed");
#endif

    s_panel = panel;
#if defined(CONFIG_APP_ENABLE_MIPI_DSI) && !defined(CONFIG_BOARD_JC1060P470C_7_INCH)
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(
                            panel, FRAME_BUFFER_COUNT, &s_frame_buffers[0],
                            &s_frame_buffers[1], &s_frame_buffers[2]),
                        TAG, "Could not get DSI frame buffers");

    // One SRM (scale-rotate-mirror) client, reused for every flush_cb() call for the
    // lifetime of the display -- see flush_cb()'s ppa_do_scale_rotate_mirror() call.
    ppa_client_config_t ppa_client_config = {};
    ppa_client_config.oper_type = PPA_OPERATION_SRM;
    ESP_RETURN_ON_ERROR(ppa_register_client(&ppa_client_config, &s_ppa_srm_client), TAG,
                        "PPA SRM client registration failed");
#endif
#if defined(CONFIG_APP_ENABLE_MIPI_DSI) && !defined(CONFIG_BOARD_JC1060P470C_7_INCH)
    s_dpi_vsync = xSemaphoreCreateBinary();
    if (s_dpi_vsync == nullptr) {
        ESP_LOGE(TAG, "Could not create DSI VSYNC semaphore");
        return ESP_ERR_NO_MEM;
    }
    const esp_lcd_dpi_panel_event_callbacks_t dpi_callbacks = {
        .on_color_trans_done = nullptr,
        .on_frame_buf_complete = nullptr,
        .on_vsync = on_dpi_vsync,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_register_event_callbacks(
                            panel, &dpi_callbacks, nullptr),
                        TAG, "DSI callback registration failed");
#endif

    if (CurrentBoard::PIN_BACKLIGHT != GPIO_NUM_NC) {
        ESP_RETURN_ON_ERROR(gpio_set_level(CurrentBoard::PIN_BACKLIGHT, 1), TAG,
                            "Backlight enable failed");
    }

#endif // CONFIG_APP_DISPLAY_OUTPUT_SPI

    lvgl_port_cfg_t lvgl_config = ESP_LVGL_PORT_INIT_CONFIG();
    // Library default is task_affinity = -1 (unpinned); this project pins every task it
    // creates (see app_core.hpp), so pin taskLVGL to MAIN_CORE like the rest of the UI/input
    // work instead of leaving it free to migrate.
    lvgl_config.task_affinity = App::MAIN_CORE;
    lvgl_config.task_max_sleep_ms = 33;
    // 8 KiB: a little above the port default (7 KiB) for the EEZ UI callbacks.
    lvgl_config.task_stack = 8 * 1024; // measured peak use ~4.5 KiB (the splash is decoded on the init task, not here)
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_config), TAG, "LVGL port initialization failed");

    lvgl_port_lock(0);
    // See image_cache_buf_malloc()'s doc comment above: redirect LVGL's decoded-image buffers
    // (PNG/JPEG/etc.) to PSRAM instead of the tiny builtin allocator. Must run after
    // lvgl_port_init() (lv_init() installs the default handlers this overwrites) and while
    // holding the port lock (taskLVGL is already running by this point).
    {
        lv_draw_buf_handlers_t* image_handlers = lv_draw_buf_get_image_handlers();
        image_handlers->buf_malloc_cb = image_cache_buf_malloc;
        image_handlers->buf_free_cb = image_cache_buf_free;
    }
#if defined(CONFIG_APP_DISPLAY_OUTPUT_SPI)
    lvgl_port_display_cfg_t display_config = {};
    display_config.io_handle = Feature::SPIDisplay::io;
    display_config.panel_handle = panel;
    display_config.hres = CONFIG_APP_DISPLAY_WIDTH;
    display_config.vres = CONFIG_APP_DISPLAY_HEIGHT;
    display_config.buffer_size = CONFIG_APP_DISPLAY_WIDTH * Feature::SPIDisplay::buffer_lines;
    display_config.double_buffer = true;
    display_config.color_format = LV_COLOR_FORMAT_RGB565;
    display_config.flags.buff_dma = true;
    display_config.flags.swap_bytes = true;
    display_config.rotation.swap_xy = CONFIG_APP_DISPLAY_WIDTH > 240;
    display_config.rotation.mirror_x = CONFIG_APP_DISPLAY_WIDTH > 240;
    lv_display_t* display = lvgl_port_add_disp(&display_config);
#if APP_BOOT_SPLASH
    if (display != nullptr) {
        lv_display_add_event_cb(display, [](lv_event_t* event) {
            auto* screen = static_cast<lv_display_t*>(lv_event_get_target(event));
            if (lv_display_flush_is_last(screen) &&
                s_splash_frame_pending.exchange(false, std::memory_order_acq_rel)) {
                s_splash_visible_since.store(xTaskGetTickCount(), std::memory_order_release);
            }
        }, LV_EVENT_FLUSH_FINISH, nullptr);
    }
#endif
#elif defined(CONFIG_APP_DISPLAY_OUTPUT_RGB)
    lv_display_t* display = create_rgb_display(panel);
#elif defined(CONFIG_BOARD_JC1060P470C_7_INCH)
    // The panel is natively landscape, so LVGL renders in the panel's own orientation.
    lv_display_t* display = create_direct_display(panel);
#else
    // LVGL works in the user-facing landscape orientation. The flush callback
    // rotates each complete frame into the panel's native 800x1280 buffer.
    lv_display_t* display = lv_display_create(CurrentBoard::DISPLAY_NATIVE_HEIGHT,
                                               CurrentBoard::DISPLAY_NATIVE_WIDTH);
#endif
    if (display == nullptr) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "LVGL display creation failed");
        return ESP_ERR_NO_MEM;
    }

#if defined(CONFIG_APP_ENABLE_MIPI_DSI)
#if defined(CONFIG_BOARD_JC1060P470C_7_INCH)
    ESP_LOGI(TAG, "LVGL: direct rendering into two DPI PSRAM buffers, frame-synchronized switching");
#else
    const size_t draw_buffer_size = CurrentBoard::DISPLAY_NATIVE_WIDTH *
                                    CurrentBoard::DISPLAY_NATIVE_HEIGHT *
                                    sizeof(lv_color16_t);
    const lv_display_render_mode_t render_mode = LV_DISPLAY_RENDER_MODE_FULL;
    s_lvgl_frame_buffer = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(64, draw_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_lvgl_frame_buffer == nullptr) {
        s_lvgl_frame_buffer = static_cast<uint8_t*>(
            heap_caps_malloc(draw_buffer_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (s_lvgl_frame_buffer == nullptr) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "LVGL draw buffer allocation failed");
        return ESP_ERR_NO_MEM;
    }

    lv_display_set_buffers(display, s_lvgl_frame_buffer, nullptr, draw_buffer_size, render_mode);
    ESP_LOGI(TAG, "LVGL draw buffer: %u KiB", static_cast<unsigned>(draw_buffer_size / 1024));
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, flush_cb);
#endif

#endif // CONFIG_APP_ENABLE_MIPI_DSI

#if defined(CONFIG_APP_ENABLE_MIPI_DSI) || defined(CONFIG_APP_DISPLAY_OUTPUT_RGB)
    i2c_master_bus_handle_t touch_i2c_bus = Board::get_i2c_bus_handle(0);
    if (touch_i2c_bus == nullptr) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "Touch initialization failed: shared I2C0 bus is unavailable");
        return ESP_ERR_INVALID_STATE;
    }
    esp_lcd_panel_io_handle_t touch_io = nullptr;
    esp_lcd_panel_io_i2c_config_t touch_io_config = {};
#if defined(CONFIG_BOARD_JC1060P470C_7_INCH) || defined(CONFIG_BOARD_CROWPANEL_5INCH)
    touch_io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
    touch_io_config.lcd_cmd_bits = 16; // GT911 uses 16-bit register addresses
#else
    touch_io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_GSL3680_ADDRESS;
    touch_io_config.lcd_cmd_bits = 8;
#endif
    touch_io_config.scl_speed_hz = Board::i2c_bus_frequency_hz(0);
    touch_io_config.control_phase_bytes = 1;
    touch_io_config.flags.disable_control_phase = 1;
    // Never let a missing/unresponsive touch controller block the LVGL task
    // forever while probing the shared I2C bus.
    touch_io_config.transaction_timeout_ms = 250;
    esp_err_t touch_result = esp_lcd_new_panel_io_i2c(
        touch_i2c_bus, &touch_io_config, &touch_io);
    if (touch_result == ESP_OK) {
        esp_lcd_touch_config_t touch_config = {};
        touch_config.rst_gpio_num = CurrentBoard::PIN_TOUCH_RST;
        touch_config.int_gpio_num = CurrentBoard::PIN_TOUCH_INT;
        touch_config.levels.reset = 0;
        touch_config.levels.interrupt = 0;
#if defined(CONFIG_BOARD_JC1060P470C_7_INCH) || defined(CONFIG_BOARD_CROWPANEL_5INCH)
        // GT911 reports in the panel's own landscape coordinates, so no remapping.
        touch_config.x_max = CurrentBoard::DISPLAY_NATIVE_WIDTH;
        touch_config.y_max = CurrentBoard::DISPLAY_NATIVE_HEIGHT;
        touch_result = esp_lcd_touch_new_i2c_gt911(
            touch_io, &touch_config, &s_touch_handle);
#else
        touch_config.x_max = CurrentBoard::DISPLAY_NATIVE_WIDTH;
        touch_config.y_max = CurrentBoard::DISPLAY_NATIVE_HEIGHT;
        touch_config.process_coordinates = calibrate_touch_coordinates;
        touch_result = esp_lcd_touch_new_i2c_gsl3680(
            touch_io, &touch_config, &s_touch_handle);
#endif
    }
    if (touch_result != ESP_OK) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "Touch controller initialization failed: %s",
                 esp_err_to_name(touch_result));
        return touch_result;
    }
    lv_indev_t* touch_indev = lv_indev_create();
    if (touch_indev == nullptr) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "Touch input device creation failed");
        return ESP_ERR_NO_MEM;
    }
    lv_indev_set_type(touch_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch_indev, touch_read_cb);
    lv_indev_set_display(touch_indev, display);
    lv_timer_set_period(lv_indev_get_read_timer(touch_indev), 16);
    ESP_LOGI(TAG, "Touch ready on I2C0 (RST GPIO%d, INT GPIO%d, 16 ms polling)",
             CurrentBoard::PIN_TOUCH_RST, CurrentBoard::PIN_TOUCH_INT);

#endif // CONFIG_APP_ENABLE_MIPI_DSI || CONFIG_APP_DISPLAY_OUTPUT_RGB

#if (defined(CONFIG_APP_ENABLE_USB_HOST) && defined(CONFIG_USB_HOST_CLASS_HID)) || \
    (defined(CONFIG_APP_ENABLE_BLUETOOTH) && defined(CONFIG_BLE_ENABLE_HID_HOST))
    // USB/BLE mouse support: a second pointer indev, driven by relative HID deltas
    // (see feed_mouse_event() below), plus a visible cursor dot on the top layer.
    s_mouse_max_x = lv_display_get_horizontal_resolution(display) - 1;
    s_mouse_max_y = lv_display_get_vertical_resolution(display) - 1;
    s_mouse_x.store(s_mouse_max_x / 2, std::memory_order_relaxed);
    s_mouse_y.store(s_mouse_max_y / 2, std::memory_order_relaxed);

    lv_indev_t* mouse_indev = lv_indev_create();
    if (mouse_indev == nullptr) {
        ESP_LOGW(TAG, "Mouse pointer input device creation failed");
    } else {
        lv_indev_set_type(mouse_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(mouse_indev, mouse_read_cb);
        lv_indev_set_display(mouse_indev, display);

        lv_obj_t* cursor = lv_obj_create(lv_layer_top());
        lv_obj_clear_flag(cursor, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(cursor, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(cursor, 14, 14);
        lv_obj_set_style_radius(cursor, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(cursor, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(cursor, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(cursor, 2, 0);
        lv_obj_set_style_border_color(cursor, lv_color_hex(0x000000), 0);
        lv_indev_set_cursor(mouse_indev, cursor);
        // Hidden until a mouse actually connects, see feed_mouse_event()'s
        // caller in app_events.hpp and set_mouse_cursor_visible() below.
        lv_obj_add_flag(cursor, LV_OBJ_FLAG_HIDDEN);
        s_mouse_cursor = cursor;
        ESP_LOGI(TAG, "Mouse cursor ready (%ldx%ld bounds)",
                 static_cast<long>(s_mouse_max_x), static_cast<long>(s_mouse_max_y));
    }

    // Physical keyboard support: keep keypad input available for future UI controls.
    s_input_group = lv_group_create();
    if (s_input_group == nullptr) {
        ESP_LOGW(TAG, "LVGL input group creation failed, keyboard navigation disabled");
    } else {
        lv_indev_t* keypad_indev = lv_indev_create();
        if (keypad_indev == nullptr) {
            ESP_LOGW(TAG, "Keyboard input device creation failed");
        } else {
            lv_indev_set_type(keypad_indev, LV_INDEV_TYPE_KEYPAD);
            lv_indev_set_read_cb(keypad_indev, keypad_read_cb);
            lv_indev_set_group(keypad_indev, s_input_group);
        }
    }
#endif

    Feature::EEZUI::init();
#if APP_BOOT_SPLASH
    show_splash_screen(display); // no-op (logged) when splash.jpg is missing or undecodable
#endif

    lvgl_port_unlock();

#if APP_BOOT_SPLASH
    ESP_LOGI(TAG, "Display ready%s",
             s_splash_background != nullptr ? ", startup splash active" : "");
#else
    ESP_LOGI(TAG, "Display and LVGL status UI are ready");
#endif
#if defined(CONFIG_APP_ENABLE_MIPI_DSI)
    (void)dsi_phy_ldo; // Keep the PHY rail acquired for the lifetime of the display.
#endif
    return ESP_OK;
}

#if (defined(CONFIG_APP_ENABLE_USB_HOST) && defined(CONFIG_USB_HOST_CLASS_HID)) || \
    (defined(CONFIG_APP_ENABLE_BLUETOOTH) && defined(CONFIG_BLE_ENABLE_HID_HOST))
/**
 * @brief Feed a USB or BLE HID mouse report (relative deltas) into the LVGL pointer indev.
 *        Safe to call from the mouse task/callback; accumulates into screen-space
 *        coordinates clamped to the display bounds established in init().
 */
inline void feed_mouse_event(int8_t dx, int8_t dy, uint8_t buttons) {
    int32_t x = s_mouse_x.load(std::memory_order_relaxed) + dx;
    int32_t y = s_mouse_y.load(std::memory_order_relaxed) + dy;
    if (x < 0) x = 0;
    if (x > s_mouse_max_x) x = s_mouse_max_x;
    if (y < 0) y = 0;
    if (y > s_mouse_max_y) y = s_mouse_max_y;
    s_mouse_x.store(x, std::memory_order_relaxed);
    s_mouse_y.store(y, std::memory_order_relaxed);
    s_mouse_pressed.store((buttons & 0x01) != 0, std::memory_order_relaxed);
}

/**
 * @brief Show or hide the mouse cursor. Wired to the USB/BLE mouse connection
 *        callbacks, so the cursor only appears while a mouse is actually connected.
 *        Safe to call from any task; takes the LVGL port lock itself.
 */
inline void set_mouse_cursor_visible(bool visible) {
    if (s_mouse_cursor == nullptr) return;
    lvgl_port_lock(0);
    if (visible) {
        lv_obj_clear_flag(s_mouse_cursor, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_mouse_cursor, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
}

/**
 * @brief Feed one physical keyboard key transition (already mapped to an LVGL key
 *        code, see app_events.hpp) into the LVGL keypad indev. Safe to call from
 *        the USB/BLE keyboard task/callback.
 */
inline void feed_keyboard_event(uint32_t lv_key, bool pressed) {
    s_keypad_key.store(lv_key, std::memory_order_relaxed);
    s_keypad_pressed.store(pressed, std::memory_order_relaxed);
}

#endif

} // namespace Feature::LVGL
