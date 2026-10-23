#pragma once

// Audio volume/mute commands. Registered when either onboard codec is enabled
// (CONFIG_ONBOARD_AUDIO_ES8311 and/or CONFIG_ONBOARD_AUDIO_WM8960). Volume/mute go through
// esp_codec_dev, which only wraps ES8311 (see main/features/audio/es8311_manager.hpp) - the
// WM8960 path (main/features/audio/wm8960_manager.hpp) has its own DB-based volume API, not
// wired up here.

#include "esp_codec_dev.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if defined(CONFIG_ONBOARD_AUDIO_ES8311)
    #include "features/audio/es8311_manager.hpp"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Feature::CLI::Commands {

namespace {

#if defined(CONFIG_ONBOARD_AUDIO_ES8311)
inline int cli_cmd_audio_volume(int argc, char** argv) {
    esp_codec_dev_handle_t codec = Feature::Audio::ES8311::get_handle();
    if (codec == nullptr) {
        printf("Audio codec not available.\n");
        return 1;
    }

    if (argc == 1) {
        int volume = 0;
        if (esp_codec_dev_get_out_vol(codec, &volume) != ESP_CODEC_DEV_OK) {
            printf("Failed to read volume.\n");
            return 1;
        }
        printf("Volume: %d%%\n", volume);
        return 0;
    }
    if (argc == 2) {
        const int volume = std::atoi(argv[1]);
        if (volume < 0 || volume > 100) {
            printf("Usage: audio-volume [0-100]\n");
            return 1;
        }
        if (esp_codec_dev_set_out_vol(codec, volume) != ESP_CODEC_DEV_OK) {
            printf("Failed to set volume.\n");
            return 1;
        }
        printf("Volume: %d%%\n", volume);
        return 0;
    }
    printf("Usage: audio-volume [0-100]\n");
    return 1;
}

inline int cli_cmd_audio_mute(int argc, char** argv) {
    esp_codec_dev_handle_t codec = Feature::Audio::ES8311::get_handle();
    if (codec == nullptr) {
        printf("Audio codec not available.\n");
        return 1;
    }
    if (argc != 2 || (std::strcmp(argv[1], "on") != 0 && std::strcmp(argv[1], "off") != 0)) {
        printf("Usage: audio-mute <on|off>\n");
        return 1;
    }
    const bool mute = std::strcmp(argv[1], "on") == 0;
    if (esp_codec_dev_set_out_mute(codec, mute) != ESP_CODEC_DEV_OK) {
        printf("Failed to set mute.\n");
        return 1;
    }
    printf("Mute: %s\n", mute ? "on" : "off");
    return 0;
}
#endif // CONFIG_ONBOARD_AUDIO_ES8311

#if defined(CONFIG_BOARD_CROWPANEL_5INCH)
// Bring-up helper for the STC8 co-MCU outputs: "stc8 <gpio 0-3> <0|1>" (0 TP_RST, 1 CSI_RST,
// 2 AUDIO_SD, 3 LCD_BL_POWER) or "stc8 pwm <0-255>" (backlight duty).
inline int cli_cmd_stc8(int argc, char** argv) {
    if (argc != 3) {
        printf("Usage: stc8 <gpio 0-3> <0|1> | stc8 pwm <0-255>\n");
        return 1;
    }
    const bool is_pwm = std::strcmp(argv[1], "pwm") == 0;
    const int value = std::atoi(argv[2]);
    const esp_err_t err = is_pwm
        ? Board::stc8_write_reg(CurrentBoard::STC8_REG_SET_PWM + CurrentBoard::STC8_PWM_LCD_BL,
                                static_cast<uint8_t>(value))
        : Board::stc8_set_gpio(static_cast<uint8_t>(std::atoi(argv[1])), value != 0);
    printf("%s\n", err == ESP_OK ? "OK" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}
#endif

} // namespace

inline void register_audio() {
#if defined(CONFIG_BOARD_CROWPANEL_5INCH)
    const esp_console_cmd_t stc8_cmd = {
        .command = "stc8",
        .help = "Drive an STC8 co-MCU output: <gpio 0-3> <0|1>, or pwm <0-255>",
        .hint = "<gpio|pwm> <value>",
        .func = &cli_cmd_stc8,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    register_command(stc8_cmd);
#endif

#if defined(CONFIG_ONBOARD_AUDIO_ES8311)
    const esp_console_cmd_t volume_cmd = {
        .command = "audio-volume",
        .help = "Show or set output volume (0-100)",
        .hint = "[0-100]",
        .func = &cli_cmd_audio_volume,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    register_command(volume_cmd);

    const esp_console_cmd_t mute_cmd = {
        .command = "audio-mute",
        .help = "Mute or unmute audio output",
        .hint = "<on|off>",
        .func = &cli_cmd_audio_mute,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    register_command(mute_cmd);
#endif
}

} // namespace Feature::CLI::Commands
