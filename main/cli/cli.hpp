#pragma once

// Interactive CLI (REPL) over the active console (UART/USB-Serial-JTAG/USB-CDC,
// whichever is selected under "Component config -> ESP System Settings -> Channel
// for console output"). Command history (Up/Down) and Tab-completion come from
// the esp_console component itself, no extra setup needed.
//
// Adding a new command:
//   1. Create main/cli/commands/cmd_<name>.hpp with a
//      `Feature::CLI::Commands::register_<name>()` function that calls
//      register_command() (see cmd_mem.hpp / cmd_reset.hpp) -- not
//      esp_console_cmd_register() directly, so it also shows up in "help".
//   2. #include it below and call register_<name>() from init().

#include "app_core.hpp"
#include "esp_console.h"
#include "esp_log.h"
#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    #include "linenoise/linenoise.h"
#endif

#include <cstdio>
#include <cstring>

namespace Feature::CLI {

namespace {
constexpr const char* TAG = "FeatureCLI";

// Every command registered via register_command() below, recorded only so "help" can list
// them in a fixed, compact format (esp_console's own built-in help command doesn't support
// that layout).
struct CommandInfo {
    const char* command;
    const char* hint; // may be nullptr
    const char* help; // may be nullptr
};

constexpr int MAX_COMMANDS = 32;
inline CommandInfo registered_commands[MAX_COMMANDS]{};
inline int registered_command_count = 0;

// Command, parameters, then description -- all on one line, one command per line. Commands
// are left-padded to the longest registered command's length, so parameters/description
// line up in a column across every row.
inline int cli_cmd_help(int argc, char** argv) {
    (void)argc;
    (void)argv;

    size_t max_command_len = 0;
    for (int i = 0; i < registered_command_count; ++i) {
        const size_t len = std::strlen(registered_commands[i].command);
        if (len > max_command_len) max_command_len = len;
    }

    for (int i = 0; i < registered_command_count; ++i) {
        const CommandInfo& info = registered_commands[i];
        printf("%-*s", static_cast<int>(max_command_len + 2), info.command);
        if (info.hint != nullptr && info.hint[0] != '\0') printf("%s  ", info.hint);
        if (info.help != nullptr && info.help[0] != '\0') printf("%s", info.help);
        printf("\n");
    }
    return 0;
}
} // namespace

/**
 * @brief Register a console command (wraps esp_console_cmd_register()) and record it for the
 *        "help" command's listing.
 */
inline void register_command(const esp_console_cmd_t& cmd) {
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
    if (registered_command_count < MAX_COMMANDS) {
        registered_commands[registered_command_count++] = CommandInfo{cmd.command, cmd.hint, cmd.help};
    }
}

} // namespace Feature::CLI

#include "cli/commands/cmd_clear.hpp"
#include "cli/commands/cmd_mem.hpp"
#include "cli/commands/cmd_reset.hpp"
#include "cli/commands/cmd_uptime.hpp"
#include "cli/commands/cmd_version.hpp"
#include "cli/commands/cmd_chipinfo.hpp"
#include "cli/commands/cmd_top.hpp"
#include "cli/commands/cmd_htop.hpp"
#include "cli/commands/cmd_log.hpp"

#if defined(CONFIG_APP_ENABLE_BLUETOOTH)
    #include "cli/commands/cmd_ble.hpp"
#endif

#if defined(CONFIG_APP_ENABLE_COPROCESSOR_OTA)
    #include "cli/commands/cmd_coprocessor.hpp"
#endif

#if defined(CONFIG_APP_ENABLE_LITTLEFS) || defined(CONFIG_APP_ENABLE_WEBSERVER)
    #include "cli/commands/cmd_df.hpp"
#endif

#if defined(CONFIG_APP_ENABLE_LITTLEFS)
    #include "cli/commands/cmd_fs.hpp"
#endif

#if defined(CONFIG_APP_ENABLE_WIFI)
    #include "cli/commands/cmd_wifi.hpp"
#endif

#if defined(CONFIG_APP_ENABLE_ETHERNET)
    #include "cli/commands/cmd_eth.hpp"
#endif

#if defined(CONFIG_APP_ENABLE_WIFI) || defined(CONFIG_APP_ENABLE_ETHERNET)
    #include "cli/commands/cmd_ping.hpp"
#endif

#if defined(CONFIG_ONBOARD_AUDIO_ES8311) || defined(CONFIG_ONBOARD_AUDIO_WM8960) || \
    defined(CONFIG_ONBOARD_AUDIO_NS4168) || \
    (defined(CONFIG_APP_ENABLE_USB_DEVICE) && defined(CONFIG_USB_DEV_ENABLE_UAC2))
    #include "cli/commands/cmd_audio.hpp"
#endif

#if defined(CONFIG_BOARD_CROWPANEL_5INCH)
    #include "cli/commands/cmd_backlight.hpp"
#endif
#if defined(CONFIG_APP_ENABLE_I2C)
    #include "cli/commands/cmd_i2c_scan.hpp"
#endif

#if defined(CONFIG_APP_ENABLE_USB_HOST)
    #include "cli/commands/cmd_usb.hpp"
#endif

namespace Feature::CLI {

inline esp_err_t init() {
    esp_console_repl_t* repl = nullptr;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = ">";
    repl_config.max_history_len = 32;
    repl_config.max_cmdline_length = 256;
    // Default is tskNO_AFFINITY; this project pins every task it creates (see
    // app_core.hpp), so pin console_repl to MAIN_CORE like the rest of the CLI/input work.
    repl_config.task_core_id = App::MAIN_CORE;

    esp_err_t err = esp_console_new_repl_stdio(&repl_config, &repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create console REPL: %s", esp_err_to_name(err));
        return err;
    }

#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    // VS Code's serial monitor does not answer linenoise's ESC[5n probe, but it
    // supports ANSI editing sequences and arrow keys over USB Serial/JTAG.
    linenoiseSetDumbMode(0);
#endif

    const esp_console_cmd_t help_cmd = {
        .command = "help",
        .help = "List all commands",
        .hint = nullptr,
        .func = &cli_cmd_help,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    register_command(help_cmd);

    Commands::register_clear();
    Commands::register_mem();
    Commands::register_reset();
    Commands::register_uptime();
    Commands::register_version();
    Commands::register_chip_info();
    Commands::register_top();
    Commands::register_htop();
    Commands::register_log();
#if defined(CONFIG_APP_ENABLE_BLUETOOTH)
    Commands::register_ble();
#endif
#if defined(CONFIG_APP_ENABLE_COPROCESSOR_OTA)
    Commands::register_coprocessor();
#endif
#if defined(CONFIG_APP_ENABLE_LITTLEFS) || defined(CONFIG_APP_ENABLE_WEBSERVER)
    Commands::register_df();
#endif
#if defined(CONFIG_APP_ENABLE_LITTLEFS)
    Commands::register_fs();
#endif
#if defined(CONFIG_APP_ENABLE_WIFI)
    Commands::register_wifi();
#endif
#if defined(CONFIG_APP_ENABLE_ETHERNET)
    Commands::register_eth();
#endif
#if defined(CONFIG_APP_ENABLE_WIFI) || defined(CONFIG_APP_ENABLE_ETHERNET)
    Commands::register_ping();
#endif
#if defined(CONFIG_ONBOARD_AUDIO_ES8311) || defined(CONFIG_ONBOARD_AUDIO_WM8960) || \
    defined(CONFIG_ONBOARD_AUDIO_NS4168) || \
    (defined(CONFIG_APP_ENABLE_USB_DEVICE) && defined(CONFIG_USB_DEV_ENABLE_UAC2))
    Commands::register_audio();
#endif
#if defined(CONFIG_BOARD_CROWPANEL_5INCH)
    Commands::register_backlight();
#endif
#if defined(CONFIG_APP_ENABLE_I2C)
    Commands::register_i2c_scan();
#endif
#if defined(CONFIG_APP_ENABLE_USB_HOST)
    Commands::register_usb();
#endif

    return esp_console_start_repl(repl);
}

} // namespace Feature::CLI
