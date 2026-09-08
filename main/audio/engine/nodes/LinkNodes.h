#pragma once

#include <cstddef>
#include <cstring>

#include "../NodeInstance.h"

// audio-output-link / audio-input-link: a page-boundary bridge pair, pure
// stereo pass-through (GraphBuilder wires the implicit link->link bridge
// edges directly). No state, no build/param/preset step needed.
namespace nodes::link
{

inline void render(NodeInstance &inst, size_t frames)
{
    auto copyChannel = [&](int port, float *output)
    {
        if (inst.inPorts[port] != nullptr)
        {
            memcpy(output, inst.inPorts[port], frames * sizeof(float));
            return;
        }
        const float value = inst.controlValue[port];
        if (value == 0.0f)
        {
            memset(output, 0, frames * sizeof(float));
            return;
        }
        for (size_t i = 0; i < frames; ++i)
        {
            output[i] = value;
        }
    };
    copyChannel(0, inst.outL);
    copyChannel(1, inst.outR);
}

} // namespace nodes::link
