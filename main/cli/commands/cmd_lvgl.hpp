#pragma once

// LVGL refresh control, for measuring how much the display rendering disturbs the audio render
// task (compare p50/p99/peak in the WebSocket "cpuLoad" status with and without it), plus two
// helpers for taking pictures of the display without anyone standing in front of it.
//
//   lvgl pause               stop the LVGL timer: no more redraws/flushes (the panel keeps showing the last frame)
//   lvgl resume              restart it
//   lvgl tap <x> <y>         send a click to the widget at that position, like a finger tap
//   lvgl screenshot <file>   write the frame on the panel to a file: raw RGB565 (little endian),
//                            native width x height, e.g. /littlefs/screen.raw (fetch it with the
//                            web file explorer)

#include "esp_console.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Feature::CLI::Commands {

namespace {

inline int cli_cmd_lvgl(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "pause") == 0) {
        const esp_err_t err = lvgl_port_stop();
        printf("LVGL refresh paused: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "resume") == 0) {
        const esp_err_t err = lvgl_port_resume();
        printf("LVGL refresh resumed: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (argc == 4 && std::strcmp(argv[1], "tap") == 0) {
        lv_point_t point = {std::atoi(argv[2]), std::atoi(argv[3])};
        if (!lvgl_port_lock(1000)) {
            printf("LVGL is busy\n");
            return 1;
        }
        // Same order as a real touch: popups on the top layers win over the screen below.
        lv_obj_t* target = nullptr;
        for (lv_obj_t* layer : {lv_layer_sys(), lv_layer_top(), lv_screen_active()}) {
            target = lv_indev_search_obj(layer, &point);
            if (target != nullptr) break;
        }
        if (target != nullptr) lv_obj_send_event(target, LV_EVENT_CLICKED, nullptr);
        lvgl_port_unlock();
        printf(target != nullptr ? "Tapped the widget at %d,%d\n" : "Nothing clickable at %d,%d\n",
               static_cast<int>(point.x), static_cast<int>(point.y));
        return target != nullptr ? 0 : 1;
    }
    if (argc == 3 && std::strcmp(argv[1], "screenshot") == 0) {
        const uint8_t* pixels = Feature::LVGL::frame_buffer();
        if (pixels == nullptr) {
            printf("This display output keeps no frame buffer to read\n");
            return 1;
        }
        // Copied first: writing the file takes seconds, and the status bar would show the load
        // of that write in the lower rows of the picture.
        const size_t bytes = static_cast<size_t>(CurrentBoard::DISPLAY_NATIVE_WIDTH) *
                             CurrentBoard::DISPLAY_NATIVE_HEIGHT * sizeof(uint16_t);
        auto* frame = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (frame == nullptr) {
            printf("No memory for a copy of the frame\n");
            return 1;
        }
        std::memcpy(frame, pixels, bytes);
        FILE* file = std::fopen(argv[2], "wb");
        if (file == nullptr) {
            heap_caps_free(frame);
            printf("Cannot write %s\n", argv[2]);
            return 1;
        }
        const size_t written = std::fwrite(frame, 1, bytes, file);
        std::fclose(file);
        heap_caps_free(frame);
        printf("%dx%d RGB565, %u of %u bytes -> %s\n", CurrentBoard::DISPLAY_NATIVE_WIDTH,
               CurrentBoard::DISPLAY_NATIVE_HEIGHT, static_cast<unsigned>(written),
               static_cast<unsigned>(bytes), argv[2]);
        return written == bytes ? 0 : 1;
    }
    printf("Usage: lvgl pause | resume | tap <x> <y> | screenshot <file>\n");
    return 1;
}

} // namespace

inline void register_lvgl() {
    const esp_console_cmd_t cmd = {
        .command = "lvgl",
        .help = "LVGL display: pause|resume the refresh, tap <x> <y>, screenshot <file>",
        .hint = "pause|resume|tap <x> <y>|screenshot <file>",
        .func = &cli_cmd_lvgl,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    register_command(cmd);
}

} // namespace Feature::CLI::Commands
