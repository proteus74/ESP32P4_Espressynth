#pragma once

// Audio volume/mute/tone commands. Registered when either onboard codec is enabled
// (CONFIG_ONBOARD_AUDIO_ES8311 and/or CONFIG_ONBOARD_AUDIO_WM8960). Volume/mute go through
// esp_codec_dev, which only wraps ES8311 (see main/features/audio/es8311_manager.hpp) - the
// WM8960 path (main/features/audio/wm8960_manager.hpp) has its own DB-based volume API, not
// wired up here. "tone" writes raw PCM instead, so it works on either backend.

#include "driver/i2s_std.h" // i2s_chan_handle_t / i2s_channel_write(), used by the WM8960 path
                            // below even when ONBOARD_AUDIO_WM8960 itself is off (see raw_tx)
#include "esp_codec_dev.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if defined(CONFIG_ONBOARD_AUDIO_ES8311)
    #include "features/audio/es8311_manager.hpp"
#endif
#if defined(CONFIG_ONBOARD_AUDIO_WM8960)
    #include "features/audio/wm8960_manager.hpp"
#endif
#if defined(CONFIG_ONBOARD_AUDIO_NS4168)
    #include "features/audio/ns4168_manager.hpp"
#endif
#if defined(CONFIG_APP_ENABLE_USB_DEVICE) && defined(CONFIG_USB_DEV_ENABLE_UAC2)
    #include "features/usb/device/usb_device_audio.hpp"
#endif

#include <cmath>
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

constexpr uint32_t TONE_DEFAULT_I2S_PORT = 0;
constexpr uint32_t TONE_DEFAULT_DURATION_SEC = 2;
constexpr uint32_t TONE_MAX_DURATION_SEC = 60;

inline void print_tone_usage() {
    printf("Usage: tone [<i2s-port>|usb] [<seconds>]\n"
           "       tone                    - I2S port %lu, %lu s\n"
           "       tone <i2s-port> <secs>  - codec on that I2S port, <secs> seconds\n"
           "       tone usb <secs>         - out over USB Audio (UAC2 microphone stream)\n",
           static_cast<unsigned long>(TONE_DEFAULT_I2S_PORT),
           static_cast<unsigned long>(TONE_DEFAULT_DURATION_SEC));
}

// Generates a fading 440 Hz sine tone and hands each block to write_block(int16_t* interleaved
// stereo samples, size_t frame_count). write_block returns true to keep going, false to abort
// (it has already printed why). Shared across the I2S-codec and USB-mic backends below so the
// tone/envelope math isn't duplicated per backend.
template <typename WriteBlockFn>
inline bool generate_tone(uint32_t duration_seconds, WriteBlockFn&& write_block) {
    constexpr uint32_t sample_rate = 44100;
    constexpr uint32_t frequency = 440;
    // 1024 frames (~23ms @ 44.1kHz), not the ~256 (~5.8ms) this started as: that made
    // ~172 write_block() calls/sec, each only as good as the codec's I2S DMA buffer was deep
    // (default 6*240 frames, ~33ms -- see the matching comment in wm8960_manager.hpp/
    // es8311_manager.hpp) at riding out a stall on the CLI/REPL task calling this. Audible
    // clicks/pops during playback traced back to exactly that. Fewer, bigger writes (~43/sec)
    // leave much more slack either side.
    constexpr size_t frames_per_block = 1024;
    const uint32_t fade_frames = sample_rate / 100;
    const uint32_t total_frames = sample_rate * duration_seconds;
    constexpr float amplitude = 0.2f * 32767.0f;
    constexpr float two_pi = 2.0f * 3.14159265358979323846f;
    constexpr float phase_step = two_pi * frequency / sample_rate;
    // static, not a plain local: at this size (4KB) it would eat the entire default 4096-byte
    // esp_console REPL task stack (see ESP_CONSOLE_REPL_CONFIG_DEFAULT()) on top of everything
    // else already on it. Safe to share across the two generate_tone() call sites in
    // cli_cmd_tone() below -- the CLI dispatches one command at a time, and USB/codec playback
    // never run concurrently within a single "tone" invocation either.
    static int16_t samples[frames_per_block * 2];
    float phase = 0.0f;
    uint32_t frames_remaining = total_frames;

    while (frames_remaining > 0) {
        const size_t frame_count = frames_remaining < frames_per_block
                                       ? frames_remaining
                                       : frames_per_block;
        for (size_t frame = 0; frame < frame_count; ++frame) {
            const uint32_t frame_index = total_frames - frames_remaining + frame;
            const uint32_t frames_from_end = total_frames - 1 - frame_index;
            float envelope = 1.0f;
            if (frame_index < fade_frames) {
                envelope = static_cast<float>(frame_index) / fade_frames;
            } else if (frames_from_end < fade_frames) {
                envelope = static_cast<float>(frames_from_end) / fade_frames;
            }
            const int16_t sample = static_cast<int16_t>(std::sin(phase) * amplitude * envelope);
            samples[frame * 2] = sample;
            samples[frame * 2 + 1] = sample;
            phase += phase_step;
            if (phase >= two_pi) {
                phase -= two_pi;
            }
        }

        if (!write_block(samples, frame_count)) {
            return false;
        }
        frames_remaining -= static_cast<uint32_t>(frame_count);
    }
    return true;
}

inline int cli_cmd_tone(int argc, char** argv) {
    uint32_t i2s_port = TONE_DEFAULT_I2S_PORT;
    uint32_t duration_seconds = TONE_DEFAULT_DURATION_SEC;
    bool via_usb = false;

    if (argc == 3) {
        if (std::strcmp(argv[1], "usb") == 0) {
            via_usb = true;
        } else {
            char* end = nullptr;
            const long parsed_port = std::strtol(argv[1], &end, 10);
            if (end == argv[1] || *end != '\0' || parsed_port < 0 || parsed_port > 2) {
                print_tone_usage();
                return 1;
            }
            i2s_port = static_cast<uint32_t>(parsed_port);
        }
        char* end = nullptr;
        const long parsed_secs = std::strtol(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || parsed_secs <= 0 ||
            parsed_secs > static_cast<long>(TONE_MAX_DURATION_SEC)) {
            print_tone_usage();
            return 1;
        }
        duration_seconds = static_cast<uint32_t>(parsed_secs);
    } else if (argc != 1) {
        print_tone_usage();
        return 1;
    }

    if (via_usb) {
#if defined(CONFIG_APP_ENABLE_USB_DEVICE) && defined(CONFIG_USB_DEV_ENABLE_UAC2)
        if (!Feature::USBDevice::Audio::is_microphone_streaming()) {
            printf("USB Audio not available (host hasn't opened the microphone stream).\n");
            return 1;
        }
        constexpr uint32_t sample_rate = 44100;
        const bool completed = generate_tone(duration_seconds, [](int16_t* samples, size_t frame_count) {
            // write_mic() only queues into TinyUSB's ring buffer, it doesn't block/pace
            // itself to real time the way a hardware I2S write does -- without a matching
            // delay here, this loop would blast the whole tone into the buffer in a few
            // milliseconds and TinyUSB would drop almost all of it as fast as it arrives.
            Feature::USBDevice::Audio::write_mic(samples, frame_count);
            const uint32_t block_ms = static_cast<uint32_t>(frame_count) * 1000 / sample_rate;
            TickType_t delay_ticks = pdMS_TO_TICKS(block_ms);
            vTaskDelay(delay_ticks == 0 ? 1 : delay_ticks);
            return true;
        });
        if (!completed) return 1;
        printf("Played 440 Hz tone for %lu s over USB Audio.\n",
               static_cast<unsigned long>(duration_seconds));
        return 0;
#else
        printf("USB Audio not available (USB_DEV_ENABLE_UAC2 not enabled).\n");
        return 1;
#endif
    }

    esp_codec_dev_handle_t es8311_codec = nullptr;
#if defined(CONFIG_ONBOARD_AUDIO_ES8311)
    if (i2s_port == CONFIG_ONBOARD_AUDIO_ES8311_I2S_PORT) {
        es8311_codec = Feature::Audio::ES8311::get_handle();
    }
#endif
    // Raw I2S TX for the codecs without esp_codec_dev (WM8960, NS4168).
    i2s_chan_handle_t raw_tx = nullptr;
#if defined(CONFIG_ONBOARD_AUDIO_WM8960)
    if (es8311_codec == nullptr && i2s_port == CONFIG_ONBOARD_AUDIO_WM8960_I2S_PORT) {
        raw_tx = Feature::Audio::WM8960::get_tx_handle();
    }
#endif
#if defined(CONFIG_ONBOARD_AUDIO_NS4168)
    if (es8311_codec == nullptr && raw_tx == nullptr && i2s_port == CONFIG_ONBOARD_AUDIO_NS4168_I2S_PORT) {
        raw_tx = Feature::Audio::NS4168::get_tx_handle();
    }
#endif
    if (es8311_codec == nullptr && raw_tx == nullptr) {
        printf("No audio codec configured on I2S port %lu.\n", static_cast<unsigned long>(i2s_port));
        return 1;
    }

#if defined(CONFIG_ONBOARD_AUDIO_ES8311)
    if (es8311_codec != nullptr &&
        esp_codec_dev_set_out_mute(es8311_codec, false) != ESP_CODEC_DEV_OK) {
        printf("Failed to unmute audio output.\n");
        return 1;
    }
#endif

    const bool completed = generate_tone(duration_seconds, [&](int16_t* samples, size_t frame_count) {
        const size_t bytes = frame_count * 2 * sizeof(int16_t);
#if defined(CONFIG_ONBOARD_AUDIO_ES8311)
        if (es8311_codec != nullptr) {
            if (esp_codec_dev_write(es8311_codec, samples, static_cast<int>(bytes)) != ESP_CODEC_DEV_OK) {
                printf("Failed to write tone to audio output.\n");
                return false;
            }
            return true;
        }
#endif
#if defined(CONFIG_ONBOARD_AUDIO_WM8960) || defined(CONFIG_ONBOARD_AUDIO_NS4168)
        if (raw_tx != nullptr) {
            size_t bytes_written = 0;
            if (i2s_channel_write(raw_tx, samples, bytes, &bytes_written, 1000) != ESP_OK ||
                bytes_written != bytes) {
                printf("Failed to write tone to audio output.\n");
                return false;
            }
            return true;
        }
#endif
        return false;
    });
    if (!completed) return 1;

    // Flush a block of silence so no leftover buffered sample lingers on the hardware DAC
    // (only meaningful for the real I2S codecs above, not the USB path, which has none).
    int16_t silence[256 * 2] = {};
#if defined(CONFIG_ONBOARD_AUDIO_ES8311)
    if (es8311_codec != nullptr) {
        esp_codec_dev_write(es8311_codec, silence, sizeof(silence));
    }
#endif
#if defined(CONFIG_ONBOARD_AUDIO_WM8960) || defined(CONFIG_ONBOARD_AUDIO_NS4168)
    if (raw_tx != nullptr) {
        size_t bytes_written = 0;
        i2s_channel_write(raw_tx, silence, sizeof(silence), &bytes_written, 1000);
    }
#endif

    printf("Played 440 Hz tone for %lu s on I2S port %lu.\n",
           static_cast<unsigned long>(duration_seconds), static_cast<unsigned long>(i2s_port));
    return 0;
}

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

    const esp_console_cmd_t tone_cmd = {
        .command = "tone",
        .help = "Play a 440 Hz tone. No args: I2S port 0, 2s. Or give an I2S port (0-2) or "
                 "'usb' (UAC2 microphone stream) plus a duration in seconds.",
        .hint = "[<i2s-port>|usb] [<seconds>]",
        .func = &cli_cmd_tone,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    register_command(tone_cmd);
}

} // namespace Feature::CLI::Commands
