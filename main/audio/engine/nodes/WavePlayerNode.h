#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../CvConventions.h"
#include "../DspPrimitives.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"

// Per-voice one-shot WAV sample playback. WavePlayerState (value member,
// see NodeState.h) points into a widget-owned buffer decoded ONCE at graph-
// build time - see GraphBuilder.cpp's initWavePlayer(), which stays there
// (not here) since it needs Graph& for the per-widget WaveSampleCache, a
// shape distinct from every other node's (NodeInstance&, WidgetParams&)
// build step.
namespace nodes::waveplayer
{

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    const float kSampleRate = static_cast<float>(synth::kSampleRate);
    auto &s = inst.wavePlayer;
    const bool gateNow = portSample(inst, 2, 0) > cv::kGateThresholdVolts;
    if (gateNow && !s.gatePrev)
    {
        s.position = 0.0f;
        s.playing = (s.sampleData != nullptr && s.frameCount > 1);
        s.fadeGain = 1.0f;
    }
    s.gatePrev = gateNow;

    if (!s.playing)
    {
        memset(inst.outL, 0, frames * sizeof(float));
        memset(inst.outR, 0, frames * sizeof(float));
        return;
    }

    const float ratio = dsp::pow2Lookup(
        (portSample(inst, 1, 0) - static_cast<float>(s.rootNoteMidi)) / 12.0f);
    const bool ampConnected = controlPortConnected(inst, 0) ||
                              (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 0)) != 0;
    const uint32_t lastFrame = s.frameCount - 1;
    // 20ms linear release fade - see WavePlayerState::fadeGain.
    const float fadeStep = 1.0f / (0.02f * kSampleRate);

    for (size_t i = 0; i < frames; ++i)
    {
        if (!s.playing)
        {
            inst.outL[i] = 0.0f;
            inst.outR[i] = 0.0f;
            continue;
        }

        if (gateNow)
        {
            s.fadeGain = 1.0f;
        }
        else if (s.fadeGain > 0.0f)
        {
            s.fadeGain -= fadeStep;
            if (s.fadeGain < 0.0f)
            {
                s.fadeGain = 0.0f;
            }
        }

        const uint32_t idx0 = static_cast<uint32_t>(s.position);
        const float frac = s.position - static_cast<float>(idx0);
        const uint32_t idx1 = idx0 + 1 > lastFrame ? lastFrame : idx0 + 1;
        const float l0 = s.sampleData[idx0 * 2] / 32768.0f;
        const float l1 = s.sampleData[idx1 * 2] / 32768.0f;
        const float r0 = s.sampleData[idx0 * 2 + 1] / 32768.0f;
        const float r1 = s.sampleData[idx1 * 2 + 1] / 32768.0f;
        // Amplitude unconnected plays back at full volume (1.0) - unlike
        // Oscillator's "unconnected = silence" convention, a bare sample
        // player should sound at all without needing a Const(1) wired in
        // just to hear it. When connected, the value is used directly, NOT
        // added to a fixed baseline: an ADSR wired here must be able to
        // fade from true silence (envelope 0..1 IS the amplitude), not
        // stack on top of an already-audible constant. fadeGain multiplies
        // on top either way, so gate-off never clicks.
        const float amp = (ampConnected ? portSample(inst, 0, i) : 1.0f) * s.fadeGain;
        inst.outL[i] = (l0 + (l1 - l0) * frac) * amp;
        inst.outR[i] = (r0 + (r1 - r0) * frac) * amp;

        s.position += ratio;
        if (s.position >= static_cast<float>(lastFrame))
        {
            s.playing = false; // one-shot: stop at end, no loop, no retrigger until next gate edge
        }
    }
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    snprintf(out.sampleFile, sizeof(out.sampleFile), "%s", presetparser::getString(p, "sampleFile", ""));
    out.rootNoteMidi = presetparser::getInt(p, "rootNoteMidi", 60);
}

} // namespace nodes::waveplayer
