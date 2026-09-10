#pragma once

#include <cstddef>
#include <cstring>

#include "esp_ae_eq.h"
#include "esp_log.h"

#include "EspAudioEffectsAdapter.h"
#include "NodeState.h"
#include "NodeTypes.h"
#include "PresetParser.h"
#include "SynthConfig.h"

namespace
{
constexpr char kEspEqTag[] = "EspEq";

inline float espEqClampf(float value, float minimum, float maximum)
{
    return value < minimum ? minimum : (value > maximum ? maximum : value);
}

inline esp_ae_eq_filter_type_t espEqToHwFilterType(EqFilterKind kind)
{
    switch (kind)
    {
    case EqFilterKind::HighPass:
        return ESP_AE_EQ_FILTER_HIGH_PASS;
    case EqFilterKind::LowPass:
        return ESP_AE_EQ_FILTER_LOW_PASS;
    case EqFilterKind::HighShelf:
        return ESP_AE_EQ_FILTER_HIGH_SHELF;
    case EqFilterKind::LowShelf:
        return ESP_AE_EQ_FILTER_LOW_SHELF;
    case EqFilterKind::Peak:
    default:
        return ESP_AE_EQ_FILTER_PEAK;
    }
}

inline esp_ae_eq_filter_para_t espEqMakeFilterPara(EqFilterKind kind, float frequencyHz, float q, float gainDb)
{
    esp_ae_eq_filter_para_t para = {};
    para.filter_type = espEqToHwFilterType(kind);
    para.fc = static_cast<uint32_t>(espEqClampf(frequencyHz, 20.0f,
                                                  static_cast<float>(synth::kSampleRate / 2 - 1)));
    para.q = espEqClampf(q, 0.1f, 20.0f);
    para.gain = espEqClampf(gainDb, -15.0f, 15.0f);
    return para;
}

inline void espEqBypass(const float *inputL, const float *inputR, float *outputL, float *outputR, size_t frames)
{
    if (inputL != outputL)
        memcpy(outputL, inputL, frames * sizeof(float));
    if (inputR != outputR)
        memcpy(outputR, inputR, frames * sizeof(float));
}
} // namespace

inline void destroyEspEq(EspEqState &state);

inline bool initEspEq(EspEqState &state, EqFilterKind filterType, float frequencyHz, float q, float gainDb)
{
    destroyEspEq(state);
    state.filterType = filterType;
    state.frequencyHz = frequencyHz;
    state.q = q;
    state.gainDb = gainDb;

    if (!espae::allocateScratch(state.scratch, 2, synth::kBlockSize))
    {
        ESP_LOGE(kEspEqTag, "S32 scratch allocation failed");
        return false;
    }

    esp_ae_eq_filter_para_t para = espEqMakeFilterPara(filterType, frequencyHz, q, gainDb);
    esp_ae_eq_cfg_t cfg = {};
    cfg.sample_rate = synth::kSampleRate;
    cfg.channel = 2;
    cfg.bits_per_sample = ESP_AE_BIT32;
    cfg.filter_num = 1;
    cfg.para = &para;

    esp_ae_eq_handle_t handle = nullptr;
    const esp_ae_err_t err = esp_ae_eq_open(&cfg, &handle);
    if (err != ESP_AE_ERR_OK || handle == nullptr)
    {
        ESP_LOGE(kEspEqTag, "esp_ae_eq_open fehlgeschlagen: %d", static_cast<int>(err));
        espae::freeScratch(state.scratch);
        return false;
    }
    state.handle = handle;
    state.ready = true;
    return true;
}

inline void processEspEq(EspEqState &state, const float *inputL, const float *inputR,
                          float *outputL, float *outputR, size_t frames)
{
    if (state.bypass || !state.ready || state.handle == nullptr || frames > state.scratch.capacityFrames)
    {
        espEqBypass(inputL, inputR, outputL, outputR, frames);
        return;
    }

    espae::floatToS32(inputL, state.scratch.channels[0], frames);
    espae::floatToS32(inputR, state.scratch.channels[1], frames);
    esp_ae_sample_t channels[2] = {state.scratch.channels[0], state.scratch.channels[1]};
    const esp_ae_err_t err = esp_ae_eq_deintlv_process(
        static_cast<esp_ae_eq_handle_t>(state.handle), static_cast<uint32_t>(frames), channels, channels);
    if (err != ESP_AE_ERR_OK)
    {
        espEqBypass(inputL, inputR, outputL, outputR, frames);
        return;
    }
    espae::s32ToFloat(state.scratch.channels[0], outputL, frames);
    espae::s32ToFloat(state.scratch.channels[1], outputR, frames);
}

inline bool updateEspEq(EspEqState &state, EqFilterKind filterType, float frequencyHz, float q, float gainDb)
{
    state.filterType = filterType;
    state.frequencyHz = frequencyHz;
    state.q = q;
    state.gainDb = gainDb;
    if (!state.ready || state.handle == nullptr)
    {
        return false;
    }
    esp_ae_eq_filter_para_t para = espEqMakeFilterPara(filterType, frequencyHz, q, gainDb);
    return esp_ae_eq_set_filter_para(static_cast<esp_ae_eq_handle_t>(state.handle), 0, &para) == ESP_AE_ERR_OK;
}

inline void destroyEspEq(EspEqState &state)
{
    if (state.handle != nullptr)
    {
        esp_ae_eq_close(static_cast<esp_ae_eq_handle_t>(state.handle));
        state.handle = nullptr;
    }
    espae::freeScratch(state.scratch);
    state.ready = false;
}

inline void parsePresetEspEq(cJSON *p, WidgetParams &out)
{
    out.eqFilterType = eqFilterKindFromString(presetparser::getString(p, "filterType", "peak"));
    out.eqFrequency = presetparser::getFloat(p, "frequency", 1000.0f);
    out.eqQ = presetparser::getFloat(p, "q", 1.0f);
    out.eqGainDb = presetparser::getFloat(p, "gainDb", 0.0f);
    out.eqBypass = presetparser::getBool(p, "bypass", false);
}
