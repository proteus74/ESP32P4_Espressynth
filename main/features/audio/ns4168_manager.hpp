#pragma once

#include <atomic>

#include "boards/board_config.hpp"
#include "boards/board_init.hpp"
#include "boards/pin_utils.hpp"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

// NS4168 I2S class-D speaker amplifier (onboard on the Elecrow CrowPanel Advanced boards, or any
// NS4168 breakout). No codec registers, no MCLK: the amp just consumes BCLK/LRCK/SDATA. Like the
// WM8960 path (wm8960_manager.hpp) this owns a raw I2S TX channel that SynthEngine writes PCM to
// directly; volume/mute come from the "NS4168-Out" widget, applied in software by the engine.
namespace Feature::Audio::NS4168 {

namespace {
// Fixed project-wide (shared with the USB UAC2 sound card and the other codecs).
constexpr uint32_t SAMPLE_RATE_HZ  = 44100;
constexpr uint8_t  BITS_PER_SAMPLE = 16;

constexpr gpio_num_t PIN_BCLK = static_cast<gpio_num_t>(CONFIG_ONBOARD_AUDIO_NS4168_PIN_BCLK);
constexpr gpio_num_t PIN_LRCK = static_cast<gpio_num_t>(CONFIG_ONBOARD_AUDIO_NS4168_PIN_LRCK);
constexpr gpio_num_t PIN_DOUT = static_cast<gpio_num_t>(CONFIG_ONBOARD_AUDIO_NS4168_PIN_DOUT);
constexpr gpio_num_t PIN_CTRL = static_cast<gpio_num_t>(CONFIG_ONBOARD_AUDIO_NS4168_PIN_CTRL);
constexpr int I2S_PORT = CONFIG_ONBOARD_AUDIO_NS4168_I2S_PORT;

// The pins are free-form Kconfig values, so catch collisions at compile time (same pattern as
// wm8960_manager.hpp). The CrowPanel lists its own onboard NS4168 pins in RESERVED_PINS, so
// only check other boards against the board's reserved list.
#if !defined(CONFIG_BOARD_CROWPANEL_5INCH)
static_assert(!Board::pin_in_list(PIN_BCLK, CurrentBoard::RESERVED_PINS),
              "ONBOARD_AUDIO_NS4168_PIN_BCLK conflicts with a pin already used by the current board");
static_assert(!Board::pin_in_list(PIN_LRCK, CurrentBoard::RESERVED_PINS),
              "ONBOARD_AUDIO_NS4168_PIN_LRCK conflicts with a pin already used by the current board");
static_assert(!Board::pin_in_list(PIN_DOUT, CurrentBoard::RESERVED_PINS),
              "ONBOARD_AUDIO_NS4168_PIN_DOUT conflicts with a pin already used by the current board");
static_assert(!Board::pin_in_list(PIN_CTRL, CurrentBoard::RESERVED_PINS),
              "ONBOARD_AUDIO_NS4168_PIN_CTRL conflicts with a pin already used by the current board");
#endif
static_assert(PIN_BCLK == GPIO_NUM_NC || (PIN_BCLK != PIN_LRCK && PIN_BCLK != PIN_DOUT && PIN_LRCK != PIN_DOUT),
              "ONBOARD_AUDIO_NS4168 BCLK/LRCK/DOUT must be distinct pins");
#if defined(CONFIG_ONBOARD_AUDIO_ES8311)
static_assert(I2S_PORT != CONFIG_ONBOARD_AUDIO_ES8311_I2S_PORT,
              "ONBOARD_AUDIO_NS4168_I2S_PORT must differ from ONBOARD_AUDIO_ES8311_I2S_PORT");
#endif
#if defined(CONFIG_ONBOARD_AUDIO_WM8960)
static_assert(I2S_PORT != CONFIG_ONBOARD_AUDIO_WM8960_I2S_PORT,
              "ONBOARD_AUDIO_NS4168_I2S_PORT must differ from ONBOARD_AUDIO_WM8960_I2S_PORT");
#endif

inline i2s_chan_handle_t s_tx_handle = nullptr;

// DMA-ring underrun tracking, see the identical comment in es8311_manager.hpp.
inline std::atomic<uint32_t> s_underrun_count{0};
inline std::atomic<int64_t> s_last_underrun_us{0};

bool IRAM_ATTR onSendQueueOverflow(i2s_chan_handle_t /*handle*/, i2s_event_data_t * /*event*/, void * /*user_ctx*/) {
    s_underrun_count.fetch_add(1, std::memory_order_relaxed);
    s_last_underrun_us.store(esp_timer_get_time(), std::memory_order_relaxed);
    return false;
}

// Switches the amp out of shutdown once BCLK/LRCK are running (avoids the start-up pop).
inline esp_err_t enable_amp() {
#if defined(CONFIG_BOARD_CROWPANEL_5INCH)
#if defined(CONFIG_CROWPANEL_AUDIO_SD_ACTIVE_HIGH)
    constexpr bool kAmpOnLevel = true;
#else
    constexpr bool kAmpOnLevel = false;
#endif
    return Board::stc8_set_gpio(CurrentBoard::STC8_GPIO_OUT_AUDIO_SD, kAmpOnLevel);
#else
    if (PIN_CTRL == GPIO_NUM_NC) return ESP_OK;
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = 1ULL << PIN_CTRL;
    io_conf.mode = GPIO_MODE_OUTPUT;
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) return err;
    return gpio_set_level(PIN_CTRL, 1);
#endif
}
} // namespace

/** @brief Raw I2S TX channel SynthEngine writes to; nullptr before/after a failed init(). */
inline i2s_chan_handle_t get_tx_handle() {
    return s_tx_handle;
}

inline uint32_t underrunCount() {
    return s_underrun_count.load(std::memory_order_relaxed);
}

inline int64_t lastUnderrunUs() {
    return s_last_underrun_us.load(std::memory_order_relaxed);
}

inline void resetUnderrunCount() {
    s_underrun_count.store(0, std::memory_order_relaxed);
    s_last_underrun_us.store(0, std::memory_order_relaxed);
}

/** @brief Create + enable the I2S TX channel and wake the amp (ONBOARD_AUDIO_NS4168). */
inline esp_err_t init() {
    constexpr const char* TAG = "FeatureAudioNS4168";

    if (PIN_BCLK == GPIO_NUM_NC || PIN_LRCK == GPIO_NUM_NC || PIN_DOUT == GPIO_NUM_NC) {
        ESP_LOGW(TAG, "NS4168 I2S pins not configured (BCLK/LRCK/DOUT left at -1)");
        return ESP_ERR_NOT_SUPPORTED;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear_after_cb = true;
    chan_cfg.dma_desc_num = CONFIG_AUDIO_I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = CONFIG_AUDIO_I2S_DMA_FRAME_NUM;
    i2s_chan_handle_t tx_handle = nullptr;
    esp_err_t err = i2s_new_channel(&chan_cfg, &tx_handle, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel on port %d: %s", I2S_PORT, esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            static_cast<i2s_data_bit_width_t>(BITS_PER_SAMPLE), I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_NC,
            .bclk = PIN_BCLK,
            .ws   = PIN_LRCK,
            .dout = PIN_DOUT,
            .din  = GPIO_NUM_NC,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    // The I2S default clock on P4 < v3 is the 40 MHz XTAL: 44.1 kHz * 256 needs a fractional
    // divider (~3.54), whose period jitter puts inharmonic sidebands on every tone - plainly
    // audible as a metallic ring on pure sines (Hammond). The APLL hits 44.1 kHz * 512 exactly,
    // the same frequency pdm_mic_manager.hpp tunes it to, so both can share it.
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_512;
    if ((err = i2s_channel_init_std_mode(tx_handle, &std_cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure I2S standard mode: %s", esp_err_to_name(err));
        i2s_del_channel(tx_handle);
        return err;
    }

    // Must be registered before i2s_channel_enable() - see es8311_manager.hpp.
    i2s_event_callbacks_t underrun_callbacks = {};
    underrun_callbacks.on_send_q_ovf = onSendQueueOverflow;
    if (i2s_channel_register_event_callback(tx_handle, &underrun_callbacks, nullptr) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register I2S underrun callback");
    }
    if ((err = i2s_channel_enable(tx_handle)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S channel: %s", esp_err_to_name(err));
        i2s_del_channel(tx_handle);
        return err;
    }
    s_tx_handle = tx_handle;

    err = enable_amp();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to enable the NS4168 amp: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "NS4168 initialized on I2S%d (BCLK:%d LRCK:%d DOUT:%d CTRL:%d), %lu Hz / %u-bit stereo",
             I2S_PORT, PIN_BCLK, PIN_LRCK, PIN_DOUT, PIN_CTRL,
             static_cast<unsigned long>(SAMPLE_RATE_HZ), BITS_PER_SAMPLE);
    return ESP_OK;
}

} // namespace Feature::Audio::NS4168
