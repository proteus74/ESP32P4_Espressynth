#pragma once

#include <cstddef>
#include <cstring>

#include "../NodeInstance.h"
#include "../PresetParser.h"

// Fixed scalar output (ConstState in NodeState.h) - no input ports.
namespace nodes::constant
{

inline void render(NodeInstance &inst, size_t frames)
{
    for (size_t i = 0; i < frames; ++i)
    {
        inst.outL[i] = inst.constant.value;
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    inst.constant.value = p.constValue;
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name != nullptr && strcmp(name, "value") == 0)
    {
        inst.constant.value = value;
        return true;
    }
    return false;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.constValue = presetparser::getFloat(p, "value", 0.0f);
}

} // namespace nodes::constant
