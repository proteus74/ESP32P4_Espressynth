#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "esp_heap_caps.h"

#include "../CvConventions.h"
#include "../DspPrimitives.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"

// Hammond-B3-style tonewheel organ: nine drawbar registers, each a pure sine
// at its own (user-editable) harmonic ratio of the played pitch, weighted by
// its own (also independently CV-controllable) level and summed. Port shape
// mirrors Oscillator (nodes/OscillatorNode.h) one-to-one minus its
// "waveform" port - a B3's nine sines are not a user-selectable waveform,
// and the nine levels take that role instead. Heap-allocated per instance
// (see NodeInstance.h's pointer-member comment): drawbarCv[9] +
// drawbarSmooth[9] alone is as large as most whole state structs in
// NodeState.h, and only an actual Hammond instance ever uses it.
namespace nodes::hammond
{

constexpr int kDrawbarCount = 9;
constexpr int kAmplitudePort = 0, kPitchCvPort = 1, kTunePort = 2, kGainPort = 3, kDrawbar1Port = 4;
static_assert(kDrawbar1Port + kDrawbarCount <= static_cast<int>(synth::kMaxAudioInPorts), "fits port budget");
constexpr float kPhaseUnitsPerHz = 4294967296.0f / static_cast<float>(synth::kSampleRate);

inline uint32_t phaseStep(float hz, float ratio)
{
    const float scaled = hz * ratio * kPhaseUnitsPerHz;
    return static_cast<uint32_t>(static_cast<uint64_t>(scaled));
}

// Linearly interpolated: a plain truncated 4096-entry lookup leaves a phase-
// truncation error of up to ~-56 dB per partial as inharmonic spurs - summed
// over several drawbars that is an audible buzz under the tone.
inline float sineLookup(uint32_t phase)
{
    return dsp::sineLookupInt(phase);
}

// Classic Hammond drawbar footages' harmonic ratio relative to the played
// pitch (16', 5-1/3', 8', 4', 2-2/3', 2', 1-3/5', 1-1/3', 1') - the
// SETTINGS-DIALOG DEFAULT, not a fixed per-instance value: each drawbar's
// ratio is user-editable (HammondState::ratio[k] below); a "Default" button
// in the widget's settings dialog resets to exactly this table after a
// confirm() prompt (see main.js).
constexpr float kDefaultRatio[kDrawbarCount] = {0.5f, 1.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f};
// Power normalization (1/sqrt(9)) rather than worst-case peak (1/9): a classic
// three-drawbar registration (888000000) then peaks at cv::kAudioPeakVolts like
// a full-scale Oscillator. Fuller registrations can exceed it; the output
// stage's SoftLimiter (OutputNodes.h) catches that, as for summed voices.
constexpr float kDrawbarNorm = cv::kAudioPeakVolts / 3.0f;

struct HammondState
{
    // Nine INDEPENDENT phase accumulators, each [0,1), each advanced at its
    // own rate hz*ratio[k]. Independent (not one shared accumulator) because
    // ratios are user-editable to arbitrary values: sineLookup wraps at
    // phase=1, and unless a ratio is an exact integer relative to a shared
    // accumulator's own rate, the wrap would introduce an audible phase
    // discontinuity once per fundamental period. Nine independent
    // accumulators are continuous by construction for ANY ratio, with no
    // special-casing. Free-running, NEVER reset on note-on (a real B3's
    // tonewheels spin continuously; the random phase relationship at
    // key-down is part of the sound).
    uint32_t phase[kDrawbarCount] = {};
    // Per-drawbar harmonic ratio relative to the played pitch. User-editable
    // via the settings dialog; "ratio1".."ratio9" is also a live-param/
    // preset key (see setParam/parsePreset below) - NOT a port, independent
    // of the "drawbarN" CV level ports.
    float ratio[kDrawbarCount] = {0.5f, 1.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f};
    // Same 0-10V pitch-CV domain and substitute-not-add semantics as
    // OscillatorState::tuneVolts.
    float tuneVolts = 5.0f;
    // Written directly by a MIDI-velocity ControlRoute (Graph.h), exactly
    // like OscillatorState::amplitude, and only consulted when a route
    // actually targets port 0 (see render()).
    float amplitude = 1.0f;
    // "gain" (port 3): 0-10V CV, 10V = unchanged. Identical convention and
    // per-sample smoothing pattern to OscillatorState::gainCv/gainSmooth.
    float gainCv = 10.0f;
    OnePoleLowpass gainSmooth;
    // Each drawbar's LEVEL (ports 4-12): same contract as OscillatorState::
    // gainCv/gainSmooth, nine times over - 0-10V CV via cv::vcaGain; the
    // scalar is only consulted when that port has no audio-rate edge, in
    // which case it's smoothed per sample to avoid a block-constant jump.
    // Default = classic 888000000 registration.
    float drawbarCv[kDrawbarCount] = {10.0f, 10.0f, 10.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    OnePoleLowpass drawbarSmooth[kDrawbarCount];
    // Audio-rate pitch/tune/amplitude inputs are sampled every Nth frame.
    // 1 = sample-accurate; 2/4/8/16 trade modulation resolution for CPU.
    uint8_t modulationStride = 4;
};

// One drawbar for one block, frequency constant within the block: a complex
// phasor rotated by the per-sample increment, weighted by gainAt(i) and ADDED
// into out[]. Replaces sineLookup() per drawbar-sample (two table loads plus
// index/fraction arithmetic) by four multiplies and no memory traffic beyond
// out[] itself; drawbar-outer so phasor, rotation and gain stay in registers
// instead of being re-fetched through activeDrawbar[]/phase[]/dbBuf[] per
// sample. Start angle and rotation come from the LUT;
// float error over one block stays below 1e-4 (angle and magnitude) and the
// integer phase the caller keeps advancing re-seeds the phasor next block,
// so nothing accumulates. Same sample alignment as before: out[i] holds
// sin(phase + (i + 1) * increment), the phase accumulator advances first.
// sineLookup plus the lerp's second-order term: linear interpolation sits
// below sine's arc by sin(x)/2 * frac * (1 - frac) * h^2 (h = table step), so
// scaling the lerp back up leaves ~1e-9 instead of ~3e-7. Only for the per-
// block rotation coefficients, whose error compounds over the block (~4x
// lower block error than plain sineLookup, measured in simulation).
inline float rotationCoefficient(uint32_t angle)
{
    constexpr float kStep = 2.0f * kDspPi / static_cast<float>(synth::kSineLutSize);
    const float frac = static_cast<float>(angle & ((1u << dsp::kSineLutFracBits) - 1)) * dsp::kSineLutFracScale;
    return sineLookup(angle) * (1.0f + 0.5f * frac * (1.0f - frac) * (kStep * kStep));
}

template <typename GainAt>
IRAM_ATTR inline void accumulateDrawbar(float *out, size_t frames, uint32_t phase, uint32_t increment, GainAt gainAt)
{
    constexpr uint32_t kQuarterTurn = 0x40000000u;
    float im = sineLookup(phase);
    float re = sineLookup(phase + kQuarterTurn);
    const float s = rotationCoefficient(increment);
    const float c = rotationCoefficient(increment + kQuarterTurn);
    for (size_t i = 0; i < frames; ++i)
    {
        const float nextRe = re * c - im * s;
        im = re * s + im * c;
        re = nextRe;
        out[i] += im * gainAt(i);
    }
}

// IRAM_ATTR: same reason as Oscillator's render() (see DspPrimitives.h's sineLookup()
// comment). Measured on CrowPanel 5": from flash, a 5-voice Hammond block took ~1.0 ms
// median but up to ~2.9 ms whenever display/Wi-Fi traffic evicted it from the shared L2
// cache - the source of the Audio-Overruns while turning MIDI knobs.
IRAM_ATTR inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    constexpr float kCenterVolts = cv::kMaxVolts * 0.5f;
    constexpr float kSilentGain = 1e-4f;
    auto &s = *inst.hammond;

    const bool pitchAudioFed = (inst.audioConnectedPortMask & synth::portBit(kPitchCvPort)) != 0;
    const bool pitchControlRamping = (inst.controlRampPortMask & synth::portBit(kPitchCvPort)) != 0;
    const bool pitchControlFed = controlPortConnected(inst, kPitchCvPort);
    const bool tuneAudioFed = (inst.audioConnectedPortMask & synth::portBit(kTunePort)) != 0;
    // Same constant-for-block detection as Oscillator (OscillatorNode.h) -
    // plan section 2 explicitly calls out that Hammond lacked this. A held
    // note through e.g. a Frequency Divider still produces one repeated CV
    // value all block; recomputing nine hz*ratio phase steps from it every
    // sample (or even every modulationStride-th sample) is pure waste when
    // it never actually changes.
    float constantPitchCv = kCenterVolts;
    const bool pitchAudioConstant = pitchAudioFed && audioPortConstantForBlock(inst, kPitchCvPort, frames, constantPitchCv);
    const bool pitchNeedsAudioRate = (pitchAudioFed && !pitchAudioConstant) || pitchControlRamping;
    float constantTuneVolts = s.tuneVolts;
    const bool tuneAudioConstant = tuneAudioFed && audioPortConstantForBlock(inst, kTunePort, frames, constantTuneVolts);
    const bool tuneNeedsAudioRate = tuneAudioFed && !tuneAudioConstant;
    const bool amplitudeAudioFed = (inst.audioConnectedPortMask & synth::portBit(kAmplitudePort)) != 0;
    float constantAmplitude = 0.0f;
    const bool amplitudeConstant = amplitudeAudioFed && audioPortConstantForBlock(inst, kAmplitudePort, frames, constantAmplitude);
    const bool amplitudeNeedsAudioRate = amplitudeAudioFed && !amplitudeConstant;
    const bool gainAudioFed = (inst.audioConnectedPortMask & synth::portBit(kGainPort)) != 0;
    float constantGain = 0.0f;
    const bool gainConstant = gainAudioFed && audioPortConstantForBlock(inst, kGainPort, frames, constantGain);
    const bool gainFed = gainAudioFed && !gainConstant;
    const float gainTarget = cv::vcaGain(gainConstant ? constantGain : s.gainCv, false);
    const bool gainSmoothing = !gainAudioFed && fabsf(s.gainSmooth.current() - gainTarget) > kSilentGain;

    // Node-level silence fast path - same click-safe pattern as Oscillator's
    // gain==0 case (see OscillatorNode.h): gated on the CONVERGED smoothed
    // value, not the raw target, so a knob turned down this block still
    // fades through the normal per-sample path below - never a click.
    if (!gainAudioFed && gainTarget <= 0.0f && s.gainSmooth.current() <= kSilentGain)
    {
        memset(inst.outL, 0, frames * sizeof(float));
        return;
    }

    // Per-drawbar port resolution, hoisted once per block (not per sample).
    const float *dbBuf[kDrawbarCount];
    float dbTarget[kDrawbarCount];
    bool dbActive[kDrawbarCount];
    bool dbSmoothing[kDrawbarCount];
    uint8_t activeDrawbar[kDrawbarCount];
    uint8_t activeDrawbarCount = 0;
    for (int k = 0; k < kDrawbarCount; ++k)
    {
        const int port = kDrawbar1Port + k;
        const bool fed = (inst.audioConnectedPortMask & synth::portBit(port)) != 0;
        dbBuf[k] = fed ? inst.inPorts[port] : nullptr;
        dbTarget[k] = cv::vcaGain(s.drawbarCv[k], false);
        float constantDrawbar = 0.0f;
        if (fed && audioPortConstantForBlock(inst, port, frames, constantDrawbar))
        {
            dbBuf[k] = nullptr;
            dbTarget[k] = cv::vcaGain(constantDrawbar, false);
        }
        dbActive[k] = (dbBuf[k] != nullptr) || dbTarget[k] > 0.0f || s.drawbarSmooth[k].current() > kSilentGain;
        dbSmoothing[k] = !fed && fabsf(s.drawbarSmooth[k].current() - dbTarget[k]) > kSilentGain;
        if (dbActive[k])
        {
            activeDrawbar[activeDrawbarCount++] = static_cast<uint8_t>(k);
        }
    }

    const float baseAmp = (controlPortConnected(inst, kAmplitudePort) ? s.amplitude : 0.0f) +
                         (amplitudeConstant ? cv::vcaGain(constantAmplitude, false) : 0.0f);

    // Frequency modulated at audio rate (pitch/tune cable or MIDI glide): the
    // nine phase steps change per modulationStride samples, so this stays the
    // sample-outer integer-phase loop. Rare for an organ - the usual case
    // (pitch from Midi-In, tune static) takes the constant-frequency path
    // below, which also covers an audio-rate amplitude (ADSR) since that is a
    // per-sample multiply after the drawbar sum, not a per-drawbar cost.
    const bool frequencyModulated = pitchNeedsAudioRate || tuneNeedsAudioRate;
    if (frequencyModulated)
    {
        const size_t modulationStride = static_cast<size_t>(clampf(static_cast<float>(s.modulationStride), 1.0f, 16.0f));
        size_t nextModulationSample = 0;
        float amp = baseAmp;
        uint32_t phaseIncrement[kDrawbarCount] = {};
        for (size_t i = 0; i < frames; ++i)
        {
            if (i == nextModulationSample)
            {
                nextModulationSample += modulationStride;
                const float pitchCv = pitchNeedsAudioRate ? portSample(inst, kPitchCvPort, i)
                                     : (pitchAudioFed ? constantPitchCv : (pitchControlFed ? portSample(inst, kPitchCvPort, 0) : kCenterVolts));
                const float tuneVolts = tuneNeedsAudioRate ? portSample(inst, kTunePort, i)
                                       : (tuneAudioFed ? constantTuneVolts : s.tuneVolts);
                const float hz = cv::hzFromPitchCv(pitchCv + (tuneVolts - kCenterVolts));
                for (int k = 0; k < kDrawbarCount; ++k)
                {
                    phaseIncrement[k] = phaseStep(hz, s.ratio[k]);
                }
                amp = baseAmp +
                      (amplitudeNeedsAudioRate ? cv::vcaGain(portSample(inst, kAmplitudePort, i), false) : 0.0f);
            }

            // Every drawbar's phase accumulator always advances (continuous
            // by construction for ANY ratio, including one just edited mid-
            // note - see HammondState::phase's comment), regardless of
            // whether it's currently active - cheap (one integer add each).
            for (int k = 0; k < kDrawbarCount; ++k)
            {
                s.phase[k] += phaseIncrement[k];
            }

            float sum = 0.0f;
            for (uint8_t activeIndex = 0; activeIndex < activeDrawbarCount; ++activeIndex)
            {
                const int k = activeDrawbar[activeIndex];
                if (dbBuf[k] != nullptr)
                {
                    // Live cable: CV arrives per sample, like Oscillator's
                    // wired "gain" - never skipped, regardless of level.
                    sum += sineLookup(s.phase[k]) * cv::vcaGain(dbBuf[k][i], false);
                }
                else
                {
                    const float drawbarGain = dbSmoothing[k] ? s.drawbarSmooth[k].process(dbTarget[k]) : dbTarget[k];
                    sum += sineLookup(s.phase[k]) * drawbarGain;
                }
            }

            const float gainMul =
                gainFed ? cv::vcaGain(portSample(inst, kGainPort, i), false)
                        : (gainSmoothing ? s.gainSmooth.process(gainTarget) : gainTarget);
            inst.outL[i] = sum * kDrawbarNorm * amp * gainMul;
        }
        return;
    }

    const float pitchCv = pitchAudioFed ? constantPitchCv : (pitchControlFed ? portSample(inst, kPitchCvPort, 0) : kCenterVolts);
    const float tuneVoltsConst = tuneAudioFed ? constantTuneVolts : s.tuneVolts;
    const float hz = cv::hzFromPitchCv(pitchCv + (tuneVoltsConst - kCenterVolts));
    uint32_t phaseIncrement[kDrawbarCount];
    for (int k = 0; k < kDrawbarCount; ++k)
    {
        phaseIncrement[k] = phaseStep(hz, s.ratio[k]);
    }
    // A silent amplitude block still advances tonewheels and manual smoothers.
    // Do not freeze their state or skip a pitch-modulated block.
    if (!amplitudeNeedsAudioRate && baseAmp == 0.0f)
    {
        for (int k = 0; k < kDrawbarCount; ++k)
        {
            s.phase[k] += phaseIncrement[k] * static_cast<uint32_t>(frames);
            if (dbSmoothing[k])
                for (size_t i = 0; i < frames; ++i)
                    s.drawbarSmooth[k].process(dbTarget[k]);
        }
        if (gainSmoothing)
            for (size_t i = 0; i < frames; ++i)
                s.gainSmooth.process(gainTarget);
        memset(inst.outL, 0, frames * sizeof(float));
        return;
    }

    // Drawbar sum first (drawbar-outer, see accumulateDrawbar), then one pass
    // for amplitude, gain and normalization.
    memset(inst.outL, 0, frames * sizeof(float));
    for (uint8_t activeIndex = 0; activeIndex < activeDrawbarCount; ++activeIndex)
    {
        const int k = activeDrawbar[activeIndex];
        if (dbBuf[k] != nullptr)
        {
            // Live cable: CV arrives per sample, like Oscillator's wired
            // "gain" - never skipped, regardless of level.
            const float *cable = dbBuf[k];
            accumulateDrawbar(inst.outL, frames, s.phase[k], phaseIncrement[k],
                              [cable](size_t i) { return cv::vcaGain(cable[i], false); });
        }
        else if (dbSmoothing[k])
        {
            OnePoleLowpass &smooth = s.drawbarSmooth[k];
            const float target = dbTarget[k];
            accumulateDrawbar(inst.outL, frames, s.phase[k], phaseIncrement[k],
                              [&smooth, target](size_t) { return smooth.process(target); });
        }
        else
        {
            const float drawbarGain = dbTarget[k];
            accumulateDrawbar(inst.outL, frames, s.phase[k], phaseIncrement[k],
                              [drawbarGain](size_t) { return drawbarGain; });
        }
    }
    for (int k = 0; k < kDrawbarCount; ++k)
    {
        s.phase[k] += phaseIncrement[k] * static_cast<uint32_t>(frames);
    }

    const float norm = kDrawbarNorm * baseAmp;
    if (!amplitudeNeedsAudioRate && !gainFed && !gainSmoothing)
    {
        const float scale = norm * gainTarget;
        for (size_t i = 0; i < frames; ++i)
        {
            inst.outL[i] *= scale;
        }
        return;
    }
    const float *ampPort = amplitudeNeedsAudioRate ? inst.inPorts[kAmplitudePort] : nullptr;
    const float *gainPort = gainFed ? inst.inPorts[kGainPort] : nullptr;
    for (size_t i = 0; i < frames; ++i)
    {
        const float amp = ampPort != nullptr ? kDrawbarNorm * (baseAmp + cv::vcaGain(ampPort[i], false)) : norm;
        const float gainMul =
            gainPort != nullptr ? cv::vcaGain(gainPort[i], false)
                                : (gainSmoothing ? s.gainSmooth.process(gainTarget) : gainTarget);
        inst.outL[i] *= amp * gainMul;
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    auto &s = *inst.hammond;
    s.tuneVolts = p.tuneVolts;
    s.amplitude = p.amplitude;
    s.gainCv = p.gain;
    s.modulationStride = p.hammondModulationStride;
    // Same cutoff/reason as Oscillator's and Gain's gainSmooth - see NodeState.h.
    s.gainSmooth.setCutoff(300.0f, static_cast<float>(synth::kSampleRate));
    for (int k = 0; k < kDrawbarCount; ++k)
    {
        s.drawbarCv[k] = p.hammondDrawbar[k];
        s.ratio[k] = p.hammondRatio[k];
        // Slower than gainSmooth (~8 ms vs ~0.5 ms): a fast-turned MIDI knob jumps a drawbar
        // 10-20 CC steps per message, and a 0.5 ms ramp over such a jump clicks audibly.
        s.drawbarSmooth[k].setCutoff(20.0f, static_cast<float>(synth::kSampleRate));
    }
}

inline void destroy(NodeInstance &inst)
{
    // No sub-buffers to release (unlike ChorusNode's delay lines) - the
    // state block itself is the whole allocation.
    if (inst.hammond != nullptr)
    {
        heap_caps_free(inst.hammond);
        inst.hammond = nullptr;
    }
}

namespace
{
// Matches "drawbar1".."drawbar9" (level, a real CV port name) or
// "ratio1".."ratio9" (harmonic ratio, settings-dialog-only, no port) -
// returns the drawbar index 0-8 and which field via `isRatio`, or -1.
inline int hammondFieldIndex(const char *name, bool &isRatio)
{
    if (name == nullptr)
    {
        return -1;
    }
    if (strncmp(name, "drawbar", 7) == 0)
    {
        const char *n = name + 7;
        if (n[0] < '1' || n[0] > '9' || n[1] != '\0')
        {
            return -1;
        }
        isRatio = false;
        return n[0] - '1';
    }
    if (strncmp(name, "ratio", 5) == 0)
    {
        const char *n = name + 5;
        if (n[0] < '1' || n[0] > '9' || n[1] != '\0')
        {
            return -1;
        }
        isRatio = true;
        return n[0] - '1';
    }
    return -1;
}
} // namespace

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr)
    {
        return false;
    }
    bool isRatio = false;
    const int d = hammondFieldIndex(name, isRatio);
    if (d >= 0)
    {
        if (isRatio)
        {
            inst.hammond->ratio[d] = value; // settings-dialog field, not a port
        }
        else
        {
            // "drawbarN" CV port, 0-10V - clamped here rather than trusted
            // from the wire: a Knob wired straight to this port delivers
            // 0-10V already, so this is really just belt-and-suspenders.
            inst.hammond->drawbarCv[d] = value;
        }
        return true;
    }
    if (strcmp(name, "tune") == 0)
    {
        inst.hammond->tuneVolts = value;
        return true;
    }
    if (strcmp(name, "amplitude") == 0)
    {
        inst.hammond->amplitude = value;
        return true;
    }
    if (strcmp(name, "gain") == 0)
    {
        inst.hammond->gainCv = value;
        return true;
    }
    if (strcmp(name, "modulationStride") == 0)
    {
        const int clamped = static_cast<int>(value) < 1 ? 1 : (static_cast<int>(value) > 16 ? 16 : static_cast<int>(value));
        inst.hammond->modulationStride = static_cast<uint8_t>(clamped);
        return true;
    }
    return false;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    // Same keys/defaults as Oscillator's (main.js's WIDGET_ENGINES.hammond
    // serializes Tune under "tune" in the 0-10V pitch-CV domain, and shares
    // the "gain"/"amplitude" conventions).
    out.tuneVolts = presetparser::getFloat(p, "tune", 5.0f);
    out.amplitude = presetparser::getFloat(p, "amplitude", 1.0f);
    out.gain = presetparser::getFloat(p, "gain", 10.0f);
    // "drawbars" - nine register LEVELS as raw 0-10V CV, like Mixer's
    // "gains" array. Missing/short keeps WidgetParams' 888000000 default for
    // the entries it doesn't cover (only entries actually returned are
    // copied - a zero-filled `levels` tail must NOT overwrite the default).
    float levels[kDrawbarCount] = {};
    const uint8_t levelCount =
        presetparser::getFloatArray(p, "drawbars", levels, static_cast<uint8_t>(kDrawbarCount));
    for (uint8_t d = 0; d < levelCount; ++d)
    {
        out.hammondDrawbar[d] = levels[d];
    }
    // "ratios" - the settings-dialog harmonic-ratio table. Missing/short
    // (e.g. a preset saved before this field existed) keeps WidgetParams'
    // kDefaultRatio-seeded default for the entries it doesn't cover.
    float ratios[kDrawbarCount] = {};
    const uint8_t ratioCount =
        presetparser::getFloatArray(p, "ratios", ratios, static_cast<uint8_t>(kDrawbarCount));
    for (uint8_t d = 0; d < ratioCount; ++d)
    {
        out.hammondRatio[d] = ratios[d];
    }
    const int modulationStride = presetparser::getInt(p, "modulationStride", 4);
    out.hammondModulationStride =
        static_cast<uint8_t>(modulationStride < 1 ? 1 : (modulationStride > 16 ? 16 : modulationStride));
}

} // namespace nodes::hammond

using nodes::hammond::HammondState;
