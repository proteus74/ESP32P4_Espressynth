#pragma once

#include <atomic>
#include <cmath>

#include "../CvConventions.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"

// Graph participant - exists so the voiced/global BFS has a starting point and ControlRoutes
// can reference it. See Graph::handleMidiEvent for the actual note-on/off/CC routing.
// render() only reads its three control-only CV inputs (glide/glideTime/voices), so a patch
// can drive them standalone, without the browser. MidiInState stays in NodeState.h.
namespace nodes::midiin
{

// Voice count requested by a "voices" CV (0 = none pending). Written by the render task,
// consumed by SynthEngine's loader task, which rebuilds the current preset with it.
inline std::atomic<uint8_t> gVoicesCvRequest{0};

// A voice-count change means a graph rebuild (short audio dropout), so a CV value must stay
// on the same voice count this long before it is requested.
constexpr float kVoicesCvStableSeconds = 0.3f;

inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    auto &m = inst.midiIn;
    if (portIsFed(inst, 0)) m.glide = portSample(inst, 0, 0) >= cv::kGateThresholdVolts;
    if (portIsFed(inst, 1)) m.glideTime = clampToRange(portSample(inst, 1, 0), 0.0f, cv::kMaxVolts) * (2.0f / cv::kMaxVolts);
    if (!portIsFed(inst, 2)) return;

    // Same mapping as the browser: 0-10V -> 1..max voices, rounded.
    const float normalized = clampToRange(portSample(inst, 2, 0), 0.0f, cv::kMaxVolts) / cv::kMaxVolts;
    const uint8_t desired = static_cast<uint8_t>(lroundf(1.0f + normalized * static_cast<float>(SYNTH_VOICE_COUNT - 1)));
    if (desired != m.voicesCvCandidate)
    {
        m.voicesCvCandidate = desired;
        m.voicesCvStableBlocks = 0;
        return;
    }
    const uint32_t stableBlocksNeeded =
        static_cast<uint32_t>(kVoicesCvStableSeconds * static_cast<float>(synth::kSampleRate) / static_cast<float>(frames > 0 ? frames : 1));
    if (m.voicesCvStableBlocks < stableBlocksNeeded)
    {
        ++m.voicesCvStableBlocks;
        return;
    }
    if (desired != m.voices && desired != m.voicesCvRequested)
    {
        m.voicesCvRequested = desired;
        gVoicesCvRequest.store(desired, std::memory_order_release);
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    inst.midiIn.channel = p.channel;
    inst.midiIn.voices = p.voices;
    inst.midiIn.overflowMode = p.overflowMode;
    inst.midiIn.glide = p.glide;
    inst.midiIn.glideTime = p.glideTime;
    inst.midiIn.pitchBendRangeCents = p.midiInPitchBendRangeCents;
    inst.midiIn.pitchBendQuantizeCents = p.midiInPitchBendQuantizeCents;
    inst.midiIn.program = p.midiInProgram;
    memcpy(inst.midiIn.keyMask, p.midiInKeyMask, sizeof(inst.midiIn.keyMask));
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    cJSON *ch = p != nullptr ? cJSON_GetObjectItemCaseSensitive(p, "channel") : nullptr;
    out.channel = cJSON_IsNumber(ch) ? static_cast<int8_t>(ch->valueint) : -1;
    out.voices = static_cast<uint8_t>(presetparser::getInt(p, "voices", 1));
    out.overflowMode = overflowModeFromString(presetparser::getString(p, "overflowMode", "steal-oldest"));
    out.glide = presetparser::getBool(p, "glide", false);
    out.glideTime = presetparser::getFloat(p, "glideTime", 0.0f);
    const float bendRange = presetparser::getFloat(p, "pitchBendRangeCents", 200.0f);
    out.midiInPitchBendRangeCents = std::isfinite(bendRange) ? std::fmax(0.0f, std::fmin(6000.0f, bendRange)) : 200.0f;
    out.midiInPitchBendQuantizeCents = presetparser::getBool(p, "pitchBendQuantizeCents", false);
    const float program = presetparser::getFloat(p, "program", 0.0f);
    out.midiInProgram = std::isfinite(program) ? std::fmax(0.0f, std::fmin(127.0f, std::round(program))) : 0.0f;
    parseKeyMaskHex(presetparser::getString(p, "keyMask", ""), out.midiInKeyMask); // absent/invalid: all keys
}

} // namespace nodes::midiin
