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
    const float *source = inst.inPorts[0];
    if (source != nullptr)
    {
        memcpy(inst.outL, source, frames * sizeof(float));
    }
    else
    {
        memset(inst.outL, 0, frames * sizeof(float));
    }
}

inline void parseLinkDefinition(cJSON *p, WidgetParams &out)
{
    snprintf(out.linkId, sizeof(out.linkId), "%s", presetparser::getString(p, "linkId", ""));
    snprintf(out.linkName, sizeof(out.linkName), "%s", presetparser::getString(p, "linkName", ""));
}

inline void parseLinkSelection(cJSON *p, WidgetParams &out)
{
    snprintf(out.sourceLinkId, sizeof(out.sourceLinkId), "%s", presetparser::getString(p, "sourceLinkId", ""));
}

} // namespace nodes::linkedio
