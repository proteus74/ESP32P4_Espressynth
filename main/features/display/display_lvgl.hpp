#pragma once

#include "app_core.hpp"
#include "boards/board_config.hpp"
#include "boards/board_init.hpp"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#if defined(CONFIG_BOARD_JC1060P470C_7_INCH)
    // JC1060P470C 7": JD9165 panel, GT911 touch, natively landscape (no rotation).
    #include "esp_lcd_jd9165.h"
    #include "esp_lcd_touch_gt911.h"
    #include "features/display/jd9165_panel_init.h"
#else
    // JC8012P4A1 and friends: JD9365 panel, GSL3680 touch, portrait panel driven landscape.
    #include "esp_lcd_jd9365.h"
    #include "esp_lcd_gsl3680.h"
    #include "features/display/jd9365_init.h"
    // The rotating flush path below offloads the per-frame rotation to the ESP32-P4's PPA
    // (Pixel Processing Accelerator) hardware instead of LVGL's software rotator
    // (lv_draw_sw_rotate()), which was pegging taskLVGL near 100% transposing the whole
    // 1280x800 RGB565 frame on the CPU every flush.
    #include "driver/ppa.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include <atomic>

// USB and BLE HID (keyboard/mouse) both feed the same LVGL input devices below. Every
// block gated on this condition compiles in if either transport's HID host is enabled:
// "#if (USB_HOST + USB_HOST_CLASS_HID) || (BLUETOOTH + BLE_ENABLE_HID_HOST)"

namespace Feature::LVGL {

namespace {

#if defined(CONFIG_BOARD_JC1060P470C_7_INCH)
// LVGL renders straight into the panel's orientation here, so the DPI driver's own
// double buffer is all that's needed.
constexpr size_t FRAME_BUFFER_COUNT = 2;
#else
// The rotating flush path cycles through the DSI frame buffers itself.
constexpr size_t FRAME_BUFFER_COUNT = 3;
#endif
static lv_obj_t* s_status_label = nullptr;
static esp_lcd_panel_handle_t s_panel = nullptr;
static SemaphoreHandle_t s_dpi_vsync = nullptr;
#if !defined(CONFIG_BOARD_JC1060P470C_7_INCH)
static void* s_frame_buffers[FRAME_BUFFER_COUNT] = {};
static ppa_client_handle_t s_ppa_srm_client = nullptr;
#endif
static uint8_t* s_lvgl_frame_buffer = nullptr;
#if !defined(CONFIG_BOARD_JC1060P470C_7_INCH)
static uint8_t s_next_frame_buffer = 1;
#endif
static lv_obj_t* s_touch_position_label = nullptr;
static lv_obj_t* s_touch_popup = nullptr;
static lv_timer_t* s_touch_popup_timer = nullptr;
static esp_lcd_touch_handle_t s_touch_handle = nullptr;

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

void on_test_button(lv_event_t* event) {
    (void)event;
    if (s_status_label != nullptr) {
        lv_label_set_text_static(s_status_label, "Button pressed!");
    }
    if (s_touch_popup == nullptr) {
        s_touch_popup = lv_obj_create(lv_layer_top());
        lv_obj_set_size(s_touch_popup, 700, 220);
        lv_obj_center(s_touch_popup);
        lv_obj_set_style_bg_color(s_touch_popup, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(s_touch_popup, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_touch_popup, 5, 0);
        lv_obj_set_style_border_color(s_touch_popup, lv_color_hex(0x000000), 0);
        lv_obj_t* popup_label = lv_label_create(s_touch_popup);
        lv_label_set_text_static(popup_label, "Button gedrueckt!");
        lv_obj_set_style_text_color(popup_label, lv_color_hex(0x000000), 0);
        lv_obj_center(popup_label);
    }
    lv_obj_clear_flag(s_touch_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_touch_popup);
    if (s_touch_popup_timer == nullptr) {
        s_touch_popup_timer = lv_timer_create(
            [](lv_timer_t* timer) {
                (void)timer;
                if (s_touch_popup != nullptr) {
                    lv_obj_add_flag(s_touch_popup, LV_OBJ_FLAG_HIDDEN);
                }
            }, 1500, nullptr);
    } else {
        lv_timer_reset(s_touch_popup_timer);
    }
    ESP_LOGI("FeatureLVGL", "LVGL test button pressed");
}

#if !defined(CONFIG_BOARD_JC1060P470C_7_INCH)
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
        if (s_touch_position_label != nullptr) {
            lv_label_set_text_fmt(s_touch_position_label, "Touch X: %u  Y: %u", x, y);
        }
        static int last_logged_x = -1;
        static int last_logged_y = -1;
        if (last_logged_x != x || last_logged_y != y) {
            // ESP_LOGI("FeatureLVGL", "Touch position: x=%u y=%u", x, y);
            last_logged_x = x;
            last_logged_y = y;
        }
    }
}

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

#if defined(CONFIG_BOARD_JC1060P470C_7_INCH)

void flush_cb(lv_display_t* display, const lv_area_t* area, uint8_t* px_map) {
    // LV_DISPLAY_RENDER_MODE_PARTIAL: px_map holds only the [area] rectangle (a small,
    // internal-RAM-preferred strip buffer - see lv_display_set_buffers() below), not the
    // whole screen. esp_lcd_panel_draw_bitmap() on a DPI panel handle copies just that
    // rectangle into the panel's own continuously-scanned framebuffer at the given offset -
    // this is what actually cuts the PSRAM traffic per flush (previously a full 1024x600x2B
    // frame every redraw, regardless of how much of the screen actually changed).
    // Waiting for the following VSYNC keeps the DPI engine from scanning the rectangle while
    // it's still being written (tearing), same as the old full-frame path.
    if (s_dpi_vsync != nullptr) {
        while (xSemaphoreTake(s_dpi_vsync, 0) == pdTRUE) {
        }
    }

    const esp_err_t draw_result = esp_lcd_panel_draw_bitmap(
        s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
    if (draw_result != ESP_OK) {
        ESP_LOGE("FeatureLVGL", "Display frame switch failed: %s",
                 esp_err_to_name(draw_result));
    } else if (s_dpi_vsync != nullptr &&
               xSemaphoreTake(s_dpi_vsync, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW("FeatureLVGL", "Timed out waiting for display VSYNC");
    }

    lv_display_flush_ready(display);
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
    lv_display_flush_ready(display);
}

#endif

void create_test_ui() {
    lv_obj_t* screen = lv_screen_active();

    lv_obj_t* title = lv_label_create(screen);
    lv_label_set_text_static(title, CurrentBoard::BOARD_NAME);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 100);

#if (defined(CONFIG_APP_ENABLE_USB_HOST) && defined(CONFIG_USB_HOST_CLASS_HID)) || \
    (defined(CONFIG_APP_ENABLE_BLUETOOTH) && defined(CONFIG_BLE_ENABLE_HID_HOST))
    // Demo text field: proves USB/BLE keyboard text entry (see feed_keyboard_event()),
    // including Tab/Shift+Tab to move focus and arrow keys/Backspace to edit.
    lv_obj_t* text_field = lv_textarea_create(screen);
    lv_textarea_set_one_line(text_field, true);
    lv_textarea_set_placeholder_text(text_field, "Type here (USB/BLE keyboard)");
    lv_obj_set_width(text_field, 400);
    lv_obj_align_to(text_field, title, LV_ALIGN_OUT_BOTTOM_MID, 0, 30);
    if (s_input_group != nullptr) {
        lv_group_add_obj(s_input_group, text_field);
        lv_group_focus_obj(text_field);
    }
#endif

    lv_obj_t* button = lv_button_create(screen);
    lv_obj_set_size(button, 320, 120);
    lv_obj_center(button);
    // The default theme only styles PRESSED, not HOVERED, so a mouse resting on
    // the button (without clicking) shows no feedback at all. Add an explicit
    // hover tint, plus a darker one for hover+press (touch presses also pass
    // through HOVERED on the way to PRESSED, see lv_indev.c's pointer handling).
    lv_obj_set_style_bg_color(button, lv_color_hex(0x4DA3FF),
                              static_cast<lv_style_selector_t>(LV_PART_MAIN) | LV_STATE_HOVERED);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x1E5FBF),
                              static_cast<lv_style_selector_t>(LV_PART_MAIN) | LV_STATE_HOVERED | LV_STATE_PRESSED);
    // PRESSED is intentional for this hardware test: it gives immediate
    // feedback even if the controller does not report a clean release yet.
    lv_obj_add_event_cb(button, on_test_button, LV_EVENT_PRESSED, nullptr);
#if (defined(CONFIG_APP_ENABLE_USB_HOST) && defined(CONFIG_USB_HOST_CLASS_HID)) || \
    (defined(CONFIG_APP_ENABLE_BLUETOOTH) && defined(CONFIG_BLE_ENABLE_HID_HOST))
    if (s_input_group != nullptr) {
        lv_group_add_obj(s_input_group, button);
    }
#endif

    lv_obj_t* button_label = lv_label_create(button);
    lv_label_set_text_static(button_label, "Test button");
    lv_obj_center(button_label);

    s_status_label = lv_label_create(screen);
    lv_label_set_text_static(s_status_label, "Display online");
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, -100);

    s_touch_position_label = lv_label_create(screen);
    lv_label_set_text_static(s_touch_position_label, "Touch X: --  Y: --");
    lv_obj_align(s_touch_position_label, LV_ALIGN_TOP_LEFT, 30, 30);
}

} // namespace

/**
 * @brief Initialize the selected board's MIPI-DSI panel and a minimal LVGL screen.
 */
inline esp_err_t init() {
    constexpr const char* TAG = "FeatureLVGL";

    ESP_LOGI(TAG, "Initializing MIPI-DSI display for %s (%dx%d native)",
             CurrentBoard::BOARD_NAME,
             CurrentBoard::DISPLAY_NATIVE_WIDTH, CurrentBoard::DISPLAY_NATIVE_HEIGHT);

    if (CurrentBoard::PIN_BACKLIGHT != GPIO_NUM_NC) {
        gpio_config_t backlight_config = {};
        backlight_config.pin_bit_mask = 1ULL << CurrentBoard::PIN_BACKLIGHT;
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

    jd9165_vendor_config_t vendor_config = {};
    vendor_config.init_cmds = jd9165_jc1060p470c_init_cmds;
    vendor_config.init_cmds_size = jd9165_jc1060p470c_init_cmds_size;
#else
    dpi_config.dpi_clock_freq_mhz = 60;
    dpi_config.video_timing.hsync_back_porch = 20;
    dpi_config.video_timing.hsync_pulse_width = 20;
    dpi_config.video_timing.hsync_front_porch = 40;
    dpi_config.video_timing.vsync_back_porch = 8;
    dpi_config.video_timing.vsync_pulse_width = 4;
    dpi_config.video_timing.vsync_front_porch = 20;

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
#if defined(CONFIG_BOARD_JC1060P470C_7_INCH)
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
#if !defined(CONFIG_BOARD_JC1060P470C_7_INCH)
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

    if (CurrentBoard::PIN_BACKLIGHT != GPIO_NUM_NC) {
        ESP_RETURN_ON_ERROR(gpio_set_level(CurrentBoard::PIN_BACKLIGHT, 1), TAG,
                            "Backlight enable failed");
    }

    lvgl_port_cfg_t lvgl_config = ESP_LVGL_PORT_INIT_CONFIG();
    // Library default is task_affinity = -1 (unpinned); this project pins every task it
    // creates (see app_core.hpp), so pin taskLVGL to MAIN_CORE like the rest of the UI/input
    // work instead of leaving it free to migrate.
    lvgl_config.task_affinity = App::MAIN_CORE;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_config), TAG, "LVGL port initialization failed");

    lvgl_port_lock(0);
#if defined(CONFIG_BOARD_JC1060P470C_7_INCH)
    // The panel is natively landscape, so LVGL renders in the panel's own orientation.
    lv_display_t* display = lv_display_create(CurrentBoard::DISPLAY_NATIVE_WIDTH,
                                               CurrentBoard::DISPLAY_NATIVE_HEIGHT);
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

#if defined(CONFIG_BOARD_JC1060P470C_7_INCH)
    // Partial mode: a small strip buffer (1/10 of screen height) instead of a full
    // 1024x600x2B (~1.2MB) frame buffer. flush_cb() above now draws only the changed
    // rectangle via esp_lcd_panel_draw_bitmap()'s partial-rect support, so most UI updates
    // (a label, the touch popup, button feedback) write a small fraction of what a full
    // repaint did - measured on real hardware to remove a periodic PSRAM-bandwidth-
    // contention burst that was pushing the synth render task's block time over budget at
    // higher voice counts (see project notes). Only this board's flush_cb is partial-rect-
    // aware; other boards keep the full-frame path below untouched.
    const size_t draw_buffer_size = CurrentBoard::DISPLAY_NATIVE_WIDTH *
                                    (CurrentBoard::DISPLAY_NATIVE_HEIGHT / 10) *
                                    sizeof(lv_color16_t);
    const lv_display_render_mode_t render_mode = LV_DISPLAY_RENDER_MODE_PARTIAL;
#else
    const size_t draw_buffer_size = CurrentBoard::DISPLAY_NATIVE_WIDTH *
                                    CurrentBoard::DISPLAY_NATIVE_HEIGHT *
                                    sizeof(lv_color16_t);
    const lv_display_render_mode_t render_mode = LV_DISPLAY_RENDER_MODE_FULL;
#endif
    s_lvgl_frame_buffer = static_cast<uint8_t*>(
        heap_caps_malloc(draw_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
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
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, flush_cb);

    i2c_master_bus_handle_t touch_i2c_bus = Board::get_i2c_bus_handle(0);
    if (touch_i2c_bus == nullptr) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "Touch initialization failed: shared I2C0 bus is unavailable");
        return ESP_ERR_INVALID_STATE;
    }
    esp_lcd_panel_io_handle_t touch_io = nullptr;
    esp_lcd_panel_io_i2c_config_t touch_io_config = {};
#if defined(CONFIG_BOARD_JC1060P470C_7_INCH)
    touch_io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
    touch_io_config.lcd_cmd_bits = 16; // GT911 uses 16-bit register addresses
#else
    touch_io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_GSL3680_ADDRESS;
    touch_io_config.lcd_cmd_bits = 8;
#endif
    touch_io_config.scl_speed_hz = 400000;
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
#if defined(CONFIG_BOARD_JC1060P470C_7_INCH)
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
    ESP_LOGI(TAG, "Touch ready on I2C0 (RST GPIO%d, INT GPIO%d)",
             CurrentBoard::PIN_TOUCH_RST, CurrentBoard::PIN_TOUCH_INT);

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

    // Physical keyboard support: a keypad indev feeding an LVGL group, so Tab/Shift+Tab
    // move focus between the widgets added to the group (see create_test_ui()), arrow
    // keys/Backspace/Delete/Enter go to whichever one is focused, and printable characters
    // type into a focused text field. Key -> LV_KEY_* mapping lives in app_events.hpp.
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

    create_test_ui();
    lvgl_port_unlock();

    ESP_LOGI(TAG, "Display and LVGL test button are ready");
    (void)dsi_phy_ldo; // Keep the PHY rail acquired for the lifetime of the display.
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
