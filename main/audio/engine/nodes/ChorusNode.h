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

// Stereo chorus: one LFO-modulated fractional delay line per channel,
// mirrored (offset+depth*lfo / offset-depth*lfo). Heap-allocated per
// instance (see NodeInstance.h's pointer-member comment).
namespace nodes::chorus
{

struct ChorusState
{
    // Single ring buffer: both channels were always fed the identical mono
    // input sample (see render() below), so a second full duplicate buffer
    // was pure waste - one write stream, two independently interpolated read
    // taps reproduce the exact same signal (plan section 3, P1 row: "ein
    // gemeinsamer Ring mit zwei interpolierten Lesetaps erhält das Verfahren
    // und spart einen Schreibstrom sowie etwa 17.6 KiB pro Instanz").
    DelayLine delay;
    float lfoPhase = 0.0f;
    float rateHz = 1.5f;
    float offsetSec = 0.02f;
    float depthSec = 0.003f;
    // Cached derived values (see refreshDerived() below) - recomputed only
    // when rate/offset/depth actually change (build()/setParam()), not once
    // per sample: lfoStep = rateHz/sampleRate, offset/depthSamples =
    // offset/depthSec*sampleRate (plan: "LFO-Schritt, Offset/Depth in
    // Samples vorberechnen").
    float lfoStep = 0.0f;
    float offsetSamples = 0.0f;
    float depthSamples = 0.0f;
    // UI toggle / setParam value; a wired "bypass" port overrides it (see render()).
    bool bypass = false;
};

inline void refreshDerived(ChorusState &s)
{
    constexpr float kSampleRate = static_cast<float>(synth::kSampleRate);
    s.lfoStep = s.rateHz / kSampleRate;
    s.offsetSamples = s.offsetSec * kSampleRate;
    s.depthSamples = s.depthSec * kSampleRate;
}

inline float clampChorus(float value, float lo, float hi)
{
    return !(value >= lo) ? lo : (value > hi ? hi : value);
}

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    auto &s = *inst.chorus;
    // Wired ports override the widget's settings once per block, same raw units the browser
    // preview uses (rate Hz, depth/offset seconds), clamped to the widget's slider ranges
    // (CHORUS_SLIDER_CONFIG in data/static/js/main.js).
    auto fed = [&](int port)
    { return controlPortConnected(inst, port) || (inst.audioConnectedPortMask & synth::portBit(port)) != 0; };
    bool derivedDirty = false;
    if (fed(2)) { s.rateHz = clampChorus(portSample(inst, 2, 0), 0.05f, 8.0f); derivedDirty = true; }
    if (fed(3)) { s.depthSec = clampChorus(portSample(inst, 3, 0), 0.0f, 0.015f); derivedDirty = true; }
    if (fed(4)) { s.offsetSec = clampChorus(portSample(inst, 4, 0), 0.0f, 0.04f); derivedDirty = true; }
    if (derivedDirty) refreshDerived(s);
    const bool bypass = fed(1) ? portSample(inst, 1, 0) >= cv::kGateThresholdVolts : s.bypass;
    if (bypass)
    {
        // Dry passthrough - mono "in" duplicated to both outputs, same as
        // the normal (non-bypassed) signal is always mono-sourced stereo
        // here (chorus has no separate dry/wet mix, unlike ReverbLight/Delay).
        for (size_t i = 0; i < frames; ++i)
        {
            const float x = portSample(inst, 0, i);
            inst.outL[i] = x;
            inst.outR[i] = x;
        }
        return;
    }
    for (size_t i = 0; i < frames; ++i)
    {
        const float x = portSample(inst, 0, i);
        s.lfoPhase += s.lfoStep;
        if (s.lfoPhase >= 1.0f)
        {
            s.lfoPhase -= floorf(s.lfoPhase);
        }
        const float lfo = dsp::sineLookup(s.lfoPhase);
        float dtL = s.offsetSamples + s.depthSamples * lfo;
        float dtR = s.offsetSamples - s.depthSamples * lfo;
        if (dtL < 0.0f)
            dtL = 0.0f;
        if (dtR < 0.0f)
            dtR = 0.0f;
        s.delay.write(x);
        inst.outL[i] = s.delay.readInterpolated(dtL);
        inst.outR[i] = s.delay.readInterpolated(dtR);
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p, bool psram)
{
    inst.chorus->rateHz = p.rate;
    inst.chorus->offsetSec = p.offset;
    inst.chorus->depthSec = p.depth;
    inst.chorus->bypass = p.chorusBypass;
    refreshDerived(*inst.chorus);
    // 100ms buffer covers offset (max 0.04s) + depth (max 0.015s) with
    // headroom - see CHORUS_SLIDER_CONFIG in data/static/js/main.js.
    inst.chorus->delay.init(0.1f, static_cast<float>(synth::kSampleRate), psram);
}

inline void destroy(NodeInstance &inst)
{
    if (inst.chorus != nullptr)
    {
        inst.chorus->delay.freeBuffer();
        heap_caps_free(inst.chorus);
        inst.chorus = nullptr;
    }
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr)
    {
        return false;
    }
    if (strcmp(name, "rate") == 0)
    {
        inst.chorus->rateHz = value;
        refreshDerived(*inst.chorus);
        return true;
    }
    if (strcmp(name, "depth") == 0)
    {
        inst.chorus->depthSec = value;
        refreshDerived(*inst.chorus);
        return true;
    }
    if (strcmp(name, "offset") == 0)
    {
        inst.chorus->offsetSec = value;
        refreshDerived(*inst.chorus);
        return true;
    }
    if (strcmp(name, "bypass") == 0)
    {
        inst.chorus->bypass = value >= 0.5f;
        return true;
    }
    return false;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.rate = presetparser::getFloat(p, "rate", 1.5f);
    out.offset = presetparser::getFloat(p, "offset", 0.02f);
    out.depth = presetparser::getFloat(p, "depth", 0.003f);
    out.chorusBypass = presetparser::getBool(p, "bypass", false);
}

} // namespace nodes::chorus

using nodes::chorus::ChorusState;
