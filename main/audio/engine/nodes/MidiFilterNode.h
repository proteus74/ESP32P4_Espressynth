#pragma once

#include <cmath>
#include <cstring>
#include "../NodeInstance.h"
#include "../PresetParser.h"

namespace nodes::midifilter
{
struct MidiFilterState
{
    MidiInState midi;
    int8_t channel = -1;
    int16_t note = -1;
    uint8_t activeNotes[16][16] = {};
    uint16_t admittedChannels = 0;

    bool accepts(uint8_t status, uint8_t data1, uint8_t data2)
    {
        if (status >= 0xF0) return true;
        if (status < 0x80 || data1 > 127 || data2 > 127) return false;
        const uint8_t ch = status & 15;
        const uint8_t kind = status >> 4;
        const uint8_t bit = static_cast<uint8_t>(1u << (data1 & 7));
        uint8_t &active = activeNotes[ch][data1 >> 3];
        const bool noteOff = kind == 8 || (kind == 9 && data2 == 0);
        if (noteOff)
        {
            if (!(active & bit) && ((channel >= 0 && channel != ch) || (note >= 0 && note != data1))) return false;
            active &= static_cast<uint8_t>(~bit);
            return true;
        }
        // Pedal release and panic messages must still reach admitted notes
        // after a channel-setting change.
        const bool release = kind == 11 && ((data1 == 64 && data2 < 64) || data1 == 120 || data1 == 123);
        if (channel >= 0 && channel != ch && !(release && (admittedChannels & (1u << ch)))) return false;
        if ((kind == 9 || kind == 10) && note >= 0 && note != data1) return false;
        if (kind == 9)
        {
            active |= bit;
            admittedChannels |= static_cast<uint16_t>(1u << ch);
        }
        if (kind == 11 && (data1 == 120 || data1 == 123)) std::memset(activeNotes[ch], 0, 16);
        return true;
    }
};

inline int criterion(float value, int maximum)
{
    return std::isfinite(value) && value >= -1 && value <= maximum ? static_cast<int>(std::round(value)) : -1;
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    inst.midiFilter->channel = p.channel;
    inst.midiFilter->note = p.midiFilterNote;
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (strcmp(name, "channel") == 0) inst.midiFilter->channel = criterion(value, 15);
    else if (strcmp(name, "note") == 0) inst.midiFilter->note = criterion(value, 127);
    else return false;
    return true;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.channel = criterion(presetparser::getFloat(p, "channel", -1), 15);
    out.midiFilterNote = criterion(presetparser::getFloat(p, "note", -1), 127);
}
} // namespace nodes::midifilter
