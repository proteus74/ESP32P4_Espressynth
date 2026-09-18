#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "esp_heap_caps.h"

#include "../CvConventions.h"
#include "../DspPrimitives.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"

// CPU-cheap mono algorithmic reverb (Freeverb-style: 4 combs + 2 allpasses,
// one-pole damping lowpass, no pre-delay). Heap-allocated per instance (see
// NodeInstance.h's pointer-member comment).
namespace nodes::reverb
{

// ONE shared mono wet path (instead of separate L/R tanks) with few
// combs/allpasses and no pre-delay - ~12 DelayLine accesses/sample. Dry
// stays stereo (see render() below). See optimization plan
// .claude/plans/schaue-die-die-ganze-zany-moore.md for the derivation.
struct ReverbLightState
{
    static constexpr int kCombs = 4;
    static constexpr int kAllpasses = 2;

    DelayLine combs[kCombs];
    DelayLine allpasses[kAllpasses];
    float combGain[kCombs] = {};
    OnePoleLowpass dampingFilter;
    float roomSize = 0.5f;    // 0..1
    float damping = 0.5f;     // 0..1
    float wetLevelDb = -6.0f; // -96..0 dB
    float dryLevelDb = 0.0f;  // -96..0 dB
    float wetGain = 0.5011872f;
    float dryGain = 1.0f;
    bool bypass = false;
    // *Min/*Max/*Log mirror main.js's settings dialog (openEffectCvSettings/
    // EFFECT_CV_PARAMS) - see parsePresetReverbLight/buildReverbLight.
    float roomSizeMin = 0.0f, roomSizeMax = 1.0f;
    bool roomSizeLog = false;
    float dampingMin = 0.0f, dampingMax = 1.0f;
    bool dampingLog = false;
    float wetLevelMin = -96.0f, wetLevelMax = 0.0f;
    bool wetLevelLog = false;
    float dryLevelMin = -96.0f, dryLevelMax = 0.0f;
    bool dryLevelLog = false;
};

inline float reverbDbToGain(float db)
{
    return powf(10.0f, db / 20.0f);
}

inline void refreshMixGains(ReverbLightState &s)
{
    s.wetGain = reverbDbToGain(s.wetLevelDb);
    s.dryGain = reverbDbToGain(s.dryLevelDb);
}

// Re-derives the comb feedback gains (from roomSize, one powf() per comb) -
// same formula as buildReverbLight() below, re-applied to the already-
// allocated (fixed-length) comb buffers, no reinit. Split from the
// damping-cutoff derivation below (plan section 5, Reverb row: "beide
// Ableitungen entkoppeln") so a damping-only change doesn't pay for redundant
// powf() calls, and vice versa.
inline void recomputeReverbLightCombGains(ReverbLightState &s)
{
    const float sizeSec = 0.1f + s.roomSize * 7.9f;
    for (int c = 0; c < ReverbLightState::kCombs; ++c)
    {
        const uint32_t len = s.combs[c].capacitySamples();
        if (len == 0)
        {
            continue;
        }
        float g = powf(10.0f, -3.0f * static_cast<float>(len) / (static_cast<float>(synth::kSampleRate) * sizeSec));
        if (g > 0.98f)
            g = 0.98f;
        if (g < 0.0f)
            g = 0.0f;
        s.combGain[c] = g;
    }
}

inline void recomputeReverbLightDamping(ReverbLightState &s)
{
    const float dampCutoffHz = 20000.0f - s.damping * 19800.0f;
    s.dampingFilter.setCutoff(dampCutoffHz, static_cast<float>(synth::kSampleRate));
}

inline void recomputeReverbLightDerived(ReverbLightState &s)
{
    recomputeReverbLightCombGains(s);
    recomputeReverbLightDamping(s);
}

inline void renderReverbLight(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    // CPU-cheap variant: ONE shared mono wet path instead of separate L/R
    // tanks, no pre-delay - see ReverbLightState/optimization plan
    // .claude/plans/schaue-die-die-ganze-zany-moore.md. Dry stays stereo
    // (xl/xr unchanged), only the reverb tail is mixed mono.
    auto &s = *inst.reverbLight;
    // "roomSize"/"damping"/"wetLevel"/"dryLevel" (Port-Index 3-6) are real,
    // but control-only audio-rate CV ports (see PortTables.h) - read once
    // per block, same pattern as Delay. Only recomputed when the resolved
    // value actually changed, not merely because the CV port is connected.
    if ((inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 3)) != 0)
    {
        const float roomSize = cv::cvToRange(portSample(inst, 3, 0), s.roomSizeMin, s.roomSizeMax, s.roomSizeLog);
        if (roomSize != s.roomSize)
        {
            s.roomSize = roomSize;
            recomputeReverbLightCombGains(s);
        }
    }
    if ((inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 4)) != 0)
    {
        const float damping = cv::cvToRange(portSample(inst, 4, 0), s.dampingMin, s.dampingMax, s.dampingLog);
        if (damping != s.damping)
        {
            s.damping = damping;
            recomputeReverbLightDamping(s);
        }
    }
    bool mixGainsDirty = false;
    if ((inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 5)) != 0)
    {
        const float wetLevelDb = cv::cvToRange(portSample(inst, 5, 0), s.wetLevelMin, s.wetLevelMax, s.wetLevelLog);
        mixGainsDirty |= wetLevelDb != s.wetLevelDb;
        s.wetLevelDb = wetLevelDb;
    }
    if ((inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 6)) != 0)
    {
        const float dryLevelDb = cv::cvToRange(portSample(inst, 6, 0), s.dryLevelMin, s.dryLevelMax, s.dryLevelLog);
        mixGainsDirty |= dryLevelDb != s.dryLevelDb;
        s.dryLevelDb = dryLevelDb;
    }
    if (mixGainsDirty)
    {
        refreshMixGains(s);
    }
    constexpr float kAllpassG = 0.5f;
    const bool bypass = (inst.controlConnectedPortMask & (1u << 2)) != 0
                            ? inst.controlValue[2] >= cv::kGateThresholdVolts
                            : s.bypass;
    if (bypass)
    {
        for (size_t i = 0; i < frames; ++i)
        {
            inst.outL[i] = portSample(inst, 0, i);
            inst.outR[i] = portSample(inst, 1, i);
        }
        return;
    }
    for (size_t i = 0; i < frames; ++i)
    {
        const float xl = portSample(inst, 0, i);
        const float xr = portSample(inst, 1, i);

        const float mono = (xl + xr) * 0.5f;
        float combSum = 0.0f;
        for (int c = 0; c < ReverbLightState::kCombs; ++c)
        {
            const float delayed = s.combs[c].readOldest();
            s.combs[c].write(dsp::flushDenorm(mono + delayed * s.combGain[c]));
            combSum += delayed;
        }
        combSum *= (1.0f / static_cast<float>(ReverbLightState::kCombs));

        float ap = combSum;
        for (int a = 0; a < ReverbLightState::kAllpasses; ++a)
        {
            const float buf = s.allpasses[a].readOldest();
            const float apOut = -kAllpassG * ap + buf;
            s.allpasses[a].write(dsp::flushDenorm(ap + kAllpassG * buf));
            ap = apOut;
        }

        const float wet = s.dampingFilter.process(ap);
        inst.outL[i] = xl * s.dryGain + wet * s.wetGain;
        inst.outR[i] = xr * s.dryGain + wet * s.wetGain;
    }
}

// CPU-cheap mono reverb - deliberately internal-RAM-preferred instead of
// blanket PSRAM like Chorus/Delay, with a PSRAM fallback only if the
// internal heap is currently too tight.
inline void buildReverbLight(NodeInstance &inst, const WidgetParams &p, uint16_t &psramFallbacks)
{
    static constexpr uint32_t kCombLen44k[ReverbLightState::kCombs] = {1116, 1188, 1277, 1356};
    static constexpr uint32_t kAllpassLen44k[ReverbLightState::kAllpasses] = {556, 441};
    const float scaleF = static_cast<float>(synth::kSampleRate) / 44100.0f;

    inst.reverbLight->roomSize = p.reverbRoomSize;
    inst.reverbLight->damping = p.reverbDamping;
    inst.reverbLight->wetLevelDb = p.reverbWetLevelDb;
    inst.reverbLight->dryLevelDb = p.reverbDryLevelDb;
    inst.reverbLight->bypass = p.reverbBypass;
    inst.reverbLight->roomSizeMin = p.roomSizeMin;
    inst.reverbLight->roomSizeMax = p.roomSizeMax;
    inst.reverbLight->roomSizeLog = p.roomSizeLog;
    inst.reverbLight->dampingMin = p.dampingMin;
    inst.reverbLight->dampingMax = p.dampingMax;
    inst.reverbLight->dampingLog = p.dampingLog;
    inst.reverbLight->wetLevelMin = p.wetLevelMin;
    inst.reverbLight->wetLevelMax = p.wetLevelMax;
    inst.reverbLight->wetLevelLog = p.wetLevelLog;
    inst.reverbLight->dryLevelMin = p.dryLevelMin;
    inst.reverbLight->dryLevelMax = p.dryLevelMax;
    inst.reverbLight->dryLevelLog = p.dryLevelLog;
    refreshMixGains(*inst.reverbLight);

    const float sizeSec = 0.1f + p.reverbRoomSize * 7.9f;
    for (int c = 0; c < ReverbLightState::kCombs; ++c)
    {
        const uint32_t len = static_cast<uint32_t>(static_cast<float>(kCombLen44k[c]) * scaleF + 0.5f);
        if (!inst.reverbLight->combs[c].initSamples(len, false))
        {
            inst.reverbLight->combs[c].initSamples(len, true);
            // Counted in the same outputPsramFallbacks total GraphBuilder.h logs once per
            // preset build - otherwise this specific fallback (unlike every other buffer/
            // state allocation in the engine) was invisible: it silently trades ReverbLight's
            // whole reason for existing (avoiding Reverb's ~52 PSRAM accesses/sample) for the
            // same PSRAM latency on the hot render path, with no way to tell after the fact.
            ++psramFallbacks;
        }
        float g = powf(10.0f, -3.0f * static_cast<float>(len) / (static_cast<float>(synth::kSampleRate) * sizeSec));
        if (g > 0.98f)
            g = 0.98f;
        if (g < 0.0f)
            g = 0.0f;
        inst.reverbLight->combGain[c] = g;
    }
    for (int a = 0; a < ReverbLightState::kAllpasses; ++a)
    {
        const uint32_t len = static_cast<uint32_t>(static_cast<float>(kAllpassLen44k[a]) * scaleF + 0.5f);
        if (!inst.reverbLight->allpasses[a].initSamples(len, false))
        {
            inst.reverbLight->allpasses[a].initSamples(len, true);
            ++psramFallbacks;
        }
    }

    const float dampCutoffHz = 20000.0f - p.reverbDamping * 19800.0f;
    inst.reverbLight->dampingFilter.setCutoff(dampCutoffHz, static_cast<float>(synth::kSampleRate));
}

inline void destroyReverbLight(NodeInstance &inst)
{
    if (inst.reverbLight != nullptr)
    {
        for (auto &comb : inst.reverbLight->combs)
        {
            comb.freeBuffer();
        }
        for (auto &allpass : inst.reverbLight->allpasses)
        {
            allpass.freeBuffer();
        }
        heap_caps_free(inst.reverbLight);
        inst.reverbLight = nullptr;
    }
}

inline bool setParamReverbLight(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr)
    {
        return false;
    }
    if (strcmp(name, "bypass") == 0)
    {
        inst.reverbLight->bypass = value >= 0.5f;
        return true;
    }
    if (strcmp(name, "roomSize") == 0)
    {
        inst.reverbLight->roomSize = value;
        recomputeReverbLightDerived(*inst.reverbLight);
        return true;
    }
    if (strcmp(name, "damping") == 0)
    {
        inst.reverbLight->damping = value;
        recomputeReverbLightDerived(*inst.reverbLight);
        return true;
    }
    if (strcmp(name, "wetLevelDb") == 0)
    {
        inst.reverbLight->wetLevelDb = value;
        inst.reverbLight->wetGain = reverbDbToGain(value);
        return true;
    }
    if (strcmp(name, "dryLevelDb") == 0)
    {
        inst.reverbLight->dryLevelDb = value;
        inst.reverbLight->dryGain = reverbDbToGain(value);
        return true;
    }
    if (strcmp(name, "roomSizeMin") == 0) { inst.reverbLight->roomSizeMin = value; return true; }
    if (strcmp(name, "roomSizeMax") == 0) { inst.reverbLight->roomSizeMax = value; return true; }
    if (strcmp(name, "dampingMin") == 0) { inst.reverbLight->dampingMin = value; return true; }
    if (strcmp(name, "dampingMax") == 0) { inst.reverbLight->dampingMax = value; return true; }
    if (strcmp(name, "wetLevelMin") == 0) { inst.reverbLight->wetLevelMin = value; return true; }
    if (strcmp(name, "wetLevelMax") == 0) { inst.reverbLight->wetLevelMax = value; return true; }
    if (strcmp(name, "dryLevelMin") == 0) { inst.reverbLight->dryLevelMin = value; return true; }
    if (strcmp(name, "dryLevelMax") == 0) { inst.reverbLight->dryLevelMax = value; return true; }
    // "roomSizeCurve"/"dampingCurve"/"wetLevelCurve"/"dryLevelCurve" are
    // strings, see Graph.cpp's applyLiveParams.
    return false;
}

inline void parsePresetReverbLight(cJSON *p, WidgetParams &out)
{
    out.reverbRoomSize = presetparser::getFloat(p, "roomSize", 0.5f);
    out.reverbDamping = presetparser::getFloat(p, "damping", 0.5f);
    out.reverbWetLevelDb = presetparser::getFloat(p, "wetLevelDb", -6.0f);
    out.reverbDryLevelDb = presetparser::getFloat(p, "dryLevelDb", 0.0f);
    out.reverbBypass = presetparser::getBool(p, "bypass", false);
    out.roomSizeMin = presetparser::getFloat(p, "roomSizeMin", 0.0f);
    out.roomSizeMax = presetparser::getFloat(p, "roomSizeMax", 1.0f);
    out.roomSizeLog = strcmp(presetparser::getString(p, "roomSizeCurve", "linear"), "log") == 0;
    out.dampingMin = presetparser::getFloat(p, "dampingMin", 0.0f);
    out.dampingMax = presetparser::getFloat(p, "dampingMax", 1.0f);
    out.dampingLog = strcmp(presetparser::getString(p, "dampingCurve", "linear"), "log") == 0;
    out.wetLevelMin = presetparser::getFloat(p, "wetLevelMin", -96.0f);
    out.wetLevelMax = presetparser::getFloat(p, "wetLevelMax", 0.0f);
    out.wetLevelLog = strcmp(presetparser::getString(p, "wetLevelCurve", "linear"), "log") == 0;
    out.dryLevelMin = presetparser::getFloat(p, "dryLevelMin", -96.0f);
    out.dryLevelMax = presetparser::getFloat(p, "dryLevelMax", 0.0f);
    out.dryLevelLog = strcmp(presetparser::getString(p, "dryLevelCurve", "linear"), "log") == 0;
}

} // namespace nodes::reverb

using nodes::reverb::ReverbLightState;
