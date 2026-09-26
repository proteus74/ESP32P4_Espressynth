#pragma once

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "esp_ae_eq.h"
#include "esp_log.h"

#include "CvConventions.h"
#include "EspAudioEffectsAdapter.h"
#include "NodeState.h"
#include "NodeTypes.h"
#include "PresetParser.h"
#include "SynthConfig.h"

namespace
{
constexpr char kGraphicEqTag[] = "GraphicEq";
constexpr float kGraphicEqBandQ = 1.41f;

inline float graphicEqClampf(float value, float minimum, float maximum)
{
    return value < minimum ? minimum : (value > maximum ? maximum : value);
}

inline esp_ae_eq_filter_para_t graphicEqMakeFilterPara(float frequencyHz, float gainDb)
{
    esp_ae_eq_filter_para_t para = {};
    para.filter_type = ESP_AE_EQ_FILTER_PEAK;
    para.fc = static_cast<uint32_t>(graphicEqClampf(frequencyHz, 20.0f,
                                                      static_cast<float>(synth::kSampleRate / 2 - 1)));
    para.q = kGraphicEqBandQ;
    para.gain = graphicEqClampf(gainDb, -15.0f, 15.0f);
    return para;
}

inline void graphicEqBypass(const float *inputL, const float *inputR, float *outputL, float *outputR, size_t frames)
{
    if (inputL != outputL)
        memcpy(outputL, inputL, frames * sizeof(float));
    if (inputR != outputR)
        memcpy(outputR, inputR, frames * sizeof(float));
}
} // namespace

inline void destroyGraphicEq(GraphicEqState &state);

inline bool initGraphicEq(GraphicEqState &state, const float (&frequencyHz)[GraphicEqState::kBandCount],
                           const float (&gainDb)[GraphicEqState::kBandCount], float outputGainCv)
{
    destroyGraphicEq(state);
    memcpy(state.frequencyHz, frequencyHz, sizeof(state.frequencyHz));
    memcpy(state.gainDb, gainDb, sizeof(state.gainDb));
    state.outputGainCv = outputGainCv;

    if (!espae::allocateScratch(state.scratch, 2, synth::kBlockSize))
    {
        ESP_LOGE(kGraphicEqTag, "S32 scratch allocation failed");
        return false;
    }

    esp_ae_eq_filter_para_t paras[GraphicEqState::kBandCount];
    for (int i = 0; i < GraphicEqState::kBandCount; ++i)
    {
        paras[i] = graphicEqMakeFilterPara(state.frequencyHz[i], state.gainDb[i]);
    }
    esp_ae_eq_cfg_t cfg = {};
    cfg.sample_rate = synth::kSampleRate;
    cfg.channel = 2;
    cfg.bits_per_sample = ESP_AE_BIT32;
    cfg.filter_num = GraphicEqState::kBandCount;
    cfg.para = paras;

    esp_ae_eq_handle_t handle = nullptr;
    const esp_ae_err_t err = esp_ae_eq_open(&cfg, &handle);
    if (err != ESP_AE_ERR_OK || handle == nullptr)
    {
        ESP_LOGE(kGraphicEqTag, "esp_ae_eq_open fehlgeschlagen: %d", static_cast<int>(err));
        espae::freeScratch(state.scratch);
        return false;
    }
    state.handle = handle;
    state.ready = true;
    return true;
}

inline void processGraphicEq(GraphicEqState &state, const float *inputL, const float *inputR,
                              float *outputL, float *outputR, size_t frames)
{
    if (state.bypass || !state.ready || state.handle == nullptr || frames > state.scratch.capacityFrames)
    {
        graphicEqBypass(inputL, inputR, outputL, outputR, frames);
        return;
    }

    espae::floatToS32(inputL, state.scratch.channels[0], frames);
    espae::floatToS32(inputR, state.scratch.channels[1], frames);
    esp_ae_sample_t channels[2] = {state.scratch.channels[0], state.scratch.channels[1]};
    const esp_ae_err_t err = esp_ae_eq_deintlv_process(
        static_cast<esp_ae_eq_handle_t>(state.handle), static_cast<uint32_t>(frames), channels, channels);
    if (err != ESP_AE_ERR_OK)
    {
        graphicEqBypass(inputL, inputR, outputL, outputR, frames);
        return;
    }
    espae::s32ToFloat(state.scratch.channels[0], outputL, frames);
    espae::s32ToFloat(state.scratch.channels[1], outputR, frames);

    // Final output trim - raw 0-10V CV, 10V = unity (cv::vcaGain), applied
    // whether or not the EQ itself is bypassed above would be nicer, but
    // keeping it simple/consistent: only applied on the actual (non-bypass)
    // signal path, matching how esp-eq's own bypass is a full passthrough
    // with nothing else applied either.
    const float g = cv::vcaGain(state.outputGainCv, false);
    if (g != 1.0f)
    {
        for (size_t i = 0; i < frames; ++i)
        {
            outputL[i] *= g;
            outputR[i] *= g;
        }
    }
}

inline bool updateGraphicEqBandGain(GraphicEqState &state, int band, float gainDb)
{
    if (band < 0 || band >= GraphicEqState::kBandCount)
        return false;
    state.gainDb[band] = gainDb;
    if (!state.ready || state.handle == nullptr)
        return false;
    esp_ae_eq_filter_para_t para = graphicEqMakeFilterPara(state.frequencyHz[band], state.gainDb[band]);
    return esp_ae_eq_set_filter_para(static_cast<esp_ae_eq_handle_t>(state.handle), band, &para) == ESP_AE_ERR_OK;
}

inline bool updateGraphicEqBandFrequency(GraphicEqState &state, int band, float frequencyHz)
{
    if (band < 0 || band >= GraphicEqState::kBandCount)
        return false;
    state.frequencyHz[band] = frequencyHz;
    if (!state.ready || state.handle == nullptr)
        return false;
    esp_ae_eq_filter_para_t para = graphicEqMakeFilterPara(state.frequencyHz[band], state.gainDb[band]);
    return esp_ae_eq_set_filter_para(static_cast<esp_ae_eq_handle_t>(state.handle), band, &para) == ESP_AE_ERR_OK;
}

inline void destroyGraphicEq(GraphicEqState &state)
{
    if (state.handle != nullptr)
    {
        esp_ae_eq_close(static_cast<esp_ae_eq_handle_t>(state.handle));
        state.handle = nullptr;
    }
    espae::freeScratch(state.scratch);
    state.ready = false;
}

inline void parsePresetGraphicEq(cJSON *p, WidgetParams &out)
{
    static constexpr float kDefaultFreq[GraphicEqState::kBandCount] = {63.0f, 125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 14000.0f};
    float parsedFreq[GraphicEqState::kBandCount] = {};
    char key[8];
    for (int i = 0; i < GraphicEqState::kBandCount; ++i)
    {
        snprintf(key, sizeof(key), "gain%d", i + 1);
        out.geqGainDb[i] = presetparser::getFloat(p, key, 0.0f);
        snprintf(key, sizeof(key), "freq%d", i + 1);
        parsedFreq[i] = presetparser::getFloat(p, key, kDefaultFreq[i]);
    }
    bool validFrequencies = true;
    for (int i = 0; i < GraphicEqState::kBandCount; ++i)
    {
        if (!std::isfinite(parsedFreq[i]) || parsedFreq[i] != floorf(parsedFreq[i]) || parsedFreq[i] < 20.0f ||
            parsedFreq[i] > static_cast<float>(synth::kSampleRate / 2 - 1) ||
            (i > 0 && parsedFreq[i] <= parsedFreq[i - 1]))
        {
            validFrequencies = false;
            break;
        }
    }
    if (validFrequencies)
    {
        memcpy(out.geqFreqHz, parsedFreq, sizeof(out.geqFreqHz));
    }
    else
    {
        memcpy(out.geqFreqHz, kDefaultFreq, sizeof(out.geqFreqHz));
        ESP_LOGW(kGraphicEqTag, "Invalid band frequencies; using ascending defaults");
    }
    out.geqOutputGain = presetparser::getFloat(p, "gain", 10.0f);
    out.geqBypass = presetparser::getBool(p, "bypass", false);
    for (int i = 0; i < GraphicEqState::kBandCount; ++i)
    {
        char name[16];
        snprintf(name, sizeof(name), "band%dMin", i + 1);
        out.geqBandMin[i] = presetparser::getFloat(p, name, -15.0f);
        snprintf(name, sizeof(name), "band%dMax", i + 1);
        out.geqBandMax[i] = presetparser::getFloat(p, name, 15.0f);
        snprintf(name, sizeof(name), "band%dCurve", i + 1);
        out.geqBandLog[i] = strcmp(presetparser::getString(p, name, "linear"), "log") == 0;
    }
}
