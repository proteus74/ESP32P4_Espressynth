#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "esp_attr.h"

#include "../CvConventions.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"

// Single-operator phase-modulation FM oscillator. Structurally a clone of
// nodes::oscillator (same carrier: phase-accumulator PhaseOsc, same
// pitchCv/tune/waveform/amplitude/gain/gate/feedback behavior) plus two new ports:
// "fmIn" (audio-rate - a modulator signal, typically another Oscillator's
// "value") and "fmIndex" (control-only CV - modulation depth). The modulator
// is deliberately NOT built in - patch any node's audio output into "fmIn"
// to modulate (classic modular FM: this widget is just the carrier/operator).
//
// FM here means genuine phase modulation (DX7-style), not frequency
// modulation: PhaseOsc::process()'s existing phaseOffset argument (see
// DspPrimitives.h) is fed a per-sample value derived from fmIn/fmIndex - the
// carrier's hz itself is never touched. With no fmIn signal and self
// feedback at 0V this node renders the same pure carrier waveform as
// WaveGen (phaseOffset stays 0).
// FmOscillatorState (value member) stays in NodeState.h.
namespace nodes::fmoscillator
{

namespace
{
// Same 0-10V/6-band mapping as nodes::oscillator::waveTypeFromCv() - kept as
// its own copy here (anonymous-namespace helpers aren't reachable across
// node files, and each node file stays self-contained, see repo convention
// in nodes/ArithmeticNodes.h's header comment).
inline WaveType waveTypeFromCv(float volts)
{
    int idx = static_cast<int>(volts / (cv::kMaxVolts / 6.0f));
    if (idx < 0)
        idx = 0;
    if (idx > 5)
        idx = 5;
    switch (idx)
    {
    case 0: return WaveType::Sine;
    case 1: return WaveType::Square;
    case 2: return WaveType::Triangle;
    case 3: return WaveType::Sawtooth;
    case 4: return WaveType::Noise;
    default: return WaveType::PinkNoise;
    }
}

// Max phase deviation (in wrapped-phase units, i.e. fractions of a full
// cycle - see PhaseOsc's phase_ domain in DspPrimitives.h) at fmIndexCv =
// 10V and a full-scale (+-kAudioPeakVolts) modulator. 4 cycles of deviation
// is already an extreme/harsh FM sound; tune by ear if it needs adjusting.
constexpr float kFmIndexMaxPhaseDeviation = 4.0f;

// One full cycle at 10V. Uses previous raw carrier sample, before amplitude
// and gain, so output level does not change feedback timbre.
constexpr float kSelfFeedbackMaxPhaseDeviation = 1.0f;

// Same FastLoopCtx/renderFastLoop<W> pattern as nodes::oscillator - the
// no-FM-patched fast path is byte-identical to a plain Oscillator's, so it
// reuses the exact same technique (waveform dispatch once per block via
// PhaseOsc::processWithIncrementT<W>(), not once per sample).
struct FastLoopCtx
{
    NodeInstance *inst;
    PhaseOsc *phaseGen;
    OnePoleLowpass *gainSmooth;
    const float *ampBuf;
    const float *gainBuf;
    const float *fmInBuf;
    float ampScalar;
    float baseAmp;
    float ampConstantValue;
    float gainTarget;
    float phaseIncrement;
    float phaseOffsetScale;
    float feedbackAmount;
    float *feedbackSample;
    bool gainFed;
    bool gainConverged;
    bool ampConstantForBlock;
    bool fmActive;
    bool feedbackActive;
};

template <WaveType W>
IRAM_ATTR inline void renderFastLoop(FastLoopCtx &c, size_t frames)
{
    for (size_t i = 0; i < frames; ++i)
    {
        const float amp = c.ampConstantForBlock ? c.ampConstantValue
                                                 : c.baseAmp + cv::vcaGain(c.ampBuf != nullptr ? c.ampBuf[i] : c.ampScalar, false);
        const float gainMul = c.gainFed ? cv::vcaGain(c.gainBuf[i], false)
                                         : (c.gainConverged ? c.gainTarget : c.gainSmooth->process(c.gainTarget));
        const float sample = c.phaseGen->processWithIncrementT<W>(c.phaseIncrement);
        c.inst->outL[i] = sample * amp * gainMul * cv::kAudioPeakVolts;
    }
}

// FM's normal case has block-constant carrier pitch but an audio-rate
// modulator. Keep that modulation in the fast loop: Hz conversion and phase
// increment happen once per block, waveform dispatch once below, while only
// the phase offset changes per sample.
template <WaveType W>
IRAM_ATTR inline void renderModulatedLoop(FastLoopCtx &c, size_t frames)
{
    float previous = *c.feedbackSample;
    for (size_t i = 0; i < frames; ++i)
    {
        const float amp = c.ampConstantForBlock ? c.ampConstantValue
                                                 : c.baseAmp + cv::vcaGain(c.ampBuf != nullptr ? c.ampBuf[i] : c.ampScalar, false);
        const float gainMul = c.gainFed ? cv::vcaGain(c.gainBuf[i], false)
                                         : (c.gainConverged ? c.gainTarget : c.gainSmooth->process(c.gainTarget));
        float phaseOffset = 0.0f;
        if (c.fmActive)
            phaseOffset = c.fmInBuf[i] * c.phaseOffsetScale;
        if (c.feedbackActive)
            phaseOffset += previous * c.feedbackAmount;
        const float sample = c.phaseGen->processWithIncrementT<W, true>(c.phaseIncrement, phaseOffset);
        if (c.feedbackActive)
            previous = sample;
        c.inst->outL[i] = sample * amp * gainMul * cv::kAudioPeakVolts;
    }
    if (c.feedbackActive)
        *c.feedbackSample = previous;
}

template <WaveType W>
IRAM_ATTR inline void renderSelectedFastLoop(FastLoopCtx &c, size_t frames, bool fmActive, bool feedbackActive)
{
    if (fmActive || feedbackActive)
        renderModulatedLoop<W>(c, frames);
    else
    {
        renderFastLoop<W>(c, frames);
    }
}
} // namespace

IRAM_ATTR inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    const float kSampleRate = static_cast<float>(synth::kSampleRate);
    auto &s = inst.fmOsc;
    constexpr float kCenterVolts = cv::kMaxVolts * 0.5f;

    // pitchCv (port 1) / tune (port 2): identical block-constant detection
    // to nodes::oscillator::render() - see its comment for the full
    // reasoning (a genuine DSP edge re-evaluates hz every sample; a
    // ControlRoute-only or unconnected value only needs one evaluation per
    // block).
    const bool pitchAudioFed = (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 1)) != 0;
    const bool pitchControlRamping = (inst.controlRampPortMask & static_cast<uint8_t>(1u << 1)) != 0;
    const bool pitchControlFed = controlPortConnected(inst, 1);
    const bool tuneAudioFed = (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 2)) != 0;
    float constantPitchCv = kCenterVolts;
    const bool pitchAudioConstant = pitchAudioFed && audioPortConstantForBlock(inst, 1, frames, constantPitchCv);
    const bool pitchNeedsAudioRate = (pitchAudioFed && !pitchAudioConstant) || pitchControlRamping;
    float constantTuneVolts = s.tuneVolts;
    const bool tuneAudioConstant = tuneAudioFed && audioPortConstantForBlock(inst, 2, frames, constantTuneVolts);
    const bool tuneNeedsAudioRate = tuneAudioFed && !tuneAudioConstant;

    // fmIn changes phase per sample, but does not make carrier pitch
    // audio-rate. Keeping those concerns separate lets the common
    // constant-pitch FM case retain the precomputed-increment fast path.
    const bool fmInFed = (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 5)) != 0;
    const bool needsAudioRatePitch = pitchNeedsAudioRate || tuneNeedsAudioRate;

    // "waveform" (port 3): control-only, one evaluation per block, same
    // reasoning as Oscillator's.
    const bool waveFed = controlPortConnected(inst, 3) || (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 3)) != 0;
    const WaveType wave = waveFed ? waveTypeFromCv(portSample(inst, 3, 0)) : s.wave;

    // "fmIndex" (port 6): control-only, one evaluation per block - a
    // connected CV substitutes the widget's own fmIndexCv outright, same
    // pattern as "tune" above.
    const bool fmIndexFed = controlPortConnected(inst, 6) || (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 6)) != 0;
    const float fmIndexCv = fmIndexFed ? portSample(inst, 6, 0) : s.fmIndexCv;
    const float indexAmount = fmIndexCv * cv::kInvMaxVolts * kFmIndexMaxPhaseDeviation;
    // Unpatched and patched-but-silent fmIn both use the pure-carrier path.
    // The block scan exits on the first changed sample, so an active audio
    // modulator normally costs only a couple of comparisons. Silent blocks
    // avoid every per-sample FM multiply and phase-offset wrap.
    bool fmInCarriesSignal = false;
    if (fmInFed && indexAmount != 0.0f)
    {
        float constantFmIn = 0.0f;
        fmInCarriesSignal = !audioPortConstantForBlock(inst, 5, frames, constantFmIn) || constantFmIn != 0.0f;
    }
    const bool phaseModulatable = wave != WaveType::Noise && wave != WaveType::PinkNoise;
    const bool fmActive = fmInCarriesSignal && phaseModulatable;
    const float phaseOffsetScale = indexAmount * cv::kInvAudioPeakVolts;
    const bool feedbackFed =
        controlPortConnected(inst, 7) || (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 7)) != 0;
    const float feedbackCv = feedbackFed ? portSample(inst, 7, 0) : s.feedbackCv;
    const float feedbackAmount = cv::vcaGain(feedbackCv, false) * kSelfFeedbackMaxPhaseDeviation;
    const bool feedbackActive = feedbackAmount != 0.0f && phaseModulatable;
    const bool modulationActive = fmActive || feedbackActive;

    // "gain" (port 4): identical to Oscillator's - real audio-rate port,
    // per-sample-smoothed only in the unwired case.
    const bool gainFed = (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 4)) != 0;
    const float gainTarget = cv::vcaGain(s.gainCv, false);
    constexpr float kSilentGain = 1e-4f;
    if (!gainFed && gainTarget <= 0.0f && s.gainSmooth.current() <= kSilentGain)
    {
        memset(inst.outL, 0, frames * sizeof(float));
        return;
    }
    constexpr float kGainConvergeEps = 1e-4f;
    const bool gainConverged = !gainFed && fabsf(s.gainSmooth.current() - gainTarget) <= kGainConvergeEps;
    if (gainConverged)
    {
        s.gainSmooth.snapTo(gainTarget);
    }

    const float *ampBuf = inst.inPorts[0];
    const float ampScalar = inst.controlValue[0];
    const float *gainBuf = gainFed ? inst.inPorts[4] : nullptr;
    const float *fmInBuf = fmInFed ? inst.inPorts[5] : nullptr;

    const float pitchCvConst =
        pitchAudioFed ? constantPitchCv : (pitchControlFed ? portSample(inst, 1, 0) : kCenterVolts);
    const float tuneVoltsConst = tuneAudioFed ? constantTuneVolts : s.tuneVolts;

    const float baseAmp = controlPortConnected(inst, 0) ? s.amplitude : 0.0f;
    float constantAmpCv = ampScalar;
    const bool ampConstantForBlock = audioPortConstantForBlock(inst, 0, frames, constantAmpCv);
    const float ampConstantValue = baseAmp + cv::vcaGain(constantAmpCv, false);

    if (needsAudioRatePitch)
    {
        if (modulationActive)
        {
            for (size_t i = 0; i < frames; ++i)
            {
                const float pitchCv = pitchNeedsAudioRate
                                          ? portSample(inst, 1, i)
                                          : pitchCvConst;
                const float tuneVolts = tuneNeedsAudioRate ? portSample(inst, 2, i) : tuneVoltsConst;
                const float hz = cv::hzFromPitchCv(pitchCv + (tuneVolts - kCenterVolts));
                const float amp = ampConstantForBlock
                                      ? ampConstantValue
                                      : baseAmp + cv::vcaGain(ampBuf != nullptr ? ampBuf[i] : ampScalar, false);
                const float gainMul = gainFed ? cv::vcaGain(gainBuf[i], false)
                                              : (gainConverged ? gainTarget : s.gainSmooth.process(gainTarget));
                float phaseOffset = fmActive ? fmInBuf[i] * phaseOffsetScale : 0.0f;
                if (feedbackActive)
                    phaseOffset += s.feedbackSample * feedbackAmount;
                const float sample = s.phaseGen.processWithIncrement(wave, hz / kSampleRate, phaseOffset);
                if (feedbackActive)
                    s.feedbackSample = sample;
                inst.outL[i] = sample * amp * gainMul * cv::kAudioPeakVolts;
            }
        }
        else
        {
            for (size_t i = 0; i < frames; ++i)
            {
                const float pitchCv = pitchNeedsAudioRate
                                          ? portSample(inst, 1, i)
                                          : pitchCvConst;
                const float tuneVolts = tuneNeedsAudioRate ? portSample(inst, 2, i) : tuneVoltsConst;
                const float hz = cv::hzFromPitchCv(pitchCv + (tuneVolts - kCenterVolts));
                const float amp = ampConstantForBlock
                                      ? ampConstantValue
                                      : baseAmp + cv::vcaGain(ampBuf != nullptr ? ampBuf[i] : ampScalar, false);
                const float gainMul = gainFed ? cv::vcaGain(gainBuf[i], false)
                                              : (gainConverged ? gainTarget : s.gainSmooth.process(gainTarget));
                const float sample = s.phaseGen.processWithIncrement(wave, hz / kSampleRate);
                inst.outL[i] = sample * amp * gainMul * cv::kAudioPeakVolts;
            }
        }
        return;
    }

    const float hz = cv::hzFromPitchCv(pitchCvConst + (tuneVoltsConst - kCenterVolts));
    const float phaseIncrement = hz / kSampleRate;
    FastLoopCtx ctx{&inst,           &s.phaseGen,        &s.gainSmooth, ampBuf,
                    gainBuf,         fmInBuf,            ampScalar,     baseAmp,
                    ampConstantValue, gainTarget,        phaseIncrement, phaseOffsetScale,
                    feedbackAmount,  &s.feedbackSample,  gainFed,       gainConverged,
                    ampConstantForBlock, fmActive,       feedbackActive};
    switch (wave)
    {
    case WaveType::Sine:
        renderSelectedFastLoop<WaveType::Sine>(ctx, frames, fmActive, feedbackActive);
        break;
    case WaveType::Square:
        renderSelectedFastLoop<WaveType::Square>(ctx, frames, fmActive, feedbackActive);
        break;
    case WaveType::Triangle:
        renderSelectedFastLoop<WaveType::Triangle>(ctx, frames, fmActive, feedbackActive);
        break;
    case WaveType::Sawtooth:
        renderSelectedFastLoop<WaveType::Sawtooth>(ctx, frames, fmActive, feedbackActive);
        break;
    case WaveType::Noise:
        renderSelectedFastLoop<WaveType::Noise>(ctx, frames, fmActive, feedbackActive);
        break;
    case WaveType::PinkNoise:
        renderSelectedFastLoop<WaveType::PinkNoise>(ctx, frames, fmActive, feedbackActive);
        break;
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    inst.fmOsc.wave = p.waveType;
    inst.fmOsc.tuneVolts = p.tuneVolts;
    inst.fmOsc.amplitude = p.amplitude;
    inst.fmOsc.gainCv = p.gain;
    inst.fmOsc.fmIndexCv = p.fmIndex;
    inst.fmOsc.feedbackCv = p.oscillatorFeedback;
    // Same cutoff/reason as Oscillator's gainSmooth - see NodeState.h.
    inst.fmOsc.gainSmooth.setCutoff(300.0f, static_cast<float>(synth::kSampleRate));
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr)
    {
        return false;
    }
    if (strcmp(name, "tune") == 0)
    {
        inst.fmOsc.tuneVolts = value;
        return true;
    }
    if (strcmp(name, "amplitude") == 0)
    {
        inst.fmOsc.amplitude = value;
        return true;
    }
    if (strcmp(name, "gain") == 0)
    {
        inst.fmOsc.gainCv = value;
        return true;
    }
    if (strcmp(name, "fmIndex") == 0)
    {
        inst.fmOsc.fmIndexCv = value;
        return true;
    }
    if (strcmp(name, "feedback") == 0)
    {
        inst.fmOsc.feedbackCv = value;
        return true;
    }
    if (strcmp(name, "gate") == 0)
    {
        const bool gateNow = value > cv::kGateThresholdVolts;
        if (gateNow && !inst.fmOsc.gatePrev && inst.fmOsc.wave != WaveType::Noise &&
            inst.fmOsc.wave != WaveType::PinkNoise)
        {
            inst.fmOsc.phaseGen.resetPhase();
            inst.fmOsc.feedbackSample = 0.0f;
        }
        inst.fmOsc.gatePrev = gateNow;
        return true;
    }
    return false;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.waveType = waveTypeFromString(presetparser::getString(p, "waveType", "sine"));
    // Same key/domain/default as nodes::oscillator::parsePreset() - see
    // main.js's WIDGET_ENGINES["fm-oscillator"].
    out.tuneVolts = presetparser::getFloat(p, "tune", 5.0f);
    out.amplitude = presetparser::getFloat(p, "amplitude", 1.0f);
    out.gain = presetparser::getFloat(p, "gain", 10.0f);
    out.fmIndex = presetparser::getFloat(p, "fmIndex", 0.0f);
    out.oscillatorFeedback = presetparser::getFloat(p, "feedback", 0.0f);
}

} // namespace nodes::fmoscillator
