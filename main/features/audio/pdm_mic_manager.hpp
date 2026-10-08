#pragma once

#include <cstdint>
#include <cstring>

#include "boards/board_config.hpp"
#include "boards/pin_utils.hpp"
#include "driver/gpio.h"
#include "driver/i2s_pdm.h"
#include "esp_err.h"
#include "esp_log.h"

// Onboard PDM MEMS microphone (e.g. the CrowPanel Advanced's MMICT5838 on GPIO24/25) as the
// source of the "Mic-In" widget. The ESP32-P4 can only receive PDM on I2S0; its PDM-to-PCM
// filter delivers 16-bit mono PCM at the engine's sample rate, which SynthEngine pulls once per
// block via read_block() (non-blocking, see SynthEngine::setMicInput()).
namespace Feature::Audio::PdmMic {

namespace {
constexpr uint32_t SAMPLE_RATE_HZ = 44100; // fixed project-wide, same as every output sink
constexpr int I2S_PORT = 0;                // PDM RX exists on I2S0 only (SOC_I2S_PDM_MAX_RX_LINES)
constexpr gpio_num_t PIN_CLK = static_cast<gpio_num_t>(CONFIG_ONBOARD_AUDIO_PDM_MIC_PIN_CLK);
constexpr gpio_num_t PIN_DIN = static_cast<gpio_num_t>(CONFIG_ONBOARD_AUDIO_PDM_MIC_PIN_DIN);
constexpr float GAIN = static_cast<float>(CONFIG_ONBOARD_AUDIO_PDM_MIC_GAIN);

#if !defined(CONFIG_BOARD_CROWPANEL_5INCH)
static_assert(!Board::pin_in_list(PIN_CLK, CurrentBoard::RESERVED_PINS),
              "ONBOARD_AUDIO_PDM_MIC_PIN_CLK conflicts with a pin already used by the current board");
static_assert(!Board::pin_in_list(PIN_DIN, CurrentBoard::RESERVED_PINS),
              "ONBOARD_AUDIO_PDM_MIC_PIN_DIN conflicts with a pin already used by the current board");
#endif
static_assert(PIN_CLK == GPIO_NUM_NC || PIN_CLK != PIN_DIN, "PDM mic CLK and DIN must be distinct pins");
#if defined(CONFIG_ONBOARD_AUDIO_ES8311)
static_assert(CONFIG_ONBOARD_AUDIO_ES8311_I2S_PORT != I2S_PORT,
              "The PDM microphone needs I2S0 - move ONBOARD_AUDIO_ES8311_I2S_PORT to 1 or 2");
#endif
#if defined(CONFIG_ONBOARD_AUDIO_WM8960)
static_assert(CONFIG_ONBOARD_AUDIO_WM8960_I2S_PORT != I2S_PORT,
              "The PDM microphone needs I2S0 - move ONBOARD_AUDIO_WM8960_I2S_PORT to 1 or 2");
#endif
#if defined(CONFIG_ONBOARD_AUDIO_NS4168)
static_assert(CONFIG_ONBOARD_AUDIO_NS4168_I2S_PORT != I2S_PORT,
              "The PDM microphone needs I2S0 - move ONBOARD_AUDIO_NS4168_I2S_PORT to 1 or 2");
#endif

inline i2s_chan_handle_t s_rx_handle = nullptr;
} // namespace

inline bool available() { return s_rx_handle != nullptr; }

/** @brief Create + enable the PDM RX channel (ONBOARD_AUDIO_PDM_MIC). */
inline esp_err_t init() {
    constexpr const char* TAG = "FeatureAudioPdmMic";
    if (PIN_CLK == GPIO_NUM_NC || PIN_DIN == GPIO_NUM_NC) {
        ESP_LOGW(TAG, "PDM microphone pins not configured (CLK/DIN left at -1)");
        return ESP_ERR_NOT_SUPPORTED;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = CONFIG_AUDIO_I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = CONFIG_AUDIO_I2S_DMA_FRAME_NUM;
    i2s_chan_handle_t rx_handle = nullptr;
    esp_err_t err = i2s_new_channel(&chan_cfg, nullptr, &rx_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S%d RX channel: %s", I2S_PORT, esp_err_to_name(err));
        return err;
    }

    i2s_pdm_rx_config_t pdm_cfg = {};
    pdm_cfg.clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ);
    // PDM RX at 44.1 kHz needs MCLK = 44.1 kHz * 64 * 8 = 22.6 MHz (bclk_div 8 is the hardware
    // minimum), more than half of the 40 MHz XTAL that I2S defaults to on P4 < v3 ("sample rate
    // is too large"). The APLL is otherwise unused here and can be tuned to it exactly.
    pdm_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
    pdm_cfg.slot_cfg = I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    pdm_cfg.gpio_cfg.clk = PIN_CLK;
    pdm_cfg.gpio_cfg.din = PIN_DIN;
    if ((err = i2s_channel_init_pdm_rx_mode(rx_handle, &pdm_cfg)) != ESP_OK ||
        (err = i2s_channel_enable(rx_handle)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start PDM RX: %s", esp_err_to_name(err));
        i2s_del_channel(rx_handle);
        return err;
    }
    s_rx_handle = rx_handle;
    ESP_LOGI(TAG, "PDM microphone on I2S%d (CLK:%d DIN:%d), %lu Hz mono, gain x%d", I2S_PORT, PIN_CLK,
             PIN_DIN, static_cast<unsigned long>(SAMPLE_RATE_HZ), CONFIG_ONBOARD_AUDIO_PDM_MIC_GAIN);
    return ESP_OK;
}

/**
 * @brief Fill `frames` mono samples (+/-1.0) for the current render block. Never blocks: takes
 *        whatever the DMA ring holds and pads with silence (startup, or a momentary shortfall).
 *        Called from the render task only (SynthEngine::setMicInput()).
 */
inline void read_block(float* block, size_t frames) {
    static int16_t pcm[1024];
    size_t got_bytes = 0;
    if (s_rx_handle != nullptr && frames <= sizeof(pcm) / sizeof(pcm[0])) {
        i2s_channel_read(s_rx_handle, pcm, frames * sizeof(int16_t), &got_bytes, 0);
    }
    const size_t got = got_bytes / sizeof(int16_t);
    for (size_t i = 0; i < frames; ++i) {
        float v = i < got ? static_cast<float>(pcm[i]) * (GAIN / 32768.0f) : 0.0f;
        block[i] = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
    }
}

} // namespace Feature::Audio::PdmMic
