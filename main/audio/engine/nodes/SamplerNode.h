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

// Pitch-CV sampler: a gate rising edge plays [start, end] of a WAV - with the loop on,
// [loopStart, loopEnd] repeats for as long as the voice sounds. Gate off changes nothing by
// itself: the voice plays until amplitude*gain is back at 0 for a whole block (the ADSR's
// release has finished), or - without the loop - until `end`. Then it renders nothing until
// the next gate. The sample buffer is resolved at build time by GraphBuilder's initSampler()
// (same per-widget cache as WavePlayer).
namespace nodes::sampler
{
constexpr int kAmplitudePort = 0, kPitchCvPort = 1, kGatePort = 2, kGainPort = 3;
// Retrigger while still playing: the old position fades out over this many frames while the
// new note starts at `start` (no hard cut).
constexpr uint16_t kRetriggerFadeFrames = static_cast<uint16_t>(0.005f * synth::kSampleRate);

struct Frames
{
    uint32_t start, end, loopStart, loopEnd;
};

// Marker fractions -> frame indices, ordered defensively (live edits can arrive unordered).
inline Frames frames(const SamplerState &s)
{
    const uint32_t last = s.frameCount - 1;
    auto at = [last](float f) { return static_cast<uint32_t>(noderender::clampf(f, 0.0f, 1.0f) * static_cast<float>(last)); };
    Frames f;
    f.start = at(s.start);
    if (f.start >= last) f.start = last - 1;
    f.end = at(s.end);
    if (f.end <= f.start) f.end = f.start + 1;
    f.loopStart = at(s.loopStart);
    if (f.loopStart < f.start) f.loopStart = f.start;
    if (f.loopStart >= f.end) f.loopStart = f.end - 1;
    f.loopEnd = at(s.loopEnd);
    if (f.loopEnd > f.end) f.loopEnd = f.end;
    if (f.loopEnd <= f.loopStart) f.loopEnd = f.loopStart + 1;
    return f;
}

// Linear interpolation between frame `index` and the next one (channel 0 = L, 1 = R).
inline float readFrame(const SamplerState &s, uint32_t index, float frac, int channel)
{
    const float a = s.sampleData[index * 2 + channel] / 32768.0f;
    const float b = s.sampleData[(index + 1) * 2 + channel] / 32768.0f;
    return a + (b - a) * frac;
}

// Moves a read position on by `ratio` frames, wrapping in the loop. False once it runs past
// `end` (not looping).
inline bool advance(const SamplerState &s, uint32_t &index, float &frac, float ratio, const Frames &f, uint32_t loopLen)
{
    frac += ratio;
    const uint32_t step = static_cast<uint32_t>(frac);
    index += step;
    frac -= static_cast<float>(step);
    if (s.loop)
    {
        while (index >= f.loopEnd) index -= loopLen;
        return true;
    }
    return index < f.end;
}

// Gate rising edge / MIDI note-on (Graph::deliverNoteControlRoutes).
inline void trigger(SamplerState &s)
{
    s.fadeRemaining = s.playing ? kRetriggerFadeFrames : 0;
    s.fadeIndex = s.index;
    s.fadeFrac = s.frac;
    s.playing = s.sampleData != nullptr && s.frameCount > 2;
    if (!s.playing) return;
    s.index = frames(s).start;
    s.frac = 0.0f;
    s.heard = false;
}

inline void render(NodeInstance &inst, size_t frameCount)
{
    using namespace noderender;
    auto &s = inst.sampler;
    inst.outIsConstant = false;
    const bool gateNow = portSample(inst, kGatePort, 0) > cv::kGateThresholdVolts;
    if (gateNow && !s.gatePrev) trigger(s);
    s.gatePrev = gateNow;

    if (!s.playing)
    {
        memset(inst.outL, 0, frameCount * sizeof(float));
        memset(inst.outR, 0, frameCount * sizeof(float));
        inst.outIsConstant = true; // both channels are 0
        inst.outConstantValue = 0.0f;
        return;
    }

    const Frames f = frames(s);
    const float pitchCv = portIsFed(inst, kPitchCvPort) ? portSample(inst, kPitchCvPort, 0) : cv::kMaxVolts * 0.5f;
    const float ratio = dsp::pow2Lookup((pitchCv - cv::kMaxVolts * 0.5f) + (60.0f - static_cast<float>(s.rootNoteMidi)) / 12.0f);
    const bool ampFed = portIsFed(inst, kAmplitudePort);
    const bool gainFed = (inst.audioConnectedPortMask & synth::portBit(kGainPort)) != 0;
    const float gainTarget = cv::vcaGain(s.gainCv, false);
    const uint32_t loopLen = f.loopEnd - f.loopStart;
    float peakGain = 0.0f;

    size_t i = 0;
    for (; i < frameCount; ++i)
    {
        float sampleL = readFrame(s, s.index, s.frac, 0);
        float sampleR = readFrame(s, s.index, s.frac, 1);
        if (s.fadeRemaining > 0)
        {
            const float g = static_cast<float>(s.fadeRemaining) / static_cast<float>(kRetriggerFadeFrames);
            sampleL += (readFrame(s, s.fadeIndex, s.fadeFrac, 0) - sampleL) * g;
            sampleR += (readFrame(s, s.fadeIndex, s.fadeFrac, 1) - sampleR) * g;
            --s.fadeRemaining;
            if (!advance(s, s.fadeIndex, s.fadeFrac, ratio, f, loopLen)) s.fadeRemaining = 0;
        }
        const float gainMul = gainFed ? cv::vcaGain(portSample(inst, kGainPort, i), false) : s.gainSmooth.process(gainTarget);
        // Unlike WavePlayer, amplitude is a 0-10 V CV (vcaGain), like Oscillator - an ADSR
        // wired here spans exactly 0..1. Unconnected plays at full level.
        const float level = (ampFed ? cv::vcaGain(portSample(inst, kAmplitudePort, i), false) : 1.0f) * gainMul;
        if (level > peakGain) peakGain = level;
        const float amp = level * cv::kAudioPeakVolts;
        inst.outL[i] = sampleL * amp;
        inst.outR[i] = sampleR * amp;

        if (!advance(s, s.index, s.frac, ratio, f, loopLen)) s.playing = false;

        if (!s.playing)
        {
            ++i;
            break;
        }
    }
    if (i < frameCount)
    {
        memset(inst.outL + i, 0, (frameCount - i) * sizeof(float));
        memset(inst.outR + i, 0, (frameCount - i) * sizeof(float));
    }
    // Amplitude*gain back at 0 for the whole block: stop reading. Right after a gate an ADSR
    // may still sit at 0 (slow attack), so with the gate held this only counts once the voice
    // has been audible.
    constexpr float kSilentLevel = 1e-4f;
    if (peakGain > kSilentLevel) s.heard = true;
    else if (s.heard || !gateNow) s.playing = false;
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    auto &s = inst.sampler;
    s.rootNoteMidi = p.rootNoteMidi;
    s.start = p.samplerStart;
    s.end = p.samplerEnd;
    s.loopStart = p.samplerLoopStart;
    s.loopEnd = p.samplerLoopEnd;
    s.loop = p.samplerLoop;
    s.gainCv = p.gain;
    s.playing = false;
    s.gatePrev = false;
    s.gainSmooth.setCutoff(300.0f, static_cast<float>(synth::kSampleRate));
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    auto &s = inst.sampler;
    if (strcmp(name, "gain") == 0) s.gainCv = value;
    else if (strcmp(name, "start") == 0) s.start = value;
    else if (strcmp(name, "end") == 0) s.end = value;
    else if (strcmp(name, "loopStart") == 0) s.loopStart = value;
    else if (strcmp(name, "loopEnd") == 0) s.loopEnd = value;
    else if (strcmp(name, "loop") == 0) s.loop = value >= 0.5f;
    else if (strcmp(name, "rootNoteMidi") == 0) s.rootNoteMidi = static_cast<int>(value + 0.5f);
    else return false;
    return true;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    snprintf(out.sampleFile, sizeof(out.sampleFile), "%s", presetparser::getString(p, "sampleFile", ""));
    out.rootNoteMidi = presetparser::getInt(p, "rootNoteMidi", 60);
    out.gain = presetparser::getFloat(p, "gain", 10.0f);
    out.samplerStart = presetparser::getFloat(p, "start", 0.0f);
    out.samplerEnd = presetparser::getFloat(p, "end", 1.0f);
    out.samplerLoopStart = presetparser::getFloat(p, "loopStart", 0.0f);
    out.samplerLoopEnd = presetparser::getFloat(p, "loopEnd", 1.0f);
    out.samplerLoop = presetparser::getBool(p, "loop", false);
}

} // namespace nodes::sampler
