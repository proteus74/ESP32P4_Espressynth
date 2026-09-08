#pragma once

// "usb-list": lists connected USB host devices. Only registered when TinyUSB Host is enabled
// (CONFIG_APP_ENABLE_USB_HOST).

#include "esp_console.h"
#include "tusb.h"

#include <cstdio>

namespace Feature::CLI::Commands {

namespace {

inline const char* speed_name(tusb_speed_t speed) {
    switch (speed) {
        case TUSB_SPEED_LOW:  return "low";
        case TUSB_SPEED_FULL: return "full";
        case TUSB_SPEED_HIGH: return "high";
        default:              return "?";
    }
}

inline int cli_cmd_usb_list(int argc, char** argv) {
    (void)argc;
    (void)argv;

    bool any = false;
    for (uint8_t addr = 1; addr <= CFG_TUH_DEVICE_MAX; ++addr) {
        if (!tuh_mounted(addr)) continue;
        any = true;
        uint16_t vid = 0, pid = 0;
        tuh_vid_pid_get(addr, &vid, &pid);
        printf("addr=%u  VID=0x%04x PID=0x%04x  speed=%s\n", addr, vid, pid, speed_name(tuh_speed_get(addr)));
    }
    if (!any) {
        printf("No USB devices connected.\n");
    }
    return 0;
}

} // namespace

inline void register_usb() {
    const esp_console_cmd_t cmd = {
        .command = "usb-list",
        .help = "List connected USB host devices",
        .hint = nullptr,
        .func = &cli_cmd_usb_list,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    register_command(cmd);
}

} // namespace Feature::CLI::Commands
