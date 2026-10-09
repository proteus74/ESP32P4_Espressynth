#pragma once

#include <cmath>
#include <cstring>

#include "../NodeInstance.h"
#include "../PresetParser.h"

namespace nodes::midiprogram
{
inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (!name || !std::isfinite(value)) return false;
    if (strcmp(name, "channel") == 0)
    {
        inst.midiProgram.channel = value >= 0 && value <= 15 && value == std::floor(value)
            ? static_cast<int8_t>(value) : 0;
        return true;
    }
    if (strcmp(name, "value") == 0)
    {
        inst.midiProgram.value = std::fmax(0.0f, std::fmin(127.0f, std::round(value)));
        return true;
    }
    return false;
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    setParam(inst, "channel", p.channel);
    setParam(inst, "value", p.constValue);
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    const float channel = presetparser::getFloat(p, "channel", 0);
    out.channel = std::isfinite(channel) && channel >= 0 && channel <= 15 && channel == std::floor(channel)
        ? static_cast<int8_t>(channel) : 0;
    out.constValue = presetparser::getFloat(p, "value", 0.0f);
}
} // namespace nodes::midiprogram
