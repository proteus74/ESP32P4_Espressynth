#pragma once

#include <cmath>
#include <cstddef>
#include <cstring>

#include "esp_heap_caps.h"

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
    DelayLine delayL;
    DelayLine delayR;
    float lfoPhase = 0.0f;
    float rateHz = 1.5f;
    float offsetSec = 0.02f;
    float depthSec = 0.003f;
};

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    const float kSampleRate = static_cast<float>(synth::kSampleRate);
    auto &s = *inst.chorus;
    for (size_t i = 0; i < frames; ++i)
    {
        const float x = portSample(inst, 0, i);
        s.lfoPhase += s.rateHz / kSampleRate;
        if (s.lfoPhase >= 1.0f)
        {
            s.lfoPhase -= floorf(s.lfoPhase);
        }
        const float lfo = dsp::sineLookup(s.lfoPhase);
        float dtL = (s.offsetSec + s.depthSec * lfo) * kSampleRate;
        float dtR = (s.offsetSec - s.depthSec * lfo) * kSampleRate;
        if (dtL < 0.0f)
            dtL = 0.0f;
        if (dtR < 0.0f)
            dtR = 0.0f;
        s.delayL.write(x);
        s.delayR.write(x);
        inst.outL[i] = s.delayL.readInterpolated(dtL);
        inst.outR[i] = s.delayR.readInterpolated(dtR);
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p, bool psram)
{
    inst.chorus->rateHz = p.rate;
    inst.chorus->offsetSec = p.offset;
    inst.chorus->depthSec = p.depth;
    // 100ms buffer covers offset (max 0.04s) + depth (max 0.015s) with
    // headroom - see CHORUS_SLIDER_CONFIG in data/static/js/main.js.
    inst.chorus->delayL.init(0.1f, static_cast<float>(synth::kSampleRate), psram);
    inst.chorus->delayR.init(0.1f, static_cast<float>(synth::kSampleRate), psram);
}

inline void destroy(NodeInstance &inst)
{
    if (inst.chorus != nullptr)
    {
        inst.chorus->delayL.freeBuffer();
        inst.chorus->delayR.freeBuffer();
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
        return true;
    }
    if (strcmp(name, "depth") == 0)
    {
        inst.chorus->depthSec = value;
        return true;
    }
    if (strcmp(name, "offset") == 0)
    {
        inst.chorus->offsetSec = value;
        return true;
    }
    return false;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.rate = presetparser::getFloat(p, "rate", 1.5f);
    out.offset = presetparser::getFloat(p, "offset", 0.02f);
    out.depth = presetparser::getFloat(p, "depth", 0.003f);
}

} // namespace nodes::chorus

using nodes::chorus::ChorusState;
