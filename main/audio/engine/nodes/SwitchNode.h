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
    for (size_t i = 0; i < frames; ++i)
    {
        inst.outL[i] = inst.switchState.selectIn2 ? portSample(inst, 1, i) : portSample(inst, 0, i);
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
