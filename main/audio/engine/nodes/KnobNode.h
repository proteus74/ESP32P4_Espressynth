#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "esp_heap_caps.h"

#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"

// CC-learn + 2-point mapping curve, ported from main.js's knobStates entries
// (cc/point1In/point1Out/point2In/point2Out/constraintMin/constraintMax). Not
// DSP state - consumed by Graph::handleMidiEvent's CC branch to map an
// incoming Control Change value onto whatever this knob's ControlRoutes
// target. Heap-allocated per instance (see NodeInstance.h's pointer-member
// comment), so state, render, build, destroy and live-param-set all live
// together here.
namespace nodes::knob
{

struct KnobState
{
    int16_t cc = -1;      // -1 = unlearned (JS: cc === null)
    int8_t channel = -1;  // -1 = unlearned; 0-based MIDI channel (JS: channel === null)
    // Some controllers reuse the same CC number across several physical
    // knobs/faders, distinguishing them only by MIDI channel - so a knob's
    // identity for CC-matching (see Graph::handleMidiEvent) is the (cc,
    // channel) pair together, not cc alone.
    float point1In = 0.0f, point1Out = 0.0f;
    float point2In = 127.0f, point2Out = 10.0f; // Voltage output: 0-127 (raw) -> 0-10V (see CvConventions.h)
    bool hasMin = false, hasMax = false;
    float minVal = 0.0f, maxVal = 0.0f;
    float raw = 0.0f;
    float currentValue = 0.0f;

    // Value list (main.js: knobStates' listText, already parsed here): as
    // soon as it has >=2 entries it automatically takes precedence over the
    // linear mapping (main.js: parseValueList()/mapKnobValue() - no separate
    // mode switch, the list "wins" by itself). raw (0-127) then directly
    // selects a list index (no state, unlike the press-edge cycle in
    // ButtonState).
    float valueList[synth::kValueListMax] = {};
    uint8_t valueListCount = 0;

    float mappedValue() const
    {
        if (valueListCount >= 2)
        {
            int idx = static_cast<int>(raw * static_cast<float>(valueListCount) / 128.0f);
            if (idx < 0)
                idx = 0;
            if (idx >= static_cast<int>(valueListCount))
                idx = static_cast<int>(valueListCount) - 1;
            return valueList[idx];
        }

        float value = point2In == point1In
                          ? point1Out
                          : point1Out + (raw - point1In) * (point2Out - point1Out) / (point2In - point1In);
        if (hasMin && value < minVal)
            value = minVal;
        if (hasMax && value > maxVal)
            value = maxVal;
        return value;
    }
};

inline void render(NodeInstance &inst, size_t frames)
{
    for (size_t i = 0; i < frames; ++i)
    {
        inst.outL[i] = inst.knob->currentValue;
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    inst.knob->cc = p.knobCc;
    inst.knob->channel = p.knobChannel;
    inst.knob->point1In = p.point1In;
    inst.knob->point1Out = p.point1Out;
    inst.knob->point2In = p.point2In;
    inst.knob->point2Out = p.point2Out;
    inst.knob->hasMin = p.hasConstraintMin;
    inst.knob->minVal = p.constraintMin;
    inst.knob->hasMax = p.hasConstraintMax;
    inst.knob->maxVal = p.constraintMax;
    inst.knob->raw = p.knobRaw;
    inst.knob->valueListCount = p.valueListCount;
    for (uint8_t i = 0; i < p.valueListCount && i < synth::kValueListMax; ++i)
    {
        inst.knob->valueList[i] = p.valueList[i];
    }
    inst.knob->currentValue = inst.knob->mappedValue();
}

inline void destroy(NodeInstance &inst)
{
    if (inst.knob != nullptr)
    {
        heap_caps_free(inst.knob);
        inst.knob = nullptr;
    }
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr)
    {
        return false;
    }
    if (strcmp(name, "raw") == 0)
    {
        inst.knob->raw = value;
        inst.knob->currentValue = inst.knob->mappedValue();
        return true;
    }
    if (strcmp(name, "point1In") == 0)
    {
        inst.knob->point1In = value;
        inst.knob->currentValue = inst.knob->mappedValue();
        return true;
    }
    if (strcmp(name, "point1Out") == 0)
    {
        inst.knob->point1Out = value;
        inst.knob->currentValue = inst.knob->mappedValue();
        return true;
    }
    if (strcmp(name, "point2In") == 0)
    {
        inst.knob->point2In = value;
        inst.knob->currentValue = inst.knob->mappedValue();
        return true;
    }
    if (strcmp(name, "point2Out") == 0)
    {
        inst.knob->point2Out = value;
        inst.knob->currentValue = inst.knob->mappedValue();
        return true;
    }
    return false;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    cJSON *cc = p != nullptr ? cJSON_GetObjectItemCaseSensitive(p, "cc") : nullptr;
    out.knobCc = cJSON_IsNumber(cc) ? static_cast<int16_t>(cc->valueint) : -1;
    cJSON *ch = p != nullptr ? cJSON_GetObjectItemCaseSensitive(p, "channel") : nullptr;
    out.knobChannel = cJSON_IsNumber(ch) ? static_cast<int8_t>(ch->valueint) : -1;
    out.knobRaw = presetparser::getFloat(p, "raw", 0.0f);
    out.point1In = presetparser::getFloat(p, "point1In", 0.0f);
    out.point1Out = presetparser::getFloat(p, "point1Out", 0.0f);
    out.point2In = presetparser::getFloat(p, "point2In", 127.0f);
    out.point2Out = presetparser::getFloat(p, "point2Out", 10.0f); // CV convention: voltage output defaults to 0-10V
    cJSON *cMin = p != nullptr ? cJSON_GetObjectItemCaseSensitive(p, "constraintMin") : nullptr;
    cJSON *cMax = p != nullptr ? cJSON_GetObjectItemCaseSensitive(p, "constraintMax") : nullptr;
    out.hasConstraintMin = cJSON_IsNumber(cMin);
    out.constraintMin = out.hasConstraintMin ? static_cast<float>(cMin->valuedouble) : 0.0f;
    out.hasConstraintMax = cJSON_IsNumber(cMax);
    out.constraintMax = out.hasConstraintMax ? static_cast<float>(cMax->valuedouble) : 0.0f;
    out.valueListCount = presetparser::getFloatArray(p, "valueList", out.valueList, static_cast<uint8_t>(synth::kValueListMax));
}

} // namespace nodes::knob

using nodes::knob::KnobState;
