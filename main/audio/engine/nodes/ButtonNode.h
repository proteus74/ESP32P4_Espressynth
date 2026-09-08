#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "esp_heap_caps.h"

#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"

// Button (CC-learn momentary switch) and UpDownButton (two independently
// CC-learnable buttons driving one shared value) - grouped since they share
// the same value0/value127-or-valueList / min-max-step-or-valueList shape
// and CC-learn identity pattern as KnobNode. Both heap-allocated per
// instance (see NodeInstance.h's pointer-member comment).
namespace nodes::button
{

// Same 2-point-map/value-list machinery as KnobState's mappedValue(), minus
// the CC-learn identity (cc/channel/raw) - a MIDI momentary switch. As soon
// as valueList has >=2 entries it automatically wins over the fixed
// value0/value127 (no separate mode switch - the list "wins" on its own,
// matching main.js's buttonCurrentOutput()). Fixed: value0/value127 selected
// by whether the incoming CC value is below/at-or-above the halfway point
// (63/64). List: listIndex advances once per press edge (wasAbove flips
// false->true) rather than being derived from the raw value directly -
// ported from main.js's buttonStates entries.
struct ButtonState
{
    int16_t cc = -1;
    int8_t channel = -1;
    float value0 = 0.0f;
    float value127 = 10.0f; // Gate-high default 10V (see CvConventions.h)
    float valueList[synth::kValueListMax] = {};
    uint8_t valueListCount = 0;
    uint8_t listIndex = 0;
    bool wasAbove = false;
    float currentValue = 0.0f;

    float currentOutput() const
    {
        if (valueListCount >= 2)
        {
            const uint8_t idx = listIndex < valueListCount ? listIndex : 0;
            return valueList[idx];
        }
        return wasAbove ? value127 : value0;
    }
};

// Every CC message on either the up or down CC counts as one tick,
// regardless of its value (0 and 127 are treated identically - matches
// relative/encoder-style controllers, not just momentary above/below-64
// presses) - see Graph::handleMidiEvent's UpDownButton branch. List mode
// (valueList has >=2 entries) wins over the plain min/max/step range, same
// "no separate mode switch" convention as Button/Knob. Range mode clamps at
// minVal/maxVal (no wraparound); list mode clamps listIndex at the array
// bounds.
struct UpDownButtonState
{
    int16_t ccUp = -1;
    int8_t channelUp = -1;
    int16_t ccDown = -1;
    int8_t channelDown = -1;
    float minVal = 0.0f;
    float maxVal = 10.0f;
    float step = 1.0f;
    float valueList[synth::kValueListMax] = {};
    uint8_t valueListCount = 0;
    uint8_t listIndex = 0;
    float currentValue = 0.0f;

    float currentOutput() const
    {
        if (valueListCount >= 2)
        {
            const uint8_t idx = listIndex < valueListCount ? listIndex : 0;
            return valueList[idx];
        }
        return currentValue;
    }
};

inline void renderButton(NodeInstance &inst, size_t frames)
{
    for (size_t i = 0; i < frames; ++i)
    {
        inst.outL[i] = inst.button->currentValue;
    }
}

inline void renderUpDownButton(NodeInstance &inst, size_t frames)
{
    // currentOutput() only changes between blocks (MIDI CC handling), never
    // within a block - call once instead of recomputing the same result up
    // to 128x.
    const float value = inst.upDownButton->currentOutput();
    for (size_t i = 0; i < frames; ++i)
    {
        inst.outL[i] = value;
    }
}

inline void buildButton(NodeInstance &inst, const WidgetParams &p)
{
    inst.button->cc = p.buttonCc;
    inst.button->channel = p.buttonChannel;
    inst.button->value0 = p.buttonValue0;
    inst.button->value127 = p.buttonValue127;
    inst.button->valueListCount = p.valueListCount;
    for (uint8_t i = 0; i < p.valueListCount && i < synth::kValueListMax; ++i)
    {
        inst.button->valueList[i] = p.valueList[i];
    }
    inst.button->listIndex = p.buttonListIndex < p.valueListCount ? p.buttonListIndex : 0;
    inst.button->wasAbove = p.buttonWasAbove;
    inst.button->currentValue = inst.button->currentOutput();
}

inline void buildUpDownButton(NodeInstance &inst, const WidgetParams &p)
{
    inst.upDownButton->ccUp = p.upDownCcUp;
    inst.upDownButton->channelUp = p.upDownChannelUp;
    inst.upDownButton->ccDown = p.upDownCcDown;
    inst.upDownButton->channelDown = p.upDownChannelDown;
    inst.upDownButton->minVal = p.upDownMin;
    inst.upDownButton->maxVal = p.upDownMax;
    inst.upDownButton->step = p.upDownStep;
    inst.upDownButton->valueListCount = p.valueListCount;
    for (uint8_t i = 0; i < p.valueListCount && i < synth::kValueListMax; ++i)
    {
        inst.upDownButton->valueList[i] = p.valueList[i];
    }
    inst.upDownButton->listIndex = p.upDownListIndex < p.valueListCount ? p.upDownListIndex : 0;
    inst.upDownButton->currentValue = p.upDownCurrentValue;
}

inline void destroyButton(NodeInstance &inst)
{
    if (inst.button != nullptr)
    {
        heap_caps_free(inst.button);
        inst.button = nullptr;
    }
}

inline void destroyUpDownButton(NodeInstance &inst)
{
    if (inst.upDownButton != nullptr)
    {
        heap_caps_free(inst.upDownButton);
        inst.upDownButton = nullptr;
    }
}

inline bool setParamButton(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr)
    {
        return false;
    }
    if (strcmp(name, "listIndex") == 0)
    {
        inst.button->listIndex = static_cast<uint8_t>(value);
        inst.button->currentValue = inst.button->currentOutput();
        return true;
    }
    if (strcmp(name, "wasAbove") == 0)
    {
        inst.button->wasAbove = value != 0.0f;
        inst.button->currentValue = inst.button->currentOutput();
        return true;
    }
    if (strcmp(name, "value0") == 0)
    {
        inst.button->value0 = value;
        inst.button->currentValue = inst.button->currentOutput();
        return true;
    }
    if (strcmp(name, "value127") == 0)
    {
        inst.button->value127 = value;
        inst.button->currentValue = inst.button->currentOutput();
        return true;
    }
    return false;
}

inline bool setParamUpDownButton(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr)
    {
        return false;
    }
    if (strcmp(name, "listIndex") == 0)
    {
        inst.upDownButton->listIndex = static_cast<uint8_t>(value);
        return true;
    }
    if (strcmp(name, "currentValue") == 0)
    {
        inst.upDownButton->currentValue = value;
        return true;
    }
    if (strcmp(name, "min") == 0)
    {
        inst.upDownButton->minVal = value;
        return true;
    }
    if (strcmp(name, "max") == 0)
    {
        inst.upDownButton->maxVal = value;
        return true;
    }
    if (strcmp(name, "step") == 0)
    {
        inst.upDownButton->step = value;
        return true;
    }
    return false;
}

inline void parsePresetButton(cJSON *p, WidgetParams &out)
{
    cJSON *cc = p != nullptr ? cJSON_GetObjectItemCaseSensitive(p, "cc") : nullptr;
    out.buttonCc = cJSON_IsNumber(cc) ? static_cast<int16_t>(cc->valueint) : -1;
    cJSON *ch = p != nullptr ? cJSON_GetObjectItemCaseSensitive(p, "channel") : nullptr;
    out.buttonChannel = cJSON_IsNumber(ch) ? static_cast<int8_t>(ch->valueint) : -1;
    out.buttonValue0 = presetparser::getFloat(p, "value0", 0.0f);
    out.buttonValue127 = presetparser::getFloat(p, "value127", 10.0f); // CV convention: gate-high default 10V instead of 1
    out.buttonListIndex = static_cast<uint8_t>(presetparser::getInt(p, "listIndex", 0));
    out.buttonWasAbove = presetparser::getBool(p, "wasAbove", false);
    out.valueListCount = presetparser::getFloatArray(p, "valueList", out.valueList, static_cast<uint8_t>(synth::kValueListMax));
}

inline void parsePresetUpDownButton(cJSON *p, WidgetParams &out)
{
    cJSON *ccUp = p != nullptr ? cJSON_GetObjectItemCaseSensitive(p, "ccUp") : nullptr;
    out.upDownCcUp = cJSON_IsNumber(ccUp) ? static_cast<int16_t>(ccUp->valueint) : -1;
    cJSON *chUp = p != nullptr ? cJSON_GetObjectItemCaseSensitive(p, "channelUp") : nullptr;
    out.upDownChannelUp = cJSON_IsNumber(chUp) ? static_cast<int8_t>(chUp->valueint) : -1;
    cJSON *ccDown = p != nullptr ? cJSON_GetObjectItemCaseSensitive(p, "ccDown") : nullptr;
    out.upDownCcDown = cJSON_IsNumber(ccDown) ? static_cast<int16_t>(ccDown->valueint) : -1;
    cJSON *chDown = p != nullptr ? cJSON_GetObjectItemCaseSensitive(p, "channelDown") : nullptr;
    out.upDownChannelDown = cJSON_IsNumber(chDown) ? static_cast<int8_t>(chDown->valueint) : -1;
    out.upDownMin = presetparser::getFloat(p, "min", 0.0f);
    out.upDownMax = presetparser::getFloat(p, "max", 10.0f);
    out.upDownStep = presetparser::getFloat(p, "step", 1.0f);
    out.upDownListIndex = static_cast<uint8_t>(presetparser::getInt(p, "listIndex", 0));
    out.upDownCurrentValue = presetparser::getFloat(p, "currentValue", 0.0f);
    out.valueListCount = presetparser::getFloatArray(p, "valueList", out.valueList, static_cast<uint8_t>(synth::kValueListMax));
}

} // namespace nodes::button

using nodes::button::ButtonState;
using nodes::button::UpDownButtonState;
