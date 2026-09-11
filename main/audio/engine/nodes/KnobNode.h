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

struct KnobControl
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

struct KnobState
{
    uint8_t count = 1;
    KnobControl controls[synth::kKnobControlMax];
};

inline void render(NodeInstance &inst, size_t frames)
{
    for (size_t i = 0; i < frames; ++i)
    {
        inst.outL[i] = inst.knob->controls[0].currentValue;
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    inst.knob->count = p.knobCount < 1 ? 1 : (p.knobCount > synth::kKnobControlMax ? synth::kKnobControlMax : p.knobCount);
    for (uint8_t controlIndex = 0; controlIndex < inst.knob->count; ++controlIndex)
    {
        KnobControl &control = inst.knob->controls[controlIndex];
        const WidgetParams::KnobControlParams &source = p.knobs[controlIndex];
        control.cc = source.cc;
        control.channel = source.channel;
        control.point1In = source.point1In;
        control.point1Out = source.point1Out;
        control.point2In = source.point2In;
        control.point2Out = source.point2Out;
        control.hasMin = source.hasConstraintMin;
        control.minVal = source.constraintMin;
        control.hasMax = source.hasConstraintMax;
        control.maxVal = source.constraintMax;
        control.raw = source.raw;
        control.valueListCount = source.valueListCount;
        for (uint8_t i = 0; i < source.valueListCount && i < synth::kValueListMax; ++i)
        {
            control.valueList[i] = source.valueList[i];
        }
        control.currentValue = control.mappedValue();
    }
}

inline void destroy(NodeInstance &inst)
{
    if (inst.knob != nullptr)
    {
        heap_caps_free(inst.knob);
        inst.knob = nullptr;
    }
}

inline bool setParam(NodeInstance &inst, const char *name, float value, uint8_t controlIndex = 0)
{
    if (name == nullptr || controlIndex >= inst.knob->count)
    {
        return false;
    }
    if (strcmp(name, "raw") == 0)
    {
        KnobControl &control = inst.knob->controls[controlIndex];
        control.raw = value;
        control.currentValue = control.mappedValue();
        return true;
    }
    if (strcmp(name, "point1In") == 0)
    {
        KnobControl &control = inst.knob->controls[controlIndex];
        control.point1In = value;
        control.currentValue = control.mappedValue();
        return true;
    }
    if (strcmp(name, "point1Out") == 0)
    {
        KnobControl &control = inst.knob->controls[controlIndex];
        control.point1Out = value;
        control.currentValue = control.mappedValue();
        return true;
    }
    if (strcmp(name, "point2In") == 0)
    {
        KnobControl &control = inst.knob->controls[controlIndex];
        control.point2In = value;
        control.currentValue = control.mappedValue();
        return true;
    }
    if (strcmp(name, "point2Out") == 0)
    {
        KnobControl &control = inst.knob->controls[controlIndex];
        control.point2Out = value;
        control.currentValue = control.mappedValue();
        return true;
    }
    return false;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    const int count = presetparser::getInt(p, "knobCount", 1);
    out.knobCount = static_cast<uint8_t>(count < 1 ? 1 : (count > static_cast<int>(synth::kKnobControlMax) ? synth::kKnobControlMax : count));
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

    // Old presets carry one flat control. Seed control one from those fields
    // before optionally replacing all controls with the new array schema.
    WidgetParams::KnobControlParams &legacy = out.knobs[0];
    legacy.cc = out.knobCc;
    legacy.channel = out.knobChannel;
    legacy.raw = out.knobRaw;
    legacy.point1In = out.point1In;
    legacy.point1Out = out.point1Out;
    legacy.point2In = out.point2In;
    legacy.point2Out = out.point2Out;
    legacy.hasConstraintMin = out.hasConstraintMin;
    legacy.constraintMin = out.constraintMin;
    legacy.hasConstraintMax = out.hasConstraintMax;
    legacy.constraintMax = out.constraintMax;
    legacy.valueListCount = out.valueListCount;
    for (uint8_t i = 0; i < out.valueListCount; ++i) legacy.valueList[i] = out.valueList[i];

    cJSON *controls = p != nullptr ? cJSON_GetObjectItemCaseSensitive(p, "knobs") : nullptr;
    if (cJSON_IsArray(controls))
    {
        const int arrayCount = cJSON_GetArraySize(controls);
        out.knobCount = static_cast<uint8_t>(arrayCount < 1 ? 1 : (arrayCount > static_cast<int>(synth::kKnobControlMax) ? synth::kKnobControlMax : arrayCount));
        for (uint8_t controlIndex = 0; controlIndex < out.knobCount; ++controlIndex)
        {
            cJSON *item = cJSON_GetArrayItem(controls, controlIndex);
            if (!cJSON_IsObject(item)) continue;
            WidgetParams::KnobControlParams &control = out.knobs[controlIndex];
            cJSON *itemCc = cJSON_GetObjectItemCaseSensitive(item, "cc");
            cJSON *itemChannel = cJSON_GetObjectItemCaseSensitive(item, "channel");
            cJSON *itemMin = cJSON_GetObjectItemCaseSensitive(item, "constraintMin");
            cJSON *itemMax = cJSON_GetObjectItemCaseSensitive(item, "constraintMax");
            control.cc = cJSON_IsNumber(itemCc) ? static_cast<int16_t>(itemCc->valueint) : -1;
            control.channel = cJSON_IsNumber(itemChannel) ? static_cast<int8_t>(itemChannel->valueint) : -1;
            control.raw = presetparser::getFloat(item, "raw", 0.0f);
            control.point1In = presetparser::getFloat(item, "point1In", 0.0f);
            control.point1Out = presetparser::getFloat(item, "point1Out", 0.0f);
            control.point2In = presetparser::getFloat(item, "point2In", 127.0f);
            control.point2Out = presetparser::getFloat(item, "point2Out", 10.0f);
            control.hasConstraintMin = cJSON_IsNumber(itemMin);
            control.constraintMin = control.hasConstraintMin ? static_cast<float>(itemMin->valuedouble) : 0.0f;
            control.hasConstraintMax = cJSON_IsNumber(itemMax);
            control.constraintMax = control.hasConstraintMax ? static_cast<float>(itemMax->valuedouble) : 0.0f;
            control.valueListCount = presetparser::getFloatArray(item, "valueList", control.valueList, static_cast<uint8_t>(synth::kValueListMax));
        }
    }
}

} // namespace nodes::knob

using nodes::knob::KnobState;
using nodes::knob::KnobControl;
