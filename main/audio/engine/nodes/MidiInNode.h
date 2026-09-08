#pragma once

#include "../NodeInstance.h"
#include "../PresetParser.h"

// Graph participant only - not a DSP-rendered node (no audio ports, no
// render()), exists so the voiced/global BFS has a starting point and
// ControlRoutes can reference it. See Graph::handleMidiEvent for the actual
// note-on/off/CC routing. MidiInState (value member) stays in NodeState.h.
namespace nodes::midiin
{

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    inst.midiIn.channel = p.channel;
    inst.midiIn.voices = p.voices;
    inst.midiIn.overflowMode = p.overflowMode;
    inst.midiIn.glide = p.glide;
    inst.midiIn.glideTime = p.glideTime;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    cJSON *ch = p != nullptr ? cJSON_GetObjectItemCaseSensitive(p, "channel") : nullptr;
    out.channel = cJSON_IsNumber(ch) ? static_cast<int8_t>(ch->valueint) : -1;
    out.voices = static_cast<uint8_t>(presetparser::getInt(p, "voices", 1));
    out.overflowMode = overflowModeFromString(presetparser::getString(p, "overflowMode", "steal-oldest"));
    out.glide = presetparser::getBool(p, "glide", false);
    out.glideTime = presetparser::getFloat(p, "glideTime", 0.0f);
}

} // namespace nodes::midiin
