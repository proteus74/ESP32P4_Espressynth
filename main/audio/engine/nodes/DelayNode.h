#pragma once

#include <cmath>
#include <cstddef>
#include <cstring>

#include "esp_heap_caps.h"

#include "../CvConventions.h"
#include "../DspPrimitives.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"

// Always-stereo feedback delay line (unifies the former delay-mono/stereo/
// echo-mono/stereo widgets - "echo" was only ever "delay with feedback", so
// feedback is simply always present now). Heap-allocated per instance (see
// NodeInstance.h's pointer-member comment).
namespace nodes::delay
{

struct DelayState
{
    DelayLine delayL;
    DelayLine delayR;
    float delayTimeMs = 300.0f; // 0..2000
    float feedback = 0.3f;      // 0..0.95
    float mix = 0.35f;          // 0..1, equal-power crossfade (see render() below)
    // "time"/"feedback"/"mix" are real, but control-only audio-rate CV
    // ports (see PortTables.h) - *Min/*Max/*Log mirror main.js's settings
    // dialog (openEffectCvSettings/EFFECT_CV_PARAMS), only used when the
    // respective port is wired via a DSP edge (see render() below). The
    // unwired case remains the already existing direct delivery of the
    // fully mapped value via setParam().
    float timeMin = 0.0f, timeMax = 2000.0f;
    bool timeLog = false;
    float feedbackMin = 0.0f, feedbackMax = 0.95f;
    bool feedbackLog = false;
    float mixMin = 0.0f, mixMax = 1.0f;
    bool mixLog = false;
    // Cached cos/sin of the current mix angle - recomputed only when mix
    // actually changes (plan section 5, Delay row: "Sinus/Cosinus des Mix
    // nur bei Änderung berechnen"), not unconditionally every block. -1.0f
    // is outside mix's valid [0,1] range, so the very first render() call
    // always computes once regardless of the initial mix value.
    float cachedMix = -1.0f;
    float dryGain = 1.0f;
    float wetGain = 0.0f;
    bool bypass = false;
};

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    const float kSampleRate = static_cast<float>(synth::kSampleRate);
    auto &s = *inst.delay;
    // "bypass" (Port-Index 2, control-only) - same wired-CV-or-manual
    // pattern as Reverb's bypass (see ReverbNode.h).
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
    // "time"/"feedback"/"mix" (Port-Index 3/4/5) are real, but control-only
    // audio-rate CV ports (see PortTables.h) - read once per block (no
    // amplitude sample jump through the coefficients themselves, as with
    // Filter/ADSR). Not wired: falls back to the already existing, directly
    // via setParam() delivered fully mapped value.
    if ((inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 3)) != 0)
        s.delayTimeMs = cv::cvToRange(portSample(inst, 3, 0), s.timeMin, s.timeMax, s.timeLog);
    if ((inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 4)) != 0)
        s.feedback = cv::cvToRange(portSample(inst, 4, 0), s.feedbackMin, s.feedbackMax, s.feedbackLog);
    if ((inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 5)) != 0)
        s.mix = cv::cvToRange(portSample(inst, 5, 0), s.mixMin, s.mixMax, s.mixLog);
    const uint32_t delaySamples = static_cast<uint32_t>(s.delayTimeMs * 0.001f * kSampleRate);
    // Equal-power crossfade (matches esp_ae_delay_para_t's documented mix
    // behavior): dry/wet gains trace a quarter sine so the perceived
    // loudness stays constant across the whole 0..1 mix range. cosf/sinf
    // are real transcendental calls (no LUT here) - only worth paying when
    // mix actually changed since the last block, not on every block
    // regardless (the common case, since mix is rarely modulated).
    if (s.mix != s.cachedMix)
    {
        s.cachedMix = s.mix;
        s.dryGain = cosf(s.mix * 1.5707963267948966f);
        s.wetGain = sinf(s.mix * 1.5707963267948966f);
    }
    const float dryGain = s.dryGain;
    const float wetGain = s.wetGain;
    for (size_t i = 0; i < frames; ++i)
    {
        const float xl = portSample(inst, 0, i);
        const float xr = portSample(inst, 1, i);
        const float dl = s.delayL.readExact(delaySamples);
        const float dr = s.delayR.readExact(delaySamples);
        s.delayL.write(dsp::flushDenorm(xl + dl * s.feedback));
        s.delayR.write(dsp::flushDenorm(xr + dr * s.feedback));
        inst.outL[i] = xl * dryGain + dl * wetGain;
        inst.outR[i] = xr * dryGain + dr * wetGain;
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p, bool psram)
{
    inst.delay->delayTimeMs = p.delayTimeMs;
    inst.delay->feedback = p.delayFeedback;
    inst.delay->mix = p.delayMix;
    inst.delay->timeMin = p.timeMin;
    inst.delay->timeMax = p.timeMax;
    inst.delay->timeLog = p.timeLog;
    inst.delay->feedbackMin = p.feedbackMin;
    inst.delay->feedbackMax = p.feedbackMax;
    inst.delay->feedbackLog = p.feedbackLog;
    inst.delay->mixMin = p.mixMin;
    inst.delay->mixMax = p.mixMax;
    inst.delay->mixLog = p.mixLog;
    inst.delay->bypass = p.delayBypass;
    // 2.2s buffer covers the 0-2000ms UI range with headroom.
    inst.delay->delayL.init(2.2f, static_cast<float>(synth::kSampleRate), psram);
    inst.delay->delayR.init(2.2f, static_cast<float>(synth::kSampleRate), psram);
}

inline void destroy(NodeInstance &inst)
{
    if (inst.delay != nullptr)
    {
        inst.delay->delayL.freeBuffer();
        inst.delay->delayR.freeBuffer();
        heap_caps_free(inst.delay);
        inst.delay = nullptr;
    }
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr)
    {
        return false;
    }
    if (strcmp(name, "delayTimeMs") == 0)
    {
        inst.delay->delayTimeMs = value;
        return true;
    }
    if (strcmp(name, "feedback") == 0)
    {
        inst.delay->feedback = value;
        return true;
    }
    if (strcmp(name, "mix") == 0)
    {
        inst.delay->mix = value;
        return true;
    }
    if (strcmp(name, "timeMin") == 0) { inst.delay->timeMin = value; return true; }
    if (strcmp(name, "timeMax") == 0) { inst.delay->timeMax = value; return true; }
    if (strcmp(name, "feedbackMin") == 0) { inst.delay->feedbackMin = value; return true; }
    if (strcmp(name, "feedbackMax") == 0) { inst.delay->feedbackMax = value; return true; }
    if (strcmp(name, "mixMin") == 0) { inst.delay->mixMin = value; return true; }
    if (strcmp(name, "mixMax") == 0) { inst.delay->mixMax = value; return true; }
    if (strcmp(name, "bypass") == 0) { inst.delay->bypass = value >= 0.5f; return true; }
    // "timeCurve"/"feedbackCurve"/"mixCurve" are strings, see Graph.cpp's
    // applyLiveParams.
    return false;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.delayTimeMs = presetparser::getFloat(p, "delayTimeMs", 300.0f);
    out.delayFeedback = presetparser::getFloat(p, "feedback", 0.3f);
    out.delayMix = presetparser::getFloat(p, "mix", 0.35f);
    out.timeMin = presetparser::getFloat(p, "timeMin", 0.0f);
    out.timeMax = presetparser::getFloat(p, "timeMax", 2000.0f);
    out.timeLog = strcmp(presetparser::getString(p, "timeCurve", "linear"), "log") == 0;
    out.feedbackMin = presetparser::getFloat(p, "feedbackMin", 0.0f);
    out.feedbackMax = presetparser::getFloat(p, "feedbackMax", 0.95f);
    out.feedbackLog = strcmp(presetparser::getString(p, "feedbackCurve", "linear"), "log") == 0;
    out.mixMin = presetparser::getFloat(p, "mixMin", 0.0f);
    out.mixMax = presetparser::getFloat(p, "mixMax", 1.0f);
    out.mixLog = strcmp(presetparser::getString(p, "mixCurve", "linear"), "log") == 0;
    out.delayBypass = presetparser::getBool(p, "bypass", false);
}

} // namespace nodes::delay

using nodes::delay::DelayState;
