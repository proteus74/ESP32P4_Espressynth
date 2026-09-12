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
        return;
    }
    // No real DSP edge feeds port 0 - e.g. a linked-input fed by a
    // ControlRoute-only source (MidiIn's "gate"/"velocity", a Knob, a
    // Button, ...) never writes inPorts[], only controlValue[] (see
    // Graph::deliverNoteControlRoutes/pushControlRouteValue). Falling
    // straight to silence here (the previous behavior) meant a MidiIn
    // "Gate" -> linked-input -> linked-output -> Adsr "Gate" chain never
    // worked, even though the exact same MidiIn -> Adsr "Gate" wire
    // connected directly works fine. Same portSample()-style fallback
    // nodes/LinkNodes.h's AudioOutputLink passthrough already uses.
    const float scalar = inst.controlValue[0];
    if (scalar == 0.0f)
    {
        memset(inst.outL, 0, frames * sizeof(float));
    }
    else
    {
        for (size_t i = 0; i < frames; ++i)
        {
            inst.outL[i] = scalar;
        }
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
