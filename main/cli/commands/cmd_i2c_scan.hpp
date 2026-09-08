#pragma once

// Continuous I2C bus scanner: probes every 7-bit address once a second and prints which
// ones ACK, until any key is pressed. Meant for chasing an intermittently-vanishing device
// (see main/features/audio/wm8960_manager.hpp's per-write I2C diagnostics and
// Board::init()'s one-shot startup scan in main/boards/board_init.cpp) -- watch the address
// list while wiggling a wire/connector/HAT seating to catch the exact moment it drops out.

#include "boards/board_init.hpp"
#include "driver/i2c_master.h"
#include "esp_console.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstdlib>
#include <sys/select.h>

namespace Feature::CLI::Commands {

namespace {

// Blocks up to 1 second waiting for any keypress on stdin. Returns true if one arrived
// (caller should stop). Same select()-on-stdin technique as "htop" (cmd_htop.hpp); falls
// back to a plain delay if the active console backend doesn't support select().
inline bool i2c_scan_key_pressed() {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(fileno(stdin), &readfds);
    struct timeval tv = {1, 0};

    const int ready = select(fileno(stdin) + 1, &readfds, nullptr, nullptr, &tv);
    if (ready <= 0) {
        if (ready < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000)); // select() unsupported here -- plain fallback delay
        }
        return false;
    }
    fgetc(stdin); // consume the key so it doesn't land in the next command line
    return true;
}

inline int cli_cmd_i2c_scan(int argc, char** argv) {
    if (argc != 2) {
        printf("Usage: i2c-scan <port>  (0 or 1, see \"I2C Subsystem\" Kconfig)\n");
        return 1;
    }
    char* end = nullptr;
    const long port = std::strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || (port != 0 && port != 1)) {
        printf("Usage: i2c-scan <port>  (0 or 1, see \"I2C Subsystem\" Kconfig)\n");
        return 1;
    }

    i2c_master_bus_handle_t bus_handle = Board::get_i2c_bus_handle(static_cast<int>(port));
    if (bus_handle == nullptr) {
        printf("I2C port %ld is not enabled (see \"I2C Subsystem\" Kconfig).\n", port);
        return 1;
    }

    printf("Scanning I2C%ld every 1s, addresses 0x08-0x77 -- press any key to stop.\n", port);
    fflush(stdout);

    char found[128];
    while (true) {
        size_t found_len = 0;
        int device_count = 0;
        found[0] = '\0';
        for (uint16_t addr = 0x08; addr <= 0x77; ++addr) {
            if (i2c_master_probe(bus_handle, addr, 20) == ESP_OK) {
                found_len += static_cast<size_t>(std::snprintf(found + found_len, sizeof(found) - found_len,
                                                                "%s0x%02X", device_count == 0 ? "" : ", ", addr));
                ++device_count;
            }
        }
        const double uptime_s = static_cast<double>(esp_timer_get_time()) / 1e6;
        if (device_count == 0) {
            printf("[%8.3fs] no devices found\n", uptime_s);
        } else {
            printf("[%8.3fs] %d device(s): %s\n", uptime_s, device_count, found);
        }
        fflush(stdout);

        if (i2c_scan_key_pressed()) break;
    }

    printf("i2c-scan stopped.\n");
    return 0;
}

} // namespace

inline void register_i2c_scan() {
    const esp_console_cmd_t cmd = {
        .command = "i2c-scan",
        .help = "Continuously scan an I2C bus (0 or 1) once a second and print found "
                "addresses, until any key is pressed",
        .hint = "<port>",
        .func = &cli_cmd_i2c_scan,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    register_command(cmd);
}

} // namespace Feature::CLI::Commands
