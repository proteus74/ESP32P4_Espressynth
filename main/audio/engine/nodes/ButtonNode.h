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

// Button (CC-learn momentary switch) and UpDownButton (two independently
// CC-learnable buttons driving one shared value) - grouped since they share
// the same value0/value127-or-valueList / min-max-step-or-valueList shape
// and CC-learn identity pattern as KnobNode. Both heap-allocated per
// instance (see NodeInstance.h's pointer-member comment).
namespace nodes::button
{

// Same 2-point-map/value-list machinery as KnobState's mappedValue(), minus
// the CC-learn identity (cc/channel/raw). As soon
// as valueList has >=2 entries it automatically wins over the fixed
// value0/value127 (the list "wins" on its own,
// matching main.js's buttonCurrentOutput()). Fixed: value0/value127 selected
// by whether the incoming CC value is below/at-or-above the halfway point
// (63/64). By default, the list advances on a rising CC edge. With
// switchMode, every high CC message toggles the fixed output or advances
// the list, including controllers that never send a release.
// Which MIDI message a learned button binding reacts to; `cc` then holds the
// CC, note or program number (preset/JSON: bindings[].midiType "cc"/"note"/"program").
constexpr uint8_t kButtonMidiCc = 0;
constexpr uint8_t kButtonMidiNote = 1;
constexpr uint8_t kButtonMidiProgram = 2;

inline uint8_t buttonMidiTypeFromString(const char *name)
{
    if (name != nullptr && strcmp(name, "note") == 0) return kButtonMidiNote;
    if (name != nullptr && strcmp(name, "program") == 0) return kButtonMidiProgram;
    return kButtonMidiCc;
}

struct ButtonControl
{
    char label[48] = {};
    uint8_t midiType = kButtonMidiCc;
    int16_t cc = -1;
    int8_t channel = -1;
    float value0 = 0.0f;
    float value127 = 10.0f; // Gate-high default 10V (see CvConventions.h)
    float valueList[synth::kValueListMax] = {};
    uint8_t valueListCount = 0;
    uint8_t listIndex = 0;
    bool wasAbove = false;
    bool switchMode = false;
    float currentValue = 0.0f;
    uint32_t midiFeedback = UINT32_MAX;

    uint8_t feedbackValue() const
    {
        if (valueListCount >= 2)
            return static_cast<uint8_t>((listIndex < valueListCount ? listIndex : 0) * 127 / (valueListCount - 1));
        return wasAbove ? 127 : 0;
    }

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

struct ButtonState
{
    uint8_t count = 1;
    uint8_t layerCount = 1;
    uint8_t activeLayer = 0;
    uint8_t feedbackLayer = UINT8_MAX;
    LayerControls<ButtonControl> controls;

    bool resizeControls(uint8_t newCount, uint8_t newLayers)
    {
        if (!controls.resize(newCount, newLayers)) return false;
        count = newCount;
        layerCount = newLayers;
        if (activeLayer >= layerCount) activeLayer = layerCount - 1;
        return true;
    }

    static constexpr uint8_t controlIndex(uint8_t buttonIndex, uint8_t layerIndex)
    {
        return static_cast<uint8_t>(layerIndex * synth::kButtonControlMax + buttonIndex);
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
    const ButtonState &button = *inst.button;
    const float value = button.controls[ButtonState::controlIndex(0, button.activeLayer)].currentValue;
    for (size_t i = 0; i < frames; ++i)
    {
        inst.outL[i] = value;
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
    ButtonState &button = *inst.button;
    if (!button.resizeControls(p.buttonCount, p.buttonLayerCount)) return;
    button.activeLayer = p.buttonActiveLayer < button.layerCount ? p.buttonActiveLayer : 0;
    for (uint8_t layer = 0; layer < button.layerCount; ++layer)
    for (uint8_t buttonIndex = 0; buttonIndex < button.count; ++buttonIndex)
    {
        const uint8_t controlIndex = ButtonState::controlIndex(buttonIndex, layer);
        const WidgetParams::ButtonControlParams &source = p.buttons[controlIndex];
        ButtonControl &control = button.controls[controlIndex];
        memcpy(control.label, source.label, sizeof(control.label));
        control.midiType = source.midiType;
        control.cc = source.cc;
        control.channel = source.channel;
        control.value0 = source.value0;
        control.value127 = source.value127;
        control.valueListCount = source.valueListCount;
        for (uint8_t i = 0; i < source.valueListCount && i < synth::kValueListMax; ++i)
            control.valueList[i] = source.valueList[i];
        control.listIndex = source.listIndex < source.valueListCount ? source.listIndex : 0;
        control.wasAbove = source.wasAbove;
        control.switchMode = source.switchMode;
        control.currentValue = control.currentOutput();
    }
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
        inst.button->~ButtonState();
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

inline bool setParamButton(NodeInstance &inst, const char *name, float value, uint8_t controlIndex = 0)
{
    if (name == nullptr || inst.button == nullptr)
    {
        return false;
    }
    if (strcmp(name, "activeLayer") == 0)
    {
        int layer = static_cast<int>(value + 0.5f);
        if (layer < 0) layer = 0;
        if (layer >= inst.button->layerCount) layer = inst.button->layerCount - 1;
        inst.button->activeLayer = static_cast<uint8_t>(layer);
        return true;
    }
    if (strcmp(name, "layer") == 0)
    {
        const float volts = value < 0.0f ? 0.0f : (value > 10.0f ? 10.0f : value);
        int layer = static_cast<int>(volts / (10.0f / static_cast<float>(inst.button->layerCount)));
        if (layer >= inst.button->layerCount) layer = inst.button->layerCount - 1;
        inst.button->activeLayer = static_cast<uint8_t>(layer);
        return true;
    }
    if (!inst.button->controls.contains(controlIndex)) return false;
    ButtonControl &control = inst.button->controls[controlIndex];
    if (strcmp(name, "listIndex") == 0)
    {
        control.listIndex = static_cast<uint8_t>(value);
        control.currentValue = control.currentOutput();
        return true;
    }
    if (strcmp(name, "wasAbove") == 0)
    {
        control.wasAbove = value != 0.0f;
        control.currentValue = control.currentOutput();
        return true;
    }
    if (strcmp(name, "value0") == 0)
    {
        control.value0 = value;
        control.currentValue = control.currentOutput();
        return true;
    }
    if (strcmp(name, "value127") == 0)
    {
        control.value127 = value;
        control.currentValue = control.currentOutput();
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
    const int count = presetparser::getInt(p, "buttonCount", 1);
    out.buttonCount = static_cast<uint8_t>(count < 1 ? 1 : (count > static_cast<int>(synth::kButtonControlMax) ? synth::kButtonControlMax : count));
    const int layerCount = presetparser::getInt(p, "layerCount", 1);
    out.buttonLayerCount = static_cast<uint8_t>(layerCount < 1 ? 1 : (layerCount > static_cast<int>(synth::kButtonLayerMax) ? synth::kButtonLayerMax : layerCount));
    const int activeLayer = presetparser::getInt(p, "activeLayer", 0);
    out.buttonActiveLayer = static_cast<uint8_t>(activeLayer < 0 ? 0 : (activeLayer >= out.buttonLayerCount ? out.buttonLayerCount - 1 : activeLayer));

    auto parseControl = [](cJSON *item, WidgetParams::ButtonControlParams &control)
    {
        if (!cJSON_IsObject(item)) return;
        control.value0 = presetparser::getFloat(item, "value0", 0.0f);
        control.value127 = presetparser::getFloat(item, "value127", 10.0f);
        control.valueListCount = presetparser::getFloatArray(item, "valueList", control.valueList,
                                                              static_cast<uint8_t>(synth::kValueListMax));
        control.listIndex = static_cast<uint8_t>(presetparser::getInt(item, "listIndex", 0));
        control.wasAbove = presetparser::getBool(item, "wasAbove", false);
    };

    cJSON *layers = p != nullptr ? cJSON_GetObjectItemCaseSensitive(p, "layers") : nullptr;
    for (uint8_t layerIndex = 0; layerIndex < out.buttonLayerCount; ++layerIndex)
    {
        cJSON *layer = cJSON_IsArray(layers) ? cJSON_GetArrayItem(layers, layerIndex) : nullptr;
        cJSON *buttons = cJSON_IsObject(layer) ? cJSON_GetObjectItemCaseSensitive(layer, "buttons") : nullptr;
        cJSON *labels = cJSON_IsObject(layer) ? cJSON_GetObjectItemCaseSensitive(layer, "labels") : nullptr;
        for (uint8_t buttonIndex = 0; buttonIndex < out.buttonCount; ++buttonIndex)
        {
            cJSON *item = cJSON_IsArray(buttons) ? cJSON_GetArrayItem(buttons, buttonIndex) : nullptr;
            WidgetParams::ButtonControlParams &control = out.buttons[ButtonState::controlIndex(buttonIndex, layerIndex)];
            parseControl(item, control);
            cJSON *label = cJSON_IsArray(labels) ? cJSON_GetArrayItem(labels, buttonIndex) : nullptr;
            if (cJSON_IsString(label))
                snprintf(control.label, sizeof(control.label), "%s", label->valuestring);
        }
    }

    cJSON *bindings = p != nullptr ? cJSON_GetObjectItemCaseSensitive(p, "bindings") : nullptr;
    for (uint8_t buttonIndex = 0; buttonIndex < out.buttonCount; ++buttonIndex)
    {
        cJSON *binding = cJSON_IsArray(bindings) ? cJSON_GetArrayItem(bindings, buttonIndex) : nullptr;
        cJSON *cc = cJSON_IsObject(binding) ? cJSON_GetObjectItemCaseSensitive(binding, "cc") : nullptr;
        cJSON *channel = cJSON_IsObject(binding) ? cJSON_GetObjectItemCaseSensitive(binding, "channel") : nullptr;
        cJSON *switchMode = cJSON_IsObject(binding) ? cJSON_GetObjectItemCaseSensitive(binding, "switchMode") : nullptr;
        cJSON *midiType = cJSON_IsObject(binding) ? cJSON_GetObjectItemCaseSensitive(binding, "midiType") : nullptr;
        for (uint8_t layerIndex = 0; layerIndex < out.buttonLayerCount; ++layerIndex)
        {
            WidgetParams::ButtonControlParams &control = out.buttons[ButtonState::controlIndex(buttonIndex, layerIndex)];
            control.cc = cJSON_IsNumber(cc) ? static_cast<int16_t>(cc->valueint) : -1;
            control.channel = cJSON_IsNumber(channel) ? static_cast<int8_t>(channel->valueint) : -1;
            control.switchMode = cJSON_IsTrue(switchMode);
            control.midiType = buttonMidiTypeFromString(cJSON_IsString(midiType) ? midiType->valuestring : nullptr);
        }
    }
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
using nodes::button::ButtonControl;
using nodes::button::UpDownButtonState;
