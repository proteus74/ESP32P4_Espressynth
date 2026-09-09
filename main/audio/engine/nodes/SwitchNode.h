#pragma once

#include <cstddef>

#include "../NodeInstance.h"
#include "../PresetParser.h"

// 2-input CV selector (SwitchState in NodeState.h) - MIDI-CC-learnable
// selectIn2 toggle, same cc/channel pattern as Knob/Button.
namespace nodes::switchnode
{

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    // Selected port resolved once per block instead of re-checked every
    // sample (plan section 5, Switch row: "Gewählten Eingangszeiger einmal
    // wählen") - selectIn2 only ever changes via a MIDI-CC event
    // (Graph::handleMidiEvent), never mid-render.
    //
    // "Bei passender Ownership direkt weiterreichen" (pass the selected
    // input's buffer straight through instead of copying it) was
    // considered and rejected for the same reason as LFO's global-mirror
    // buffer view (see LfoNode.h): the selection can change live at
    // runtime, and Graph::renderBlock's per-instance idle-silence handling
    // operates on this instance's OWN outL - aliasing it to a currently-
    // selected input's buffer would let that memset corrupt whichever
    // OTHER instance actually owns it.
    const int selectedPort = inst.switchState.selectIn2 ? 1 : 0;
    for (size_t i = 0; i < frames; ++i)
    {
        inst.outL[i] = portSample(inst, selectedPort, i);
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    inst.switchState.cc = p.switchCc;
    inst.switchState.channel = p.switchChannel;
    inst.switchState.selectIn2 = p.switchSelectIn2;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    cJSON *cc = p != nullptr ? cJSON_GetObjectItemCaseSensitive(p, "cc") : nullptr;
    out.switchCc = cJSON_IsNumber(cc) ? static_cast<int16_t>(cc->valueint) : -1;
    cJSON *ch = p != nullptr ? cJSON_GetObjectItemCaseSensitive(p, "channel") : nullptr;
    out.switchChannel = cJSON_IsNumber(ch) ? static_cast<int8_t>(ch->valueint) : -1;
    out.switchSelectIn2 = presetparser::getBool(p, "selectIn2", false);
}

} // namespace nodes::switchnode
