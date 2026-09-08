#pragma once

#include <cstddef>

#include "../NodeInstance.h"

// Samples "audio" (Port 0) once at the END of the block and holds that value
// for the whole block on its (control-rate) output - a rate-reducing bridge
// from an audio-rate signal to something a control target can consume.
// Stateless (no NodeState.h struct, no build/param/preset step needed).
namespace nodes::audiotovalue
{

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    const float value = frames > 0 ? portSample(inst, 0, frames - 1) : 0.0f;
    for (size_t i = 0; i < frames; ++i)
    {
        inst.outL[i] = value;
    }
}

} // namespace nodes::audiotovalue
