#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"

#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"
#include "../LayerControls.h"

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
    char label[48] = {};
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
    uint32_t midiFeedback = UINT32_MAX;

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

        // Linear from A Out (raw 0, knob fully left) to B Out (raw 127). The former A In/B In
        // and Min/Max settings were removed from the UI; values still stored in old presets
        // are ignored so device and browser always agree (main.js mapKnobValue()).
        return point1Out + raw * (1.0f / 127.0f) * (point2Out - point1Out);
    }
};

struct KnobState
{
    uint8_t count = 1;
    uint8_t layerCount = 1;
    uint8_t activeLayer = 0;
    uint8_t feedbackLayer = UINT8_MAX;
    LayerControls<KnobControl> controls;

    bool resizeControls(uint8_t newCount, uint8_t newLayers)
    {
        if (!controls.resize(newCount, newLayers)) return false;
        count = newCount;
        layerCount = newLayers;
        if (activeLayer >= layerCount) activeLayer = layerCount - 1;
        return true;
    }

    static constexpr uint8_t controlIndex(uint8_t knobIndex, uint8_t layerIndex)
    {
        return static_cast<uint8_t>(layerIndex * synth::kKnobControlMax + knobIndex);
    }
};

inline void render(NodeInstance &inst, size_t frames)
{
    if (frames > 0 && (inst.audioConnectedPortMask & synth::portBit(0)) != 0)
    {
        const float sample = noderender::portSample(inst, 0, frames - 1);
        const float volts = sample < 0.0f ? 0.0f : (sample > 10.0f ? 10.0f : sample);
        int layer = static_cast<int>(volts / (10.0f / static_cast<float>(inst.knob->layerCount)));
        if (layer >= inst.knob->layerCount) layer = inst.knob->layerCount - 1;
        inst.knob->activeLayer = static_cast<uint8_t>(layer);
    }
    for (size_t i = 0; i < frames; ++i)
    {
        inst.outL[i] = inst.knob->controls[KnobState::controlIndex(0, inst.knob->activeLayer)].currentValue;
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    if (!inst.knob->resizeControls(p.knobCount, p.knobLayerCount)) return;
    inst.knob->activeLayer = p.knobActiveLayer < inst.knob->layerCount ? p.knobActiveLayer : 0;
    for (uint8_t layerIndex = 0; layerIndex < inst.knob->layerCount; ++layerIndex)
    {
        for (uint8_t knobIndex = 0; knobIndex < inst.knob->count; ++knobIndex)
        {
            const uint8_t index = KnobState::controlIndex(knobIndex, layerIndex);
            KnobControl &control = inst.knob->controls[index];
            const WidgetParams::KnobControlParams &source = p.knobs[index];
            memcpy(control.label, source.label, sizeof(control.label));
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
}

inline void destroy(NodeInstance &inst)
{
    if (inst.knob != nullptr)
    {
        inst.knob->~KnobState();
        heap_caps_free(inst.knob);
        inst.knob = nullptr;
    }
}

inline bool setParam(NodeInstance &inst, const char *name, float value, uint8_t controlIndex = 0)
{
    if (name == nullptr || inst.knob == nullptr)
    {
        return false;
    }
    if (strcmp(name, "activeLayer") == 0)
    {
        int layer = static_cast<int>(value + 0.5f);
        if (layer < 0) layer = 0;
        if (layer >= inst.knob->layerCount) layer = inst.knob->layerCount - 1;
        inst.knob->activeLayer = static_cast<uint8_t>(layer);
        return true;
    }
    if (strcmp(name, "layer") == 0)
    {
        float volts = value < 0.0f ? 0.0f : (value > 10.0f ? 10.0f : value);
        int layer = static_cast<int>(volts / (10.0f / static_cast<float>(inst.knob->layerCount)));
        if (layer >= inst.knob->layerCount) layer = inst.knob->layerCount - 1;
        inst.knob->activeLayer = static_cast<uint8_t>(layer);
        return true;
    }
    if (!inst.knob->controls.contains(controlIndex))
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
    const int layerCount = presetparser::getInt(p, "layerCount", 1);
    out.knobLayerCount = static_cast<uint8_t>(layerCount < 1 ? 1 : (layerCount > static_cast<int>(synth::kKnobLayerMax) ? synth::kKnobLayerMax : layerCount));
    const int activeLayer = presetparser::getInt(p, "activeLayer", 0);
    out.knobActiveLayer = static_cast<uint8_t>(activeLayer < 0 ? 0 : (activeLayer >= out.knobLayerCount ? out.knobLayerCount - 1 : activeLayer));
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

    auto parseControl = [](cJSON *item, WidgetParams::KnobControlParams &control)
    {
        if (!cJSON_IsObject(item)) return;
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
    };

    auto parseLabels = [](cJSON *layer, WidgetParams::KnobControlParams *controls, uint8_t count)
    {
        cJSON *labels = layer != nullptr ? cJSON_GetObjectItemCaseSensitive(layer, "labels") : nullptr;
        for (uint8_t i = 0; i < count; ++i)
        {
            cJSON *label = cJSON_IsArray(labels) ? cJSON_GetArrayItem(labels, i) : nullptr;
            if (cJSON_IsString(label))
                snprintf(controls[i].label, sizeof(controls[i].label), "%s", label->valuestring);
        }
    };
    cJSON *controls = p != nullptr ? cJSON_GetObjectItemCaseSensitive(p, "knobs") : nullptr;
    if (cJSON_IsArray(controls))
    {
        const int arrayCount = cJSON_GetArraySize(controls);
        out.knobCount = static_cast<uint8_t>(arrayCount < 1 ? 1 : (arrayCount > static_cast<int>(synth::kKnobControlMax) ? synth::kKnobControlMax : arrayCount));
        for (uint8_t knobIndex = 0; knobIndex < out.knobCount; ++knobIndex)
        {
            parseControl(cJSON_GetArrayItem(controls, knobIndex), out.knobs[knobIndex]);
        }
    }

    parseLabels(p, out.knobs, out.knobCount);
    cJSON *layers = p != nullptr ? cJSON_GetObjectItemCaseSensitive(p, "layers") : nullptr;
    if (cJSON_IsArray(layers) && cJSON_GetArraySize(layers) > 0)
    {
        const int arrayLayerCount = cJSON_GetArraySize(layers);
        out.knobLayerCount = static_cast<uint8_t>(arrayLayerCount > static_cast<int>(synth::kKnobLayerMax) ? synth::kKnobLayerMax : arrayLayerCount);
        if (out.knobActiveLayer >= out.knobLayerCount) out.knobActiveLayer = 0;
        for (uint8_t layerIndex = 0; layerIndex < out.knobLayerCount; ++layerIndex)
        {
            cJSON *layer = cJSON_GetArrayItem(layers, layerIndex);
            cJSON *layerControls = cJSON_IsObject(layer) ? cJSON_GetObjectItemCaseSensitive(layer, "knobs") : nullptr;
            if (!cJSON_IsArray(layerControls)) continue;
            if (layerIndex == 0)
            {
                const int arrayCount = cJSON_GetArraySize(layerControls);
                out.knobCount = static_cast<uint8_t>(arrayCount < 1 ? 1 : (arrayCount > static_cast<int>(synth::kKnobControlMax) ? synth::kKnobControlMax : arrayCount));
            }
            for (uint8_t knobIndex = 0; knobIndex < out.knobCount; ++knobIndex)
            {
                const uint8_t index = static_cast<uint8_t>(layerIndex * synth::kKnobControlMax + knobIndex);
                parseControl(cJSON_GetArrayItem(layerControls, knobIndex), out.knobs[index]);
            }
            parseLabels(layer, out.knobs + layerIndex * synth::kKnobControlMax, out.knobCount);
        }
    }

    cJSON *bindings = p != nullptr ? cJSON_GetObjectItemCaseSensitive(p, "bindings") : nullptr;
    for (uint8_t knobIndex = 0; knobIndex < out.knobCount; ++knobIndex)
    {
        cJSON *binding = cJSON_IsArray(bindings) ? cJSON_GetArrayItem(bindings, knobIndex) : nullptr;
        const WidgetParams::KnobControlParams &first = out.knobs[knobIndex];
        cJSON *bindingCc = cJSON_IsObject(binding) ? cJSON_GetObjectItemCaseSensitive(binding, "cc") : nullptr;
        cJSON *bindingChannel = cJSON_IsObject(binding) ? cJSON_GetObjectItemCaseSensitive(binding, "channel") : nullptr;
        const int16_t sharedCc = cJSON_IsNumber(bindingCc) ? static_cast<int16_t>(bindingCc->valueint) : first.cc;
        const int8_t sharedChannel = cJSON_IsNumber(bindingChannel) ? static_cast<int8_t>(bindingChannel->valueint) : first.channel;
        for (uint8_t layerIndex = 0; layerIndex < out.knobLayerCount; ++layerIndex)
        {
            WidgetParams::KnobControlParams &control = out.knobs[layerIndex * synth::kKnobControlMax + knobIndex];
            control.cc = sharedCc;
            control.channel = sharedChannel;
        }
    }
}

} // namespace nodes::knob

using nodes::knob::KnobState;
using nodes::knob::KnobControl;
