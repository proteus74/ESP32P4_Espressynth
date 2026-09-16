#pragma once

#include <cstddef>

#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"

// Mirror image of SwitchNode.h: one audio input routed to whichever of
// "out1"/"out2" is selected (same MIDI-CC-learnable selectOut2 toggle
// convention as SwitchState::selectIn2 - see Graph.h's handleMidiEvent/
// applyLiveParams). The non-selected output is simply left untouched each
// block - its buffer still holds whatever bytes render() last wrote there
// while it WAS selected, which is exactly the desired "frozen at its own
// last value" behavior (see OutputSwitchState's comment in NodeState.h).
// held1/held2 exist only to survive a graph rebuild (preset save/reload
// zeroes every buffer) - build() below re-primes both outputs from them.
namespace nodes::outputswitch
{

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    auto &s = inst.outputSwitch;
    float *active = s.selectOut2 ? inst.outR : inst.outL;
    float last = s.selectOut2 ? s.held2 : s.held1;
    for (size_t i = 0; i < frames; ++i)
    {
        last = portSample(inst, 0, i);
        active[i] = last;
    }
    if (s.selectOut2)
        s.held2 = last;
    else
        s.held1 = last;
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    inst.outputSwitch.cc = p.outputSwitchCc;
    inst.outputSwitch.channel = p.outputSwitchChannel;
    inst.outputSwitch.selectOut2 = p.outputSwitchSelectOut2;
    inst.outputSwitch.held1 = p.outputSwitchHeld1;
    inst.outputSwitch.held2 = p.outputSwitchHeld2;
    // Re-prime both outputs from their persisted held value so a freshly
    // rebuilt graph (preset load) shows the last-seen value immediately
    // instead of silence, until each output is actually selected again.
    for (size_t i = 0; i < synth::kBlockSize; ++i)
    {
        inst.outL[i] = inst.outputSwitch.held1;
        inst.outR[i] = inst.outputSwitch.held2;
    }
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    cJSON *cc = p != nullptr ? cJSON_GetObjectItemCaseSensitive(p, "cc") : nullptr;
    out.outputSwitchCc = cJSON_IsNumber(cc) ? static_cast<int16_t>(cc->valueint) : -1;
    cJSON *ch = p != nullptr ? cJSON_GetObjectItemCaseSensitive(p, "channel") : nullptr;
    out.outputSwitchChannel = cJSON_IsNumber(ch) ? static_cast<int8_t>(ch->valueint) : -1;
    out.outputSwitchSelectOut2 = presetparser::getBool(p, "selectOut2", false);
    out.outputSwitchHeld1 = presetparser::getFloat(p, "held1", 0.0f);
    out.outputSwitchHeld2 = presetparser::getFloat(p, "held2", 0.0f);
}

} // namespace nodes::outputswitch
