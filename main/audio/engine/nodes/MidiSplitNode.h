#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../NodeInstance.h"
#include "../PresetParser.h"

// midi-in's zone-split counterpart: routes each incoming note to the first
// zone (of up to 4) whose [fromNote, toNote] range contains it, applying
// that zone's whole-octave transpose to the note/frequency it forwards. A
// note falling in a gap between zones is silently dropped (no forward). No
// DSP state of its own (no render() - see Graph::deliverNoteControlRoutes's
// MidiSplit case for how these get used), no audio output.
// MidiSplitState/MidiSplitZone (value members) stay in NodeState.h.
namespace nodes::midisplit
{

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    inst.midiSplit.zoneCount = p.midiSplitZoneCount;
    for (uint8_t z = 0; z < 4; ++z)
    {
        inst.midiSplit.zones[z].fromNote = p.midiSplitZoneFrom[z];
        inst.midiSplit.zones[z].toNote = p.midiSplitZoneTo[z];
        inst.midiSplit.zones[z].octaveTranspose = p.midiSplitZoneOctave[z];
    }
}

// "zone0From".."zone3Octave" - per-zone live field, matches main.js's
// deliverToWidget() naming (zoneCount itself is structural, never delivered
// here - see GraphBuilder's applyParams comment).
inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr || strncmp(name, "zone", 4) != 0 || name[4] < '0' || name[4] > '3')
    {
        return false;
    }
    const uint8_t zoneIdx = static_cast<uint8_t>(name[4] - '0');
    const char *suffix = name + 5;
    if (strcmp(suffix, "From") == 0)
    {
        inst.midiSplit.zones[zoneIdx].fromNote = static_cast<int8_t>(value);
        return true;
    }
    if (strcmp(suffix, "To") == 0)
    {
        inst.midiSplit.zones[zoneIdx].toNote = static_cast<int8_t>(value);
        return true;
    }
    if (strcmp(suffix, "Octave") == 0)
    {
        inst.midiSplit.zones[zoneIdx].octaveTranspose = static_cast<int8_t>(value);
        return true;
    }
    return false;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    const int zoneCount = presetparser::getInt(p, "zoneCount", 0);
    out.midiSplitZoneCount = static_cast<uint8_t>(zoneCount < 0 ? 0 : (zoneCount > 4 ? 4 : zoneCount));
    cJSON *zones = p != nullptr ? cJSON_GetObjectItemCaseSensitive(p, "zones") : nullptr;
    if (cJSON_IsArray(zones))
    {
        int i = 0;
        cJSON *z;
        cJSON_ArrayForEach(z, zones)
        {
            if (i >= 4)
            {
                break;
            }
            out.midiSplitZoneFrom[i] = static_cast<int8_t>(presetparser::getInt(z, "from", 0));
            out.midiSplitZoneTo[i] = static_cast<int8_t>(presetparser::getInt(z, "to", 127));
            out.midiSplitZoneOctave[i] = static_cast<int8_t>(presetparser::getInt(z, "octave", 0));
            ++i;
        }
    }
}

} // namespace nodes::midisplit
