#pragma once

#include "app_core.hpp"
#include "boards/board_config.hpp"
#include "boards/board_init.hpp"
#include "boards/pin_utils.hpp"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <cmath>

// jonlidgard/wm8960 v0.0.3's public header has no extern "C" guard, so its C-linkage
// wm8960_* functions (compiled as plain C in wm8960.c) would otherwise be looked up under
// C++-mangled names here and fail to link. Wrap the include ourselves instead of patching
// the vendored header (which a fresh component fetch would just overwrite).
extern "C" {
#include "wm8960.h"
}

// wm8960_setHeadphoneVolumeDB()/setSpeakerVolumeDB() each collapse 4 underlying I2C register
// writes (L-volume, R-volume, then the OUT1VU/SPKVU "apply now" bit on each of L and R) into
// one aggregate ESP_OK/ESP_FAIL, discarding which one actually failed and the real esp_err_t
// (I2C NACK vs timeout vs ...) from the driver. Since the WM8960 control interface is
// write-only (no register readback), that one bit is otherwise the only signal available that
// a write silently didn't land. wm8960_register_write_bit()/_multi_bits() have external
// (non-static) linkage in wm8960.c but aren't declared in the public wm8960.h, so redeclare
// them here to call them directly for the diagnostic breakdown in init() below.
extern "C" {
esp_err_t wm8960_register_write_bit(wm8960_t* wm8960, uint8_t register_address, uint8_t bit_number, bool bit_value);
esp_err_t wm8960_register_write_multi_bits(wm8960_t* wm8960, uint8_t register_address,
                                           uint8_t setting_msb_num, uint8_t setting_lsb_num,
                                           uint8_t setting);
uint8_t wm8960_convertDBtoSetting(float dB, float offset, float stepSize, float minDB, float maxDB);
}

// Generic WM8960 codec support (e.g. a Waveshare WM8960 Audio HAT on a Raspberry-Pi-style
// 40-pin header, or any other WM8960 wiring). Unlike the ES8311 path (es8311_manager.hpp),
// this codec is not wrapped in esp_codec_dev - there is no WM8960 audio_codec_if_t
// implementation available, and the user wants direct access to the chip object. So this
// file owns its own I2S channel (read/write directly via i2s_channel_*) and exposes the
// wm8960_t control object as-is.
namespace Feature::Audio {
namespace {
// User-configurable pins (Kconfig), shared by Feature::Audio::WM8960 and
// Feature::Audio::WM8960Button below. Since these can be set to any GPIO, they're checked
// at compile time against the current board's physically-reserved pins (boards/*.hpp
// RESERVED_PINS), the "I2C Subsystem" pins (no longer board-fixed either), and each other.
constexpr gpio_num_t WM8960_PIN_BCLK   = static_cast<gpio_num_t>(CONFIG_ONBOARD_AUDIO_WM8960_PIN_BCLK);
constexpr gpio_num_t WM8960_PIN_LRCK   = static_cast<gpio_num_t>(CONFIG_ONBOARD_AUDIO_WM8960_PIN_LRCK);
constexpr gpio_num_t WM8960_PIN_DIN    = static_cast<gpio_num_t>(CONFIG_ONBOARD_AUDIO_WM8960_PIN_DIN);
constexpr gpio_num_t WM8960_PIN_DOUT   = static_cast<gpio_num_t>(CONFIG_ONBOARD_AUDIO_WM8960_PIN_DOUT);
constexpr gpio_num_t WM8960_PIN_BUTTON = static_cast<gpio_num_t>(CONFIG_ONBOARD_AUDIO_WM8960_PIN_BUTTON);

constexpr bool wm8960_pin_is_reserved(gpio_num_t pin) {
    if (pin == GPIO_NUM_NC) return false;
#if defined(CONFIG_APP_ENABLE_USB_DEVICE)
    if (pin == GPIO_NUM_26 || pin == GPIO_NUM_27) return true;
#endif
    if (Board::pin_in_list(pin, CurrentBoard::RESERVED_PINS)) return true;
#if defined(CONFIG_APP_ENABLE_I2C0)
    if (pin == static_cast<gpio_num_t>(CONFIG_I2C0_PIN_SDA) ||
        pin == static_cast<gpio_num_t>(CONFIG_I2C0_PIN_SCL)) return true;
#endif
#if defined(CONFIG_APP_ENABLE_I2C1)
    if (pin == static_cast<gpio_num_t>(CONFIG_I2C1_PIN_SDA) ||
        pin == static_cast<gpio_num_t>(CONFIG_I2C1_PIN_SCL)) return true;
#endif
    return false;
}

static_assert(!wm8960_pin_is_reserved(WM8960_PIN_BCLK),
              "ONBOARD_AUDIO_WM8960_PIN_BCLK conflicts with a pin already used by the current board");
static_assert(!wm8960_pin_is_reserved(WM8960_PIN_LRCK),
              "ONBOARD_AUDIO_WM8960_PIN_LRCK conflicts with a pin already used by the current board");
static_assert(!wm8960_pin_is_reserved(WM8960_PIN_DIN),
              "ONBOARD_AUDIO_WM8960_PIN_DIN conflicts with a pin already used by the current board");
static_assert(!wm8960_pin_is_reserved(WM8960_PIN_DOUT),
              "ONBOARD_AUDIO_WM8960_PIN_DOUT conflicts with a pin already used by the current board");
static_assert(!wm8960_pin_is_reserved(WM8960_PIN_BUTTON),
              "ONBOARD_AUDIO_WM8960_PIN_BUTTON conflicts with a pin already used by the current board");
static_assert(WM8960_PIN_BCLK != WM8960_PIN_LRCK && WM8960_PIN_BCLK != WM8960_PIN_DIN &&
              WM8960_PIN_BCLK != WM8960_PIN_DOUT && WM8960_PIN_LRCK != WM8960_PIN_DIN &&
              WM8960_PIN_LRCK != WM8960_PIN_DOUT && WM8960_PIN_DIN != WM8960_PIN_DOUT,
              "ONBOARD_AUDIO_WM8960_PIN_{BCLK,LRCK,DIN,DOUT} must all be distinct");
static_assert(WM8960_PIN_BUTTON == GPIO_NUM_NC ||
              (WM8960_PIN_BUTTON != WM8960_PIN_BCLK && WM8960_PIN_BUTTON != WM8960_PIN_LRCK &&
               WM8960_PIN_BUTTON != WM8960_PIN_DIN && WM8960_PIN_BUTTON != WM8960_PIN_DOUT),
              "ONBOARD_AUDIO_WM8960_PIN_BUTTON conflicts with one of the WM8960 I2S data pins");
} // namespace
} // namespace Feature::Audio

namespace Feature::Audio::WM8960Button {

/** @brief Fired once per confirmed (debounced) button press. */
struct Event {};
using EventCallback = void (*)(const Event& event);

namespace {
inline EventCallback s_callback         = nullptr;
inline TaskHandle_t  s_debounce_task    = nullptr;
constexpr uint32_t   DEBOUNCE_MS        = 50;

// esp_timer is not ISR-safe to call from a GPIO ISR, so debouncing is handed off to a
// small dedicated task via a (ISR-safe) task notification instead.
void IRAM_ATTR gpio_isr_handler(void* /*arg*/) {
    BaseType_t higher_priority_task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_debounce_task, &higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

void debounce_task(void* /*arg*/) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
        if (gpio_get_level(WM8960_PIN_BUTTON) != 0) continue; // still active-low pressed?
        if (s_callback != nullptr) {
            const Event event{};
            s_callback(event);
        }
    }
}
} // namespace

/**
 * @brief Register a callback invoked once per debounced button press. Pass nullptr to
 *        unregister. Only one callback is stored at a time.
 */
inline void set_callback(EventCallback callback) {
    s_callback = callback;
}

/**
 * @brief Configure the WM8960 board's onboard button (GPIO input + ISR + debounce timer).
 *        Called from Feature::Audio::WM8960::init(); safe to skip if the pin isn't configured.
 */
inline esp_err_t init_button() {
    constexpr const char* TAG = "FeatureAudioWM8960Button";
    // if constexpr, not a plain if: WM8960_PIN_BUTTON is a compile-time constant, and with
    // the default -1 (GPIO_NUM_NC) a plain runtime "if" still leaves the else branch's
    // "1ULL << WM8960_PIN_BUTTON" as a constant expression the compiler folds and rejects
    // (-Werror=shift-count-negative) even though it would never run. if constexpr discards
    // the untaken branch instead of just skipping it at runtime.
    if constexpr (WM8960_PIN_BUTTON == GPIO_NUM_NC) {
        return ESP_ERR_NOT_SUPPORTED;
    } else {
        // A GPIO debounce poll, not audio sample data, so this stays on the main core
        // (see app_core.hpp) rather than the audio core alongside the codec's I2S/I2C work.
        if (xTaskCreatePinnedToCore(debounce_task, "wm8960_btn", 2048, nullptr, 5, &s_debounce_task,
                                    App::MAIN_CORE) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create button debounce task");
            return ESP_ERR_NO_MEM;
        }

        gpio_config_t io_conf = {};
        io_conf.intr_type    = GPIO_INTR_NEGEDGE;
        io_conf.mode          = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask  = (1ULL << WM8960_PIN_BUTTON);
        io_conf.pull_up_en    = GPIO_PULLUP_ENABLE;
        io_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
        gpio_config(&io_conf);

        // First caller wins - nothing else in this codebase installs the shared ISR service yet.
        esp_err_t err = gpio_install_isr_service(0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { // INVALID_STATE = already installed
            ESP_LOGE(TAG, "Failed to install GPIO ISR service: %s", esp_err_to_name(err));
            return err;
        }
        err = gpio_isr_handler_add(WM8960_PIN_BUTTON, gpio_isr_handler, nullptr);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add GPIO ISR handler: %s", esp_err_to_name(err));
            return err;
        }

        ESP_LOGI(TAG, "WM8960 button initialized on GPIO%d", WM8960_PIN_BUTTON);
        return ESP_OK;
    }
}

} // namespace Feature::Audio::WM8960Button

namespace Feature::Audio::WM8960 {

namespace {
// Fixed project-wide (see es8311_manager.hpp). Also happens to be the only rate the PLL
// constants below (N/K/prescale/SYSCLKDIV) are verified for: they're the values documented
// by the wm8960 driver for a WM8960 board with its own onboard 24.0 MHz MCLK crystal
// oscillator (there is no MCLK line on a Raspberry-Pi header, and the ESP32-P4 never drives
// one) producing an 11.2896 MHz SYSCLK, i.e. the 44.1 kHz family only.
constexpr uint32_t SAMPLE_RATE_HZ  = 44100;
constexpr uint8_t  BITS_PER_SAMPLE = 16;
constexpr float    DEFAULT_HEADPHONE_VOLUME_DB = 0.0f;
constexpr float    DEFAULT_SPEAKER_VOLUME_DB   = 0.0f;
constexpr float    DEFAULT_MIC_PGA_GAIN_DB     = 24.0f; // line-input/PGA volume (LINVOL/RINVOL)
constexpr float    DEFAULT_MIC_BOOST_GAIN_DB   = 0.0f;  // one of 0/13/20/29 dB, see set_mic_boost_gain_db()
constexpr float    DEFAULT_ADC_VOLUME_DB       = 0.0f;  // digital trim after the ADC
constexpr float    DEFAULT_DAC_VOLUME_DB       = 0.0f;  // digital trim before the DAC; not written
                                                         // during init() -- 0dB is also this
                                                         // register's power-on-reset value

inline wm8960_t         s_wm8960    = {};
inline i2s_chan_handle_t s_tx_handle = nullptr;
inline i2s_chan_handle_t s_rx_handle = nullptr;

// True DMA-ring-underrun tracking - see the identical comment in es8311_manager.hpp
// (on_send_q_ovf fires from I2S driver ISR context once the ring has genuinely run out of
// real data, as opposed to a render-*budget* miss the ring's headroom usually absorbs).
inline std::atomic<uint32_t> s_underrun_count{0};
inline std::atomic<int64_t> s_last_underrun_us{0};

bool IRAM_ATTR onSendQueueOverflow(i2s_chan_handle_t /*handle*/, i2s_event_data_t * /*event*/, void * /*user_ctx*/) {
    s_underrun_count.fetch_add(1, std::memory_order_relaxed);
    s_last_underrun_us.store(esp_timer_get_time(), std::memory_order_relaxed);
    return false;
}

// The WM8960 control interface is write-only (no register readback, see
// main/boards/board_init.cpp's I2C scan and the per-write init() diagnostics below for the
// hardware-debugging background), so every get_*() below returns the last value this API
// successfully *commanded*, not something read back from the chip. Seeded with the same
// defaults init() applies, so a get_*() right after a clean init() already reflects reality.
inline float s_headphone_volume_db = DEFAULT_HEADPHONE_VOLUME_DB;
inline float s_speaker_volume_db   = DEFAULT_SPEAKER_VOLUME_DB;
inline float s_line_input_volume_db = DEFAULT_MIC_PGA_GAIN_DB;
inline float s_mic_boost_gain_db    = DEFAULT_MIC_BOOST_GAIN_DB;
inline float s_adc_volume_db        = DEFAULT_ADC_VOLUME_DB;
inline float s_dac_volume_db        = DEFAULT_DAC_VOLUME_DB;

// Rounds an arbitrary dB value to whichever of the WM8960's 4 fixed mic boost steps
// (0/13/20/29 dB) is closest, then returns both the enum setting to write and the dB value
// actually applied (for the s_mic_boost_gain_db shadow) so callers asking for e.g. 15 dB get
// told the truth (20 dB) instead of silently getting 15 dB back from a later get_*() call.
inline void mic_boost_gain_round(float requested_db, uint8_t* out_setting, float* out_applied_db) {
    constexpr float STEPS_DB[] = {0.0f, 13.0f, 20.0f, 29.0f};
    constexpr uint8_t STEPS_SETTING[] = {
        WM8960_MIC_BOOST_GAIN_0DB, WM8960_MIC_BOOST_GAIN_13DB,
        WM8960_MIC_BOOST_GAIN_20DB, WM8960_MIC_BOOST_GAIN_29DB};
    size_t closest = 0;
    float closest_diff = std::fabs(requested_db - STEPS_DB[0]);
    for (size_t i = 1; i < 4; ++i) {
        const float diff = std::fabs(requested_db - STEPS_DB[i]);
        if (diff < closest_diff) {
            closest = i;
            closest_diff = diff;
        }
    }
    *out_setting = STEPS_SETTING[closest];
    *out_applied_db = STEPS_DB[closest];
}
} // namespace

/**
 * @brief Get the WM8960 control object so callers can drive chip-specific features directly
 *        (any wm8960_* function from the vendored jonlidgard/wm8960 component).
 *        Returns nullptr if ONBOARD_AUDIO_WM8960 init() failed/was not called.
 */
inline wm8960_t* get_object() {
    return s_wm8960.handle != nullptr ? &s_wm8960 : nullptr;
}

/** @brief I2S TX channel (ESP32-P4 -> WM8960 DAC, speaker/headphone data). */
inline i2s_chan_handle_t get_tx_handle() {
    return s_tx_handle;
}

/** @brief I2S RX channel (WM8960 ADC -> ESP32-P4, microphone data). */
inline i2s_chan_handle_t get_rx_handle() {
    return s_rx_handle;
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
 * @brief Set the analog headphone output volume. Range -73..+6 dB (below -73, the output
 *        mutes); out-of-range values are clamped by the chip itself.
 */
inline esp_err_t set_headphone_volume_db(float dB) {
    if (s_wm8960.handle == nullptr) return ESP_ERR_INVALID_STATE;
    const esp_err_t err = wm8960_setHeadphoneVolumeDB(&s_wm8960, dB);
    if (err == ESP_OK) s_headphone_volume_db = dB;
    return err;
}

/** @brief Last dB value set_headphone_volume_db() applied (see the write-only note above). */
inline float get_headphone_volume_db() { return s_headphone_volume_db; }

/** @brief Enable/disable both headphone output amps (mute/unmute the jack). */
inline esp_err_t set_headphone_enabled(bool enabled) {
    if (s_wm8960.handle == nullptr) return ESP_ERR_INVALID_STATE;
    return enabled ? wm8960_enableHeadphones(&s_wm8960) : wm8960_disableHeadphones(&s_wm8960);
}

/**
 * @brief Set the analog speaker output volume. Range -73..+6 dB (below -73, the output
 *        mutes); out-of-range values are clamped by the chip itself.
 */
inline esp_err_t set_speaker_volume_db(float dB) {
    if (s_wm8960.handle == nullptr) return ESP_ERR_INVALID_STATE;
    const esp_err_t err = wm8960_setSpeakerVolumeDB(&s_wm8960, dB);
    if (err == ESP_OK) s_speaker_volume_db = dB;
    return err;
}

/** @brief Last dB value set_speaker_volume_db() applied (see the write-only note above). */
inline float get_speaker_volume_db() { return s_speaker_volume_db; }

/** @brief Enable/disable both speaker (Class-D) output amps. */
inline esp_err_t set_speaker_enabled(bool enabled) {
    if (s_wm8960.handle == nullptr) return ESP_ERR_INVALID_STATE;
    return enabled ? wm8960_enableSpeakers(&s_wm8960) : wm8960_disableSpeakers(&s_wm8960);
}

/**
 * @brief Mute/unmute the DAC feeding both the headphone and speaker outputs at once
 *        (see the "DAC output" comment in init() -- both share the same output mixer path).
 */
inline esp_err_t set_dac_mute(bool mute) {
    if (s_wm8960.handle == nullptr) return ESP_ERR_INVALID_STATE;
    return mute ? wm8960_enableDacMute(&s_wm8960) : wm8960_disableDacMute(&s_wm8960);
}

/**
 * @brief Set the digital gain applied to the DAC path (both channels), independent of the
 *        analog headphone/speaker volumes above. Range -97..+30 dB (below -97, mutes).
 */
inline esp_err_t set_dac_volume_db(float dB) {
    if (s_wm8960.handle == nullptr) return ESP_ERR_INVALID_STATE;
    const esp_err_t left_err = wm8960_setDacLeftDigitalVolumeDB(&s_wm8960, dB);
    const esp_err_t right_err = wm8960_setDacRightDigitalVolumeDB(&s_wm8960, dB);
    if (left_err != ESP_OK || right_err != ESP_OK) return ESP_FAIL;
    s_dac_volume_db = dB;
    return ESP_OK;
}

/** @brief Last dB value set_dac_volume_db() applied (see the write-only note above). */
inline float get_dac_volume_db() { return s_dac_volume_db; }

/**
 * @brief Set the line-input/PGA gain (both channels) feeding the ADC -- the stage the
 *        onboard electret mics run through (see init()'s "Mic input" wiring). Range
 *        -17.25..+30 dB in 0.75 dB steps.
 */
inline esp_err_t set_line_input_volume_db(float dB) {
    if (s_wm8960.handle == nullptr) return ESP_ERR_INVALID_STATE;
    const esp_err_t left_err = wm8960_setLINVOLDB(&s_wm8960, dB);
    const esp_err_t right_err = wm8960_setRINVOLDB(&s_wm8960, dB);
    if (left_err != ESP_OK || right_err != ESP_OK) return ESP_FAIL;
    s_line_input_volume_db = dB;
    return ESP_OK;
}

/** @brief Last dB value set_line_input_volume_db() applied (see the write-only note above). */
inline float get_line_input_volume_db() { return s_line_input_volume_db; }

/** @brief Mute/unmute the line-input/PGA stage (both channels), upstream of the ADC. */
inline esp_err_t set_line_input_mute(bool mute) {
    if (s_wm8960.handle == nullptr) return ESP_ERR_INVALID_STATE;
    esp_err_t left_err;
    esp_err_t right_err;
    if (mute) {
        left_err = wm8960_enableLINMUTE(&s_wm8960);
        right_err = wm8960_enableRINMUTE(&s_wm8960);
    } else {
        left_err = wm8960_disableLINMUTE(&s_wm8960);
        right_err = wm8960_disableRINMUTE(&s_wm8960);
    }
    return (left_err == ESP_OK && right_err == ESP_OK) ? ESP_OK : ESP_FAIL;
}

/**
 * @brief Set the mic boost stage (both channels), ahead of the line-input/PGA gain above.
 *        The WM8960 only has 4 fixed steps (0/13/20/29 dB) -- any other value is rounded to
 *        the nearest one, and get_mic_boost_gain_db() afterwards reports what was actually
 *        applied, not what was requested.
 */
inline esp_err_t set_mic_boost_gain_db(float dB) {
    if (s_wm8960.handle == nullptr) return ESP_ERR_INVALID_STATE;
    uint8_t setting = 0;
    float applied_db = 0.0f;
    mic_boost_gain_round(dB, &setting, &applied_db);
    const esp_err_t left_err = wm8960_setLMICBOOST(&s_wm8960, static_cast<int8_t>(setting));
    const esp_err_t right_err = wm8960_setRMICBOOST(&s_wm8960, static_cast<int8_t>(setting));
    if (left_err != ESP_OK || right_err != ESP_OK) return ESP_FAIL;
    s_mic_boost_gain_db = applied_db;
    return ESP_OK;
}

/** @brief Last dB value set_mic_boost_gain_db() actually applied (rounded, see above). */
inline float get_mic_boost_gain_db() { return s_mic_boost_gain_db; }

/** @brief Enable/disable the mic bias voltage supply for electret mics. */
inline esp_err_t set_mic_bias_enabled(bool enabled) {
    if (s_wm8960.handle == nullptr) return ESP_ERR_INVALID_STATE;
    return enabled ? wm8960_enableMicBias(&s_wm8960) : wm8960_disableMicBias(&s_wm8960);
}

/**
 * @brief Set the digital gain applied to the ADC path (both channels), downstream of the
 *        line-input/PGA and mic boost stages above. Range -97..+30 dB (below -97, mutes).
 */
inline esp_err_t set_adc_volume_db(float dB) {
    if (s_wm8960.handle == nullptr) return ESP_ERR_INVALID_STATE;
    const esp_err_t left_err = wm8960_setAdcLeftDigitalVolumeDB(&s_wm8960, dB);
    const esp_err_t right_err = wm8960_setAdcRightDigitalVolumeDB(&s_wm8960, dB);
    if (left_err != ESP_OK || right_err != ESP_OK) return ESP_FAIL;
    s_adc_volume_db = dB;
    return ESP_OK;
}

/** @brief Last dB value set_adc_volume_db() applied (see the write-only note above). */
inline float get_adc_volume_db() { return s_adc_volume_db; }

/**
 * @brief Initialize I2S bus and WM8960 codec (ONBOARD_AUDIO_WM8960)
 */
inline esp_err_t init() {
    constexpr const char* TAG = "FeatureAudioWM8960";
    ESP_LOGI(TAG, "Initializing WM8960 codec for %s", CurrentBoard::BOARD_NAME);

    if (WM8960_PIN_BCLK == GPIO_NUM_NC || WM8960_PIN_LRCK == GPIO_NUM_NC ||
        WM8960_PIN_DOUT == GPIO_NUM_NC) {
        ESP_LOGW(TAG, "WM8960 playback pins not configured (BCLK/LRCK/DOUT left at -1)");
        return ESP_ERR_NOT_SUPPORTED;
    }

    const int i2s_port = CONFIG_ONBOARD_AUDIO_WM8960_I2S_PORT;
    const bool enable_rx = WM8960_PIN_DIN != GPIO_NUM_NC;

    // --- I2C control interface (register access, port selected via
    // ONBOARD_AUDIO_WM8960_I2C_PORT - see "I2C Subsystem" Kconfig) ---
    const int i2c_port = CONFIG_ONBOARD_AUDIO_WM8960_I2C_PORT;
    i2c_master_bus_handle_t i2c_bus = Board::get_i2c_bus_handle(i2c_port);
    if (i2c_bus == nullptr) {
        ESP_LOGE(TAG, "I2C port %d not enabled - check I2C Subsystem Kconfig", i2c_port);
        return ESP_ERR_INVALID_STATE;
    }
    s_wm8960.i2c_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    s_wm8960.i2c_config.device_address  = WM8960_ADDR;
    s_wm8960.i2c_config.scl_speed_hz    = Board::I2C_BUS_FREQ_HZ;
    esp_err_t err = i2c_master_bus_add_device(i2c_bus, &s_wm8960.i2c_config, &s_wm8960.handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add WM8960 I2C device (addr 0x%02x): %s", WM8960_ADDR, esp_err_to_name(err));
        return err;
    }

    // --- WM8960 bring-up (note: wm8960_init() is declared but not implemented by the
    // vendored jonlidgard/wm8960 v0.0.3 component, so bring-up is done call-by-call here,
    // matching the component's own reference example) ---
    if (wm8960_reset(&s_wm8960) != ESP_OK) { // must be called first
        ESP_LOGE(TAG, "WM8960 reset failed (I2C ack?) - check wiring/power");
        return ESP_FAIL;
    }
    wm8960_enableVREF(&s_wm8960);
    wm8960_enableVMID(&s_wm8960);

    // Mic input: onboard electret mics wired single-ended to LINPUT1/RINPUT1 with mic bias.
    wm8960_connectLMN1(&s_wm8960);
    wm8960_connectRMN1(&s_wm8960);
    wm8960_pgaLeftNonInvSignalSelect(&s_wm8960, WM8960_PGAL_VMID);
    wm8960_pgaRightNonInvSignalSelect(&s_wm8960, WM8960_PGAR_VMID);
    wm8960_disableLINMUTE(&s_wm8960);
    wm8960_disableRINMUTE(&s_wm8960);
    wm8960_setLINVOLDB(&s_wm8960, DEFAULT_MIC_PGA_GAIN_DB);
    wm8960_setRINVOLDB(&s_wm8960, DEFAULT_MIC_PGA_GAIN_DB);
    wm8960_enableLMIC(&s_wm8960);
    wm8960_enableRMIC(&s_wm8960);
    wm8960_connectLMIC2B(&s_wm8960);
    wm8960_connectRMIC2B(&s_wm8960);
    wm8960_setLMICBOOST(&s_wm8960, WM8960_MIC_BOOST_GAIN_0DB);
    wm8960_setRMICBOOST(&s_wm8960, WM8960_MIC_BOOST_GAIN_0DB);
    wm8960_enableMicBias(&s_wm8960);
    wm8960_enableAINL(&s_wm8960);
    wm8960_enableAINR(&s_wm8960);
    wm8960_enableAdcLeft(&s_wm8960);
    wm8960_enableAdcRight(&s_wm8960);
    wm8960_setAdcLeftDigitalVolumeDB(&s_wm8960, 0.0f);
    wm8960_setAdcRightDigitalVolumeDB(&s_wm8960, 0.0f);

    // DAC output: route both DACs into the output mixers, feeding headphone + speaker.
    wm8960_enableLD2LO(&s_wm8960);
    wm8960_enableRD2RO(&s_wm8960);
    wm8960_enableLOMIX(&s_wm8960);
    wm8960_enableROMIX(&s_wm8960);
    wm8960_enableDacLeft(&s_wm8960);
    wm8960_enableDacRight(&s_wm8960);
    wm8960_disableDacMute(&s_wm8960); // DAC boots soft-muted

    // None of the wm8960_* calls above check their esp_err_t (matching the vendored
    // component's own reference example) -- a silently-NACKed I2C write partway through
    // would otherwise be invisible, and the WM8960 control interface is write-only (no
    // register readback), so a failed write here has no other symptom until the analog
    // output just doesn't work. Everything from here on is checked and logged per I2C
    // transaction instead of going through the library's own aggregate helpers, because
    // wm8960_enableHeadphones()/enableSpeakers() combine their two sub-writes with a
    // bitwise "&" instead of "&&" (see jonlidgard/wm8960 wm8960.c) -- with ESP_FAIL == -1,
    // "one write ESP_OK (0) and the other ESP_FAIL (-1)" folds to 0 == ESP_OK, silently
    // hiding exactly the kind of single-write failure this is trying to catch. Same result
    // as calling the higher-level DB-based setters otherwise, just with per-write visibility.
    bool any_output_write_failed = false;
    auto log_step = [&any_output_write_failed](const char* what, esp_err_t err) {
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "  %-28s OK", what);
        } else {
            ESP_LOGW(TAG, "  %-28s FAILED (%s)", what, esp_err_to_name(err));
            any_output_write_failed = true;
        }
    };

    ESP_LOGI(TAG, "WM8960 output bring-up (target: headphone %ddB, speaker %ddB):",
             static_cast<int>(DEFAULT_HEADPHONE_VOLUME_DB), static_cast<int>(DEFAULT_SPEAKER_VOLUME_DB));
    log_step("enable left headphone", wm8960_enableLeftHeadphone(&s_wm8960));
    log_step("enable right headphone", wm8960_enableRightHeadphone(&s_wm8960));
    log_step("enable left speaker", wm8960_enableLeftSpeaker(&s_wm8960));
    log_step("enable right speaker", wm8960_enableRightSpeaker(&s_wm8960));

    const uint8_t hp_setting = wm8960_convertDBtoSetting(
        DEFAULT_HEADPHONE_VOLUME_DB, WM8960_HP_GAIN_OFFSET, WM8960_HP_GAIN_STEPSIZE,
        WM8960_HP_GAIN_MIN, WM8960_HP_GAIN_MAX);
    log_step("LOUT1 volume", wm8960_register_write_multi_bits(
        &s_wm8960, WM8960_REG_LOUT1_VOLUME, 6, 0, hp_setting));
    log_step("ROUT1 volume", wm8960_register_write_multi_bits(
        &s_wm8960, WM8960_REG_ROUT1_VOLUME, 6, 0, hp_setting));
    log_step("OUT1VU (apply, left)", wm8960_register_write_bit(
        &s_wm8960, WM8960_REG_LOUT1_VOLUME, 8, 1));
    log_step("OUT1VU (apply, right)", wm8960_register_write_bit(
        &s_wm8960, WM8960_REG_ROUT1_VOLUME, 8, 1));

    const uint8_t spk_setting = wm8960_convertDBtoSetting(
        DEFAULT_SPEAKER_VOLUME_DB, WM8960_SPEAKER_GAIN_OFFSET, WM8960_SPEAKER_GAIN_STEPSIZE,
        WM8960_SPEAKER_GAIN_MIN, WM8960_SPEAKER_GAIN_MAX);
    log_step("LOUT2 volume", wm8960_register_write_multi_bits(
        &s_wm8960, WM8960_REG_LOUT2_VOLUME, 6, 0, spk_setting));
    log_step("ROUT2 volume", wm8960_register_write_multi_bits(
        &s_wm8960, WM8960_REG_ROUT2_VOLUME, 6, 0, spk_setting));
    log_step("SPKVU (apply, left)", wm8960_register_write_bit(
        &s_wm8960, WM8960_REG_LOUT2_VOLUME, 8, 1));
    log_step("SPKVU (apply, right)", wm8960_register_write_bit(
        &s_wm8960, WM8960_REG_ROUT2_VOLUME, 8, 1));

    if (any_output_write_failed) {
        ESP_LOGW(TAG, "One or more WM8960 output I2C writes failed (see FAILED lines above) - "
                      "output is not fully configured despite the rest of init() succeeding");
    }

    // Clocking: PLL tuned for the HAT's onboard 24.0 MHz MCLK oscillator -> 11.2896 MHz
    // SYSCLK (44.1 kHz family), peripheral(slave) mode. The I2S channel below is now also
    // requested as I2S_ROLE_SLAVE (see notes there), so neither side masters BCLK/LRCLK.
    wm8960_enablePLL(&s_wm8960);
    wm8960_setPLLPRESCALE(&s_wm8960, WM8960_PLLPRESCALE_DIV_2);
    wm8960_setSMD(&s_wm8960, WM8960_PLL_MODE_FRACTIONAL);
    wm8960_setCLKSEL(&s_wm8960, WM8960_CLKSEL_PLL);
    wm8960_setSYSCLKDIV(&s_wm8960, WM8960_SYSCLK_DIV_BY_2);
    wm8960_setPLLN(&s_wm8960, 7);
    wm8960_setPLLK(&s_wm8960, 0x86, 0xC2, 0x26); // PLLK = 86C226h
    wm8960_setWL(&s_wm8960, WM8960_WL_16BIT);
    wm8960_enablePeripheralMode(&s_wm8960);

    // --- I2S data interface (BCLK/LRCK/DOUT/DIN, no MCLK - see note above). Originally brought
    // up *after* every I2C register write above, not before: once enabled, a MASTER I2S channel
    // free-runs its BCLK/WS clocks continuously regardless of whether real audio data is queued
    // yet, and on a WM8960 wired in over jumper leads those wires often run right alongside the
    // I2C SDA/SCL pair to the same board -- a plausible source of exactly the kind of random,
    // no-fixed-pattern I2C write failures the diagnostics above have been catching, unrelated to
    // bus speed (whole bus dropped to 50 kHz for this test, see Board::I2C_BUS_FREQ_HZ) or task
    // scheduling (the very first scan in Board::init() can already miss the device, before
    // anything else is even running).
    //
    // Channel below is now deliberately requested as I2S_ROLE_SLAVE instead of MASTER -- a
    // stronger version of that same test. With the WM8960 also left in peripheral/slave mode
    // (wm8960_enablePeripheralMode() above), neither side masters BCLK/WS anymore, so those
    // lines never toggle at all (see note below), not just delayed until after I2C bring-up.
    // No audio will play in this configuration since there's no clock source for either chip
    // to run off; if I2C failures persist anyway, that rules out I2S clock activity as the
    // cause outright rather than just narrowing the timing window.
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(i2s_port, I2S_ROLE_MASTER);
    chan_cfg.auto_clear_after_cb = true;
    // Shared with es8311_manager.hpp via the "Audio Subsystem" Kconfig menu
    // (AUDIO_I2S_DMA_DESC_NUM / AUDIO_I2S_DMA_FRAME_NUM) so both onboard I2S
    // codecs tune the same tradeoff together: default 6*240 = 1440 frames
    // (~32.6ms @ 44.1kHz) of DMA buffer -- audible clicks/pops during
    // playback (e.g. the "tone" CLI command, cmd_audio.hpp) trace back to
    // that: any writer stall longer than the buffer (a CLI/REPL task on
    // MAIN_CORE briefly losing the CPU to something else) underruns it, and
    // auto_clear_after_cb fills the gap with silence instead of repeating
    // stale audio -- audible either way as a discontinuity. More than
    // doubling the buffer (~87ms via 8*480) buys much more slack against
    // that without giving up the underrun-clears-to-silence safety net, at
    // the cost of latency -- tune via menuconfig.
    chan_cfg.dma_desc_num = CONFIG_AUDIO_I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = CONFIG_AUDIO_I2S_DMA_FRAME_NUM;
    // tx+rx created together (not as two separate i2s_new_channel calls): with a shared BCLK/WS
    // pair, requesting I2S_ROLE_MASTER for both used to make the driver auto-downgrade rx to
    // slave for us at i2s_channel_init_std_mode() time (i2s_channel_try_to_constitute_duplex(),
    // i2s_common.c), logging a harmless "switched from master to slave" W. Now that both tx and
    // rx are requested as I2S_ROLE_SLAVE up front, that downgrade path doesn't trigger and the
    // warning is gone -- but the single-call requirement still stands: splitting into two
    // i2s_new_channel() calls doesn't avoid a warning, it just moves it, since the second call
    // re-acquires the same port and trips i2s_platform's "already occupied" W instead. Note this
    // also means neither side masters BCLK/WS anymore: WM8960 stays in peripheral/slave mode
    // (wm8960_enablePeripheralMode() above) as before, but the ESP32-P4 side no longer drives
    // the clock either, so BCLK/WS get no clock source at all in this configuration.
    err = i2s_new_channel(&chan_cfg, &s_tx_handle, enable_rx ? &s_rx_handle : nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel on port %d: %s", i2s_port, esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            static_cast<i2s_data_bit_width_t>(BITS_PER_SAMPLE), I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_NC, // WM8960 board has its own onboard MCLK oscillator
            .bclk = WM8960_PIN_BCLK,
            .ws   = WM8960_PIN_LRCK,
            .dout = WM8960_PIN_DOUT,
            .din  = WM8960_PIN_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    if ((err = i2s_channel_init_std_mode(s_tx_handle, &std_cfg)) != ESP_OK ||
        (enable_rx && (err = i2s_channel_init_std_mode(s_rx_handle, &std_cfg)) != ESP_OK)) {
        ESP_LOGE(TAG, "Failed to configure I2S standard mode: %s", esp_err_to_name(err));
        return err;
    }

    // Must be registered before i2s_channel_enable() below - the driver rejects DMA event
    // callback registration once the channel is running.
    i2s_event_callbacks_t underrun_callbacks = {};
    underrun_callbacks.on_send_q_ovf = onSendQueueOverflow;
    err = i2s_channel_register_event_callback(s_tx_handle, &underrun_callbacks, nullptr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register I2S underrun callback: %s", esp_err_to_name(err));
    }

    if ((err = i2s_channel_enable(s_tx_handle)) != ESP_OK ||
        (enable_rx && (err = i2s_channel_enable(s_rx_handle)) != ESP_OK)) {
        ESP_LOGE(TAG, "Failed to enable I2S channel: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG,
             "WM8960 initialized on I2S%d (BCLK:%d LRCK:%d DOUT:%d DIN:%d%s), %lu Hz / %u-bit / 2 ch, "
             "headphone:%ddB speaker:%ddB mic_pga:%ddB",
             i2s_port, WM8960_PIN_BCLK, WM8960_PIN_LRCK, WM8960_PIN_DOUT, WM8960_PIN_DIN,
             enable_rx ? "" : " disabled",
             static_cast<unsigned long>(SAMPLE_RATE_HZ), BITS_PER_SAMPLE,
             static_cast<int>(DEFAULT_HEADPHONE_VOLUME_DB), static_cast<int>(DEFAULT_SPEAKER_VOLUME_DB),
             static_cast<int>(DEFAULT_MIC_PGA_GAIN_DB));

    const esp_err_t button_err = Feature::Audio::WM8960Button::init_button();
    if (button_err != ESP_OK && button_err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "WM8960 button init failed: %s", esp_err_to_name(button_err));
    }

    return ESP_OK;
}

} // namespace Feature::Audio::WM8960
