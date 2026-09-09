#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "esp_heap_caps.h"

#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"

// 2-point (or value-list) mapping curve driven by the "in" DSP input (unlike
// Knob, which is MIDI-CC-driven) - same point1In/point1Out/point2In/
// point2Out/constraintMin/constraintMax/valueList shape as KnobState.
// Heap-allocated per instance (see NodeInstance.h's pointer-member comment),
// so state, render, build, destroy and live-param-set all live together here.
namespace nodes::valuemapper
{

struct ValueMapperState
{
    float point1In = 0.0f, point1Out = 0.0f;
    float point2In = 127.0f, point2Out = 10.0f;
    bool hasMin = false, hasMax = false;
    float minVal = 0.0f, maxVal = 0.0f;
    float valueList[synth::kValueListMax] = {};
    uint8_t valueListCount = 0;
    // Cached 1/(point2In - point1In) - refreshed only when the mapping
    // points themselves change (refreshDerived() below, called from
    // build()/setParam()), not once per sample. mappedValue() runs in the
    // audio-rate sample loop whenever "in" is DSP-connected (see render()),
    // so the division this replaces used to run per sample (plan section 5:
    // "im Samplepfad steht aktuell eine Division durch denselben Bereich").
    // 0.0f when point2In == point1In (degenerate mapping, same as before).
    float invRange = 0.0f;

    float mappedValue(float input) const
    {
        if (valueListCount >= 2)
        {
            int idx = 0;
            if (point2In != point1In)
            {
                float frac = (input - point1In) * invRange;
                if (frac < 0.0f)
                    frac = 0.0f;
                if (frac > 1.0f)
                    frac = 1.0f;
                idx = static_cast<int>(frac * static_cast<float>(valueListCount));
            }
            if (idx >= static_cast<int>(valueListCount))
                idx = static_cast<int>(valueListCount) - 1;
            return valueList[idx];
        }

        float value = point2In == point1In
                          ? point1Out
                          : point1Out + (input - point1In) * (point2Out - point1Out) * invRange;
        if (hasMin && value < minVal)
            value = minVal;
        if (hasMax && value > maxVal)
            value = maxVal;
        return value;
    }
};

inline void refreshDerived(ValueMapperState &s)
{
    s.invRange = s.point2In != s.point1In ? 1.0f / (s.point2In - s.point1In) : 0.0f;
}

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    auto &s = *inst.valueMapper;
    // audioPortConstantForBlock() (NodeInstance.h) covers every case where
    // "in" resolves to one repeated value this block, including correctly
    // recognizing a MIDI-glide ramp as NOT constant (see its comment / plan
    // section 6, finding 1).
    float inValue = 0.0f;
    if (audioPortConstantForBlock(inst, 0, frames, inValue))
    {
        const float result = s.mappedValue(inValue);
        for (size_t i = 0; i < frames; ++i)
        {
            inst.outL[i] = result;
        }
        return;
    }
    for (size_t i = 0; i < frames; ++i)
    {
        inst.outL[i] = s.mappedValue(portSample(inst, 0, i));
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    // Reuses the same point1In/point1Out/point2In/point2Out/constraintMin/
    // constraintMax/valueList WidgetParams fields as Knob - each widget
    // instance parses its own independent params object, so there's no
    // cross-widget collision (same pattern Knob/Button already share for
    // valueList).
    inst.valueMapper->point1In = p.point1In;
    inst.valueMapper->point1Out = p.point1Out;
    inst.valueMapper->point2In = p.point2In;
    inst.valueMapper->point2Out = p.point2Out;
    inst.valueMapper->hasMin = p.hasConstraintMin;
    inst.valueMapper->minVal = p.constraintMin;
    inst.valueMapper->hasMax = p.hasConstraintMax;
    inst.valueMapper->maxVal = p.constraintMax;
    inst.valueMapper->valueListCount = p.valueListCount;
    for (uint8_t i = 0; i < p.valueListCount && i < synth::kValueListMax; ++i)
    {
        inst.valueMapper->valueList[i] = p.valueList[i];
    }
    refreshDerived(*inst.valueMapper);
}

inline void destroy(NodeInstance &inst)
{
    if (inst.valueMapper != nullptr)
    {
        heap_caps_free(inst.valueMapper);
        inst.valueMapper = nullptr;
    }
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    // Same field names as Knob, minus "raw" (no MIDI-CC dial - driven by
    // the "in" DSP input instead, computed fresh every block in render()
    // above, no cached currentValue to refresh here).
    if (name != nullptr && strcmp(name, "point1In") == 0)
    {
        inst.valueMapper->point1In = value;
        refreshDerived(*inst.valueMapper);
        return true;
    }
    if (name != nullptr && strcmp(name, "point1Out") == 0)
    {
        inst.valueMapper->point1Out = value;
        return true;
    }
    if (name != nullptr && strcmp(name, "point2In") == 0)
    {
        inst.valueMapper->point2In = value;
        refreshDerived(*inst.valueMapper);
        return true;
    }
    if (name != nullptr && strcmp(name, "point2Out") == 0)
    {
        inst.valueMapper->point2Out = value;
        return true;
    }
    return false;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    // Same field names as Knob above (point1In/point1Out/point2In/
    // point2Out/constraintMin/constraintMax/valueList), minus cc/channel/
    // raw - Value Mapper has no MIDI-CC dial, just an "in" DSP input.
    out.point1In = presetparser::getFloat(p, "point1In", 0.0f);
    out.point1Out = presetparser::getFloat(p, "point1Out", 0.0f);
    out.point2In = presetparser::getFloat(p, "point2In", 127.0f);
    out.point2Out = presetparser::getFloat(p, "point2Out", 10.0f);
    cJSON *cMin = p != nullptr ? cJSON_GetObjectItemCaseSensitive(p, "constraintMin") : nullptr;
    cJSON *cMax = p != nullptr ? cJSON_GetObjectItemCaseSensitive(p, "constraintMax") : nullptr;
    out.hasConstraintMin = cJSON_IsNumber(cMin);
    out.constraintMin = out.hasConstraintMin ? static_cast<float>(cMin->valuedouble) : 0.0f;
    out.hasConstraintMax = cJSON_IsNumber(cMax);
    out.constraintMax = out.hasConstraintMax ? static_cast<float>(cMax->valuedouble) : 0.0f;
    out.valueListCount = presetparser::getFloatArray(p, "valueList", out.valueList, static_cast<uint8_t>(synth::kValueListMax));
}

} // namespace nodes::valuemapper
