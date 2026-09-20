#pragma once

#include <cstddef>
#include <cstring>

#include "../NodeInstance.h"
#include "../PresetParser.h"

// linked-input / linked-output: freely-placeable, linkId-keyed passthrough
// links. Unrelated to nodes/LinkNodes.h's fixed 8-bus audio-output-link /
// audio-input-link mechanism - do not merge with it (different shape: one
// mono port each, no LinkState, no gain stage, no bus loop). GraphBuilder
// resolves the hidden linked-input -> linked-output edge at build time
// (see GraphBuilder.h) and lands it on inPorts[0] for both types; render()
// just needs to copy that hidden port to the visible outL.
namespace nodes::linkedio
{

inline void render(NodeInstance &inst, size_t frames)
{
    const uint8_t count = inst.link != nullptr ? inst.link->activeBusCount : 1;
    for (uint8_t port = 0; port < count; ++port)
    {
        float *output = outputPortBuffer(inst, port);
        const float *source = inst.inPorts[port];
        if (source != nullptr)
            memcpy(output, source, frames * sizeof(float));
        else if (inst.controlValue[port] == 0.0f)
            memset(output, 0, frames * sizeof(float));
        else
        {
            for (size_t i = 0; i < frames; ++i) output[i] = inst.controlValue[port];
        }
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    if (inst.link != nullptr) inst.link->activeBusCount = p.linkedIoCount;
}

inline void parseLinkDefinition(cJSON *p, WidgetParams &out)
{
    snprintf(out.linkId, sizeof(out.linkId), "%s", presetparser::getString(p, "linkId", ""));
    snprintf(out.linkName, sizeof(out.linkName), "%s", presetparser::getString(p, "linkName", ""));
    const int count = presetparser::getInt(p, "linkCount", 1);
    out.linkedIoCount = static_cast<uint8_t>(count == 8 ? 8 : (count == 4 ? 4 : 1));
}

inline void parseLinkSelection(cJSON *p, WidgetParams &out)
{
    snprintf(out.sourceLinkId, sizeof(out.sourceLinkId), "%s", presetparser::getString(p, "sourceLinkId", ""));
    const int count = presetparser::getInt(p, "linkCount", 1);
    out.linkedIoCount = static_cast<uint8_t>(count == 8 ? 8 : (count == 4 ? 4 : 1));
}

} // namespace nodes::linkedio
