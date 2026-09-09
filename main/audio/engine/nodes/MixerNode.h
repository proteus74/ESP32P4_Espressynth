#pragma once

#include <cstddef>
#include <cstdio>
#include <cstring>

#include "../NodeInstance.h"
#include "../PresetParser.h"

// Configurable 2-8 input audio mixer (per-input gain + output gain).
// MixerState (value member) stays in NodeState.h.
namespace nodes::mixer
{

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    auto &s = inst.mixer;
    // Which inputs actually contribute (nonzero gain), resolved once per
    // block instead of reading+multiplying every port - including zero-
    // gain ones - on every sample (plan section 5, Mixer row: "Aktive
    // Eingänge und Gains einmal vorbereiten; Null-Gain-Eingänge nicht
    // lesen"). Same summation order as before (only zero-gain ports, whose
    // contribution is 0 regardless of the port's actual sample, are
    // skipped), so this is bit-identical for the common finite-signal case.
    uint8_t activePort[8];
    uint8_t activeCount = 0;
    for (int p = 0; p < inst.audioInPortCount; ++p)
    {
        if (s.inputGain[p] != 0.0f)
        {
            activePort[activeCount++] = static_cast<uint8_t>(p);
        }
    }
    const float outputGain = s.outputGain;
    for (size_t i = 0; i < frames; ++i)
    {
        float sum = 0.0f;
        for (uint8_t idx = 0; idx < activeCount; ++idx)
        {
            const int p = activePort[idx];
            sum += portSample(inst, p, i) * s.inputGain[p];
        }
        inst.outL[i] = sum * outputGain;
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    for (int i = 0; i < 8; ++i)
    {
        inst.mixer.inputGain[i] = p.mixerGain[i];
    }
    inst.mixer.outputGain = p.outputGain;
}

// Matches "gain1".."gain8" (1-based in the name, 0-based return), else -1.
// Deliberately separate from PortTables' indexedInPort("inN") - this is the
// *live-delivery* port-name convention used by main.js's deliverToWidget()
// for mixer gains, distinct from the "gains" array key used at
// full-preset-load time (see parsePreset below).
namespace
{
inline int gainIndex(const char *name)
{
    if (name == nullptr || strncmp(name, "gain", 4) != 0)
    {
        return -1;
    }
    const char *numPart = name + 4;
    if (numPart[0] < '1' || numPart[0] > '9' || numPart[1] != '\0')
    {
        return -1;
    }
    const int n = numPart[0] - '0';
    return (n >= 1 && n <= 8) ? (n - 1) : -1;
}
} // namespace

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    const int idx = gainIndex(name);
    if (idx >= 0)
    {
        inst.mixer.inputGain[idx] = value;
        return true;
    }
    return false;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    const int inputCount = presetparser::getInt(p, "inputCount", 2);
    out.inputCount = static_cast<uint8_t>(inputCount < 2 ? 2 : (inputCount > 8 ? 8 : inputCount));
    // main.js's WIDGET_ENGINES.mixer serializes gains as an 8-element array
    // "gains" (not individual "gain1".."gain8" keys), and never persists an
    // "outputGain" at all (audio-engine.js's DSP node supports one, but the
    // UI/preset schema doesn't expose it) - so outputGain just keeps its
    // 1.0f default here.
    cJSON *gains = p != nullptr ? cJSON_GetObjectItemCaseSensitive(p, "gains") : nullptr;
    if (cJSON_IsArray(gains))
    {
        int i = 0;
        cJSON *g;
        cJSON_ArrayForEach(g, gains)
        {
            if (i >= 8)
            {
                break;
            }
            out.mixerGain[i] = cJSON_IsNumber(g) ? static_cast<float>(g->valuedouble) : 1.0f;
            ++i;
        }
    }
}

} // namespace nodes::mixer
