#pragma once

#include <atomic>

#include "boards/board_config.hpp"
#include "boards/board_init.hpp"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

// Generic ES8311 codec support. Pins and I2S/I2C port are Kconfig-configurable rather than
// board-fixed (same pattern as wm8960_manager.hpp) - defaults match the onboard wiring of
// boards that actually carry an ES8311 (JC8012P4A1, Waveshare P4 WiFi Dev Kit), but any board
// (e.g. a CONFIG_BOARD_CUSTOM_P4 board, or an external ES8311 breakout) can point this at
// whatever pins it's actually wired to. Unlike WM8960, this codec IS wrapped in esp_codec_dev
// (an audio_codec_if_t implementation ships with the esp_codec_dev component), so init here
// just wires up esp_codec_dev's I2S/I2C interfaces and hands them to it.
namespace Feature::Audio::ES8311 {

namespace {
// Fixed project-wide (shared with the USB UAC2 sound card and the WM8960 codec).
constexpr uint32_t SAMPLE_RATE_HZ  = 44100;
constexpr uint8_t  BITS_PER_SAMPLE = 16;
constexpr uint8_t  CHANNEL_COUNT   = 2;
constexpr int      DEFAULT_VOLUME  = 60;   // 0-100 %
constexpr float    DEFAULT_MIC_GAIN_DB = 30.0f; // ES8311 PGA: 0-42 dB in 6 dB steps

// User-configurable pins (Kconfig). Default to whichever onboard wiring belongs to the
// selected board; -1 (GPIO_NUM_NC) on a board with no onboard ES8311.
constexpr gpio_num_t PIN_MCLK = static_cast<gpio_num_t>(CONFIG_ONBOARD_AUDIO_ES8311_PIN_MCLK);
constexpr gpio_num_t PIN_BCLK = static_cast<gpio_num_t>(CONFIG_ONBOARD_AUDIO_ES8311_PIN_BCLK);
constexpr gpio_num_t PIN_LRCK = static_cast<gpio_num_t>(CONFIG_ONBOARD_AUDIO_ES8311_PIN_LRCK);
constexpr gpio_num_t PIN_DOUT = static_cast<gpio_num_t>(CONFIG_ONBOARD_AUDIO_ES8311_PIN_DOUT);
constexpr gpio_num_t PIN_DIN  = static_cast<gpio_num_t>(CONFIG_ONBOARD_AUDIO_ES8311_PIN_DIN);
constexpr gpio_num_t PIN_PA   = static_cast<gpio_num_t>(CONFIG_ONBOARD_AUDIO_ES8311_PIN_PA);

inline esp_codec_dev_handle_t s_codec_dev = nullptr;

// True DMA-ring-underrun tracking, as opposed to SynthEngine::renderOverrunCount() (a render-
// *budget* miss - the DMA ring's ~AUDIO_I2S_DMA_DESC_NUM*AUDIO_I2S_DMA_FRAME_NUM frames of
// headroom usually absorb that without ever reaching here, see the dma_desc_num/dma_frame_num
// comment below). on_send_q_ovf fires from I2S driver ISR context only once the ring has
// genuinely run out of real data: every queued descriptor already finished sending and the
// render task still hasn't refilled the oldest one, so the driver is about to recycle it
// (auto_clear_after_cb below zeroes it to silence rather than repeating stale audio). Only
// atomic stores here - no logging, nothing ISR-unsafe (esp_timer_get_time() is IRAM-resident
// and safe to call from ISR context, unlike the esp_timer software-timer start/stop calls).
inline std::atomic<uint32_t> s_underrun_count{0};
inline std::atomic<int64_t> s_last_underrun_us{0};

bool IRAM_ATTR onSendQueueOverflow(i2s_chan_handle_t /*handle*/, i2s_event_data_t * /*event*/, void * /*user_ctx*/) {
    s_underrun_count.fetch_add(1, std::memory_order_relaxed);
    s_last_underrun_us.store(esp_timer_get_time(), std::memory_order_relaxed);
    return false;
}
} // namespace

/**
 * @brief Get the initialized ES8311 codec device handle.
 *        Returns nullptr if ONBOARD_AUDIO_ES8311 is disabled or init() failed/was not called.
 */
inline esp_codec_dev_handle_t get_handle() {
    return s_codec_dev;
}

/** @brief True DMA-ring underrun count since boot (see on_send_q_ovf comment above). */
inline uint32_t underrunCount() {
    return s_underrun_count.load(std::memory_order_relaxed);
}

/** @brief esp_timer_get_time() timestamp of the most recent underrun, or 0 if none yet. */
inline int64_t lastUnderrunUs() {
    return s_last_underrun_us.load(std::memory_order_relaxed);
}

/** @brief Zeroes the underrun count/timestamp - see Feature::Synth::dma_underrun_tick(). */
inline void resetUnderrunCount() {
    s_underrun_count.store(0, std::memory_order_relaxed);
    s_last_underrun_us.store(0, std::memory_order_relaxed);
}

/**
 * @brief Initialize I2S bus and ES8311 codec (ONBOARD_AUDIO_ES8311)
 */
inline esp_err_t init() {
    constexpr const char* TAG = "FeatureAudioES8311";
    ESP_LOGI(TAG, "Initializing ES8311 audio codec for %s", CurrentBoard::BOARD_NAME);

    if (PIN_BCLK == GPIO_NUM_NC || PIN_LRCK == GPIO_NUM_NC || PIN_DOUT == GPIO_NUM_NC) {
        ESP_LOGW(TAG, "ES8311 I2S pins not configured (BCLK/LRCK/DOUT left at -1)");
        return ESP_ERR_NOT_SUPPORTED;
    }

    const int i2s_port = CONFIG_ONBOARD_AUDIO_ES8311_I2S_PORT;

    // --- I2S data interface (BCLK/LRCK/DOUT/DIN, optional MCLK) ---
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(i2s_port, I2S_ROLE_MASTER);
    chan_cfg.auto_clear_after_cb = true;
    // Shared with wm8960_manager.hpp via the "Audio Subsystem" Kconfig menu
    // (AUDIO_I2S_DMA_DESC_NUM / AUDIO_I2S_DMA_FRAME_NUM) so both onboard I2S
    // codecs tune the same underrun-headroom-vs-latency tradeoff together: a
    // brief writer stall (e.g. the "tone" CLI command, cmd_audio.hpp, running
    // on the CLI task) audibly clicks once it outlasts the buffer. Default
    // 6*240 frames is ~32.6ms; raise via menuconfig (e.g. 8*480, ~87ms) for
    // more headroom, or lower for less latency.
    chan_cfg.dma_desc_num = CONFIG_AUDIO_I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = CONFIG_AUDIO_I2S_DMA_FRAME_NUM;
    i2s_chan_handle_t tx_handle = nullptr;
    i2s_chan_handle_t rx_handle = nullptr;
    // See the detailed comment in wm8960_manager.hpp: tx+rx are created together deliberately.
    // Splitting into two i2s_new_channel calls doesn't avoid a driver warning, it just trades
    // the "switched from master to slave" one for an "already occupied" one from re-acquiring
    // the same port. Single call is simplest; ESP32-P4 masters BCLK/WS, ES8311 follows on both.
    esp_err_t err = i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel on port %d: %s", i2s_port, esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            static_cast<i2s_data_bit_width_t>(BITS_PER_SAMPLE), I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_MCLK,
            .bclk = PIN_BCLK,
            .ws   = PIN_LRCK,
            .dout = PIN_DOUT,
            .din  = PIN_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    if ((err = i2s_channel_init_std_mode(tx_handle, &std_cfg)) != ESP_OK ||
        (err = i2s_channel_init_std_mode(rx_handle, &std_cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure I2S standard mode: %s", esp_err_to_name(err));
        return err;
    }

    // Must be registered before i2s_channel_enable() below - the driver rejects DMA event
    // callback registration once the channel is running.
    i2s_event_callbacks_t underrun_callbacks = {};
    underrun_callbacks.on_send_q_ovf = onSendQueueOverflow;
    err = i2s_channel_register_event_callback(tx_handle, &underrun_callbacks, nullptr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register I2S underrun callback: %s", esp_err_to_name(err));
    }

    if ((err = i2s_channel_enable(tx_handle)) != ESP_OK || (err = i2s_channel_enable(rx_handle)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S channel: %s", esp_err_to_name(err));
        return err;
    }

    audio_codec_i2s_cfg_t i2s_cfg = {};
    i2s_cfg.port      = i2s_port;
    i2s_cfg.rx_handle = rx_handle;
    i2s_cfg.tx_handle = tx_handle;
    const audio_codec_data_if_t* data_if = audio_codec_new_i2s_data(&i2s_cfg);

    // --- I2C control interface (register access, port selected via
    // ONBOARD_AUDIO_ES8311_I2C_PORT - see "I2C Subsystem" Kconfig) ---
    const int i2c_port = CONFIG_ONBOARD_AUDIO_ES8311_I2C_PORT;
    i2c_master_bus_handle_t i2c_bus = Board::get_i2c_bus_handle(i2c_port);
    if (i2c_bus == nullptr) {
        ESP_LOGE(TAG, "I2C port %d not enabled - check I2C Subsystem Kconfig", i2c_port);
        return ESP_ERR_NOT_SUPPORTED;
    }
    audio_codec_i2c_cfg_t i2c_cfg = {};
    i2c_cfg.port            = i2c_port;
    i2c_cfg.addr            = ES8311_CODEC_DEFAULT_ADDR;
    i2c_cfg.bus_handle      = i2c_bus;
    i2c_cfg.clock_speed_hz  = Board::i2c_bus_frequency_hz(i2c_port);
    const audio_codec_ctrl_if_t* ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);

    const audio_codec_gpio_if_t* gpio_if = audio_codec_new_gpio();

    // --- ES8311 codec ---
    es8311_codec_cfg_t codec_cfg = {};
    codec_cfg.ctrl_if      = ctrl_if;
    codec_cfg.gpio_if      = gpio_if;
    codec_cfg.codec_mode   = ESP_CODEC_DEV_WORK_MODE_BOTH;
    codec_cfg.pa_pin       = static_cast<int16_t>(PIN_PA);
    codec_cfg.pa_reverted  = false;
    codec_cfg.master_mode  = false; // ESP32-P4 is the I2S clock master, ES8311 is the slave
    codec_cfg.use_mclk     = (PIN_MCLK != GPIO_NUM_NC);
    const audio_codec_if_t* codec_if = es8311_codec_new(&codec_cfg);
    if (codec_if == nullptr) {
        ESP_LOGE(TAG, "Failed to create ES8311 codec interface");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {};
    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN_OUT;
    dev_cfg.codec_if = codec_if;
    dev_cfg.data_if  = data_if;
    s_codec_dev = esp_codec_dev_new(&dev_cfg);
    if (s_codec_dev == nullptr) {
        ESP_LOGE(TAG, "Failed to create ES8311 codec device");
        return ESP_FAIL;
    }

    // NOTE: the ES8311 driver only accepts set_out_vol/set_in_gain once the codec is open
    // (it checks an internal is_open flag set by open()), so both must come after this call.
    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = BITS_PER_SAMPLE;
    fs.channel         = CHANNEL_COUNT;
    fs.sample_rate     = SAMPLE_RATE_HZ;
    if (esp_codec_dev_open(s_codec_dev, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Failed to open ES8311 codec device");
        return ESP_FAIL;
    }

    if (esp_codec_dev_set_out_vol(s_codec_dev, DEFAULT_VOLUME) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "Failed to set ES8311 output volume");
    }
    if (esp_codec_dev_set_in_gain(s_codec_dev, DEFAULT_MIC_GAIN_DB) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "Failed to set ES8311 mic gain");
    }

    ESP_LOGI(TAG,
             "ES8311 initialized on I2S%d (MCLK:%d BCLK:%d LRCK:%d DOUT:%d DIN:%d, PA:%d), %lu Hz / %u-bit / %u ch, "
             "vol:%d%% mic_gain:%ddB",
             i2s_port, PIN_MCLK, PIN_BCLK, PIN_LRCK, PIN_DOUT, PIN_DIN, PIN_PA,
             static_cast<unsigned long>(SAMPLE_RATE_HZ), BITS_PER_SAMPLE, CHANNEL_COUNT, DEFAULT_VOLUME,
             static_cast<int>(DEFAULT_MIC_GAIN_DB));

    return ESP_OK;
}

} // namespace Feature::Audio::ES8311
