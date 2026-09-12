#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "esp_attr.h"

#include "../CvConventions.h"
#include "../EngineStats.h"
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
    // EXPERIMENTAL: pre-converted to PhaseOsc's internal 32-bit fixed-point
    // domain (PhaseOsc::toPhaseInt(), once per block) - see DspPrimitives.h.
    uint32_t phaseIncrementInt;
    float phaseOffsetScale;
    float feedbackAmount;
    float *feedbackSample;
    bool gainFed;
    bool gainConverged;
    bool ampConstantForBlock;
    bool fmActive;
    bool feedbackActive;
    // Set only when amplitude (port 0) is NOT block-constant but IS a
    // whole-block linear ramp tagged by the producer (typically an ADSR
    // mid attack/decay - see NodeInstance.h's rampPortMask/RampDescriptor
    // and AdsrNode.h's wholeBlockRamp() tag). ampRampStart/Step already
    // fold in baseAmp and the kInvMaxVolts scale render() would otherwise
    // apply per sample via cv::vcaGain() - see renderRampAmpFastLoop below.
    bool hasAmpRamp;
    float ampRampStart;
    float ampRampStep;
    // "Duty Cycle" (port 8, Square only - see PortTables.h/PhaseOsc's
    // sampleAt in DspPrimitives.h). Pre-converted to PhaseOsc's internal
    // 32-bit domain once per block, same reasoning as phaseIncrementInt -
    // harmless/unused for every other waveform.
    uint32_t dutyThresholdInt;
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
        const float sample = c.phaseGen->processWithIncrementT<W>(c.phaseIncrementInt, 0.0f, c.dutyThresholdInt);
        c.inst->outL[i] = sample * amp * gainMul * cv::kAudioPeakVolts;
    }
}

// P2 (documentation/fm-plan.md section 3, "Den haeufigsten hoerbaren Pfad
// gezielt vereinfachen"): disassembly of the built firmware showed the two
// audible carriers in a 4-op FM preset (fmActive/feedbackActive both false
// once their modulator's output is silence-zeroed by the P1 path above)
// spend most of their per-sample cost re-evaluating the ampConstantForBlock
// and gainConverged/gainFed ternaries in renderFastLoop below with a branch
// on EVERY sample, even though every one of those three flags is
// loop-invariant for the whole block - GCC does not hoist them out past the
// `FastLoopCtx &c` reference (the OnePoleLowpass::process() call reachable
// from the untaken ternary branch conservatively blocks that). When all
// three flags hold this way, `amp`/`gainMul` equal c.ampConstantValue/
// c.gainTarget on every single iteration regardless - reading those two
// once before the loop skips their redundant per-sample recomputation.
// gainMul*kAudioPeakVolts is also folded once per block. The constant-amp
// path folds amp into that scale too, leaving one output multiply per sample;
// the ramp path keeps amp dynamic and needs two. Reassociation can differ by
// normal single-precision rounding only (at most one int16 PCM step in the
// host-side randomized regression used for this change).
template <WaveType W>
IRAM_ATTR inline void renderConstFastLoop(FastLoopCtx &c, size_t frames)
{
    const float outputScale = c.ampConstantValue * c.gainTarget * cv::kAudioPeakVolts;
    for (size_t i = 0; i < frames; ++i)
    {
        const float sample = c.phaseGen->processWithIncrementT<W>(c.phaseIncrementInt, 0.0f, c.dutyThresholdInt);
        c.inst->outL[i] = sample * outputScale;
    }
}

// Companion to renderConstFastLoop above, for the other common non-silent
// case: amplitude (port 0) is actively ramping (ADSR attack/decay) rather
// than block-constant, but still a whole-block LINEAR ramp (see
// FastLoopCtx::hasAmpRamp). Same established pattern already used by
// GainNode.h/DetuneNode.h/ArithmeticNodes.h for a tagged ramp input -
// advance amp by simple accumulation instead of re-deriving it via
// cv::vcaGain(c.ampBuf[i], false) (itself just a clamp+scale, but paid
// again every sample through the same un-hoistable ternary P2 above
// targets) every sample. NOT bit-identical to the per-sample vcaGain
// path (repeated float addition vs. a fresh multiply each sample) - same
// trade-off already accepted project-wide for every other ramp-tag
// consumer, inaudible on a multi-millisecond envelope ramp.
template <WaveType W>
IRAM_ATTR inline void renderRampAmpFastLoop(FastLoopCtx &c, size_t frames)
{
    const float scaledGain = c.gainTarget * cv::kAudioPeakVolts;
    float amp = c.ampRampStart;
    for (size_t i = 0; i < frames; ++i)
    {
        const float sample = c.phaseGen->processWithIncrementT<W>(c.phaseIncrementInt, 0.0f, c.dutyThresholdInt);
        c.inst->outL[i] = sample * amp * scaledGain;
        amp += c.ampRampStep;
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
        const float sample = c.phaseGen->processWithIncrementT<W, true>(c.phaseIncrementInt, phaseOffset, c.dutyThresholdInt);
        if (c.feedbackActive)
            previous = sample;
        c.inst->outL[i] = sample * amp * gainMul * cv::kAudioPeakVolts;
    }
    if (c.feedbackActive)
        *c.feedbackSample = previous;
}

// Companion to renderConstFastLoop above, for the modulated (fmActive/
// feedbackActive) case - same amp/gainMul hoist (user question after
// seeing the widget-CPU breakdown in the visual patch editor: an FM
// operator whose own "FM In" is wired up never reached the P2 fast paths
// at all, only ever renderModulatedLoop below, which still had BOTH
// per-sample ternaries P2 already removed from the unmodulated case, on
// top of the phase-offset/feedback math no modulated loop can drop). Keeps
// the phase-offset/feedback machinery unchanged - only the amp/gainMul
// selection moves outside the loop, same non-reorder argument as
// renderConstFastLoop.
template <WaveType W>
IRAM_ATTR inline void renderModulatedConstFastLoop(FastLoopCtx &c, size_t frames)
{
    const float outputScale = c.ampConstantValue * c.gainTarget * cv::kAudioPeakVolts;
    float previous = *c.feedbackSample;
    for (size_t i = 0; i < frames; ++i)
    {
        float phaseOffset = 0.0f;
        if (c.fmActive)
            phaseOffset = c.fmInBuf[i] * c.phaseOffsetScale;
        if (c.feedbackActive)
            phaseOffset += previous * c.feedbackAmount;
        const float sample = c.phaseGen->processWithIncrementT<W, true>(c.phaseIncrementInt, phaseOffset, c.dutyThresholdInt);
        if (c.feedbackActive)
            previous = sample;
        c.inst->outL[i] = sample * outputScale;
    }
    if (c.feedbackActive)
        *c.feedbackSample = previous;
}

// Companion to renderRampAmpFastLoop above, for the modulated case - same
// ramp-accumulator amp, same phase-offset/feedback machinery kept as-is.
template <WaveType W>
IRAM_ATTR inline void renderModulatedRampAmpFastLoop(FastLoopCtx &c, size_t frames)
{
    const float scaledGain = c.gainTarget * cv::kAudioPeakVolts;
    float amp = c.ampRampStart;
    float previous = *c.feedbackSample;
    for (size_t i = 0; i < frames; ++i)
    {
        float phaseOffset = 0.0f;
        if (c.fmActive)
            phaseOffset = c.fmInBuf[i] * c.phaseOffsetScale;
        if (c.feedbackActive)
            phaseOffset += previous * c.feedbackAmount;
        const float sample = c.phaseGen->processWithIncrementT<W, true>(c.phaseIncrementInt, phaseOffset, c.dutyThresholdInt);
        if (c.feedbackActive)
            previous = sample;
        c.inst->outL[i] = sample * amp * scaledGain;
        amp += c.ampRampStep;
    }
    if (c.feedbackActive)
        *c.feedbackSample = previous;
}

template <WaveType W>
IRAM_ATTR inline void renderSelectedFastLoop(FastLoopCtx &c, size_t frames, bool fmActive, bool feedbackActive)
{
    if (fmActive || feedbackActive)
    {
        if (c.ampConstantForBlock && c.gainConverged && !c.gainFed)
            renderModulatedConstFastLoop<W>(c, frames);
        else if (c.hasAmpRamp && c.gainConverged && !c.gainFed)
            renderModulatedRampAmpFastLoop<W>(c, frames);
        else
            renderModulatedLoop<W>(c, frames);
    }
    else if (c.ampConstantForBlock && c.gainConverged && !c.gainFed)
        renderConstFastLoop<W>(c, frames);
    else if (c.hasAmpRamp && c.gainConverged && !c.gainFed)
        renderRampAmpFastLoop<W>(c, frames);
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
    // Cleared unconditionally up front, same reasoning as AdsrNode.h's
    // outIsRamp reset - a downstream Constant-aware consumer (see
    // NodeInstance.h's constantPortMask) must never read a stale true left
    // over from a previous block that DID take one of the silent-output
    // fast paths below.
    inst.outIsConstant = false;

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
    if (pitchNeedsAudioRate) ++enginestats::current.fmPitchAudioRate;
    else ++enginestats::current.fmPitchConstant;
    if (tuneAudioFed)
    {
        if (tuneNeedsAudioRate) ++enginestats::current.fmTuneAudioRate;
        else ++enginestats::current.fmTuneConstant;
    }

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

    // "dutyCycle" (port 8): control-only, one evaluation per block, same
    // pattern as "fmIndex" above. Only compute its fixed-point threshold for
    // Square; every other waveform ignores it and keeps the default. 5V
    // (center) = classic 50% square, matching dutyCycleCv's default.
    // NOTE: port 8 needs the full-width synth::portBit() here, not the
    // `static_cast<uint8_t>(1u << n)` pattern used for ports 0-7 elsewhere
    // in this function - 1u<<8 = 256 doesn't fit uint8_t (would silently
    // truncate to 0, i.e. "never connected").
    const bool dutyCycleFed = controlPortConnected(inst, 8) || (inst.audioConnectedPortMask & synth::portBit(8)) != 0;
    const float dutyCycleCv = dutyCycleFed ? portSample(inst, 8, 0) : s.dutyCycleCv;
    const uint32_t dutyThresholdInt = wave == WaveType::Square
                                          ? PhaseOsc::dutyThresholdFromUnit(dutyCycleCv * cv::kInvMaxVolts)
                                          : 0x80000000u;
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
    if (fmActive) ++enginestats::current.fmModActive;
    if (feedbackActive) ++enginestats::current.fmFeedbackActive;

    // "gain" (port 4): identical to Oscillator's - real audio-rate port,
    // per-sample-smoothed only in the unwired case.
    const bool gainFed = (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 4)) != 0;
    const float gainTarget = cv::vcaGain(s.gainCv, false);
    constexpr float kSilentGain = 1e-4f;
    if (!gainFed && gainTarget <= 0.0f && s.gainSmooth.current() <= kSilentGain)
    {
        memset(inst.outL, 0, frames * sizeof(float));
        inst.outIsConstant = true;
        inst.outConstantValue = 0.0f;
        ++enginestats::current.fmSilentSkipped;
        return;
    }
    constexpr float kGainConvergeEps = 1e-4f;
    const bool gainConverged = !gainFed && fabsf(s.gainSmooth.current() - gainTarget) <= kGainConvergeEps;
    if (gainConverged)
    {
        s.gainSmooth.snapTo(gainTarget);
        ++enginestats::current.fmGainConverged;
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
    if (ampBuf != nullptr)
    {
        if (ampConstantForBlock)
        {
            ++enginestats::current.fmAmpConstant;
            if (fabsf(ampConstantValue) <= kSilentGain) ++enginestats::current.fmAmpConstantZero;
        }
        else ++enginestats::current.fmAmpAudioRate;
    }

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
                const float sample = s.phaseGen.processWithIncrement(wave, hz / kSampleRate, phaseOffset, dutyThresholdInt);
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
                const float sample = s.phaseGen.processWithIncrement(wave, hz / kSampleRate, 0.0f, dutyThresholdInt);
                inst.outL[i] = sample * amp * gainMul * cv::kAudioPeakVolts;
            }
        }
        return;
    }

    const float hz = cv::hzFromPitchCv(pitchCvConst + (tuneVoltsConst - kCenterVolts));
    const float phaseIncrement = hz / kSampleRate;
    const uint32_t phaseIncrementInt = PhaseOsc::toPhaseInt(phaseIncrement);

    // P1 (documentation/fm-plan.md section 3, "Stumme Modulatoren billiger
    // weiterführen"): a modulator sitting on its Sustain-0 ADSR plateau (the
    // two-modulator/two-carrier FM preset's common case after decay) has
    // ampConstantForBlock==true and ampConstantValue==0.0f here - every
    // sample the full loop below would compute is multiplied by that exact
    // 0 anyway (amp is the FIRST factor in `sample * amp * gainMul * ...`).
    // Skip the waveform synthesis outright and only keep the two pieces of
    // state a later, audible block still depends on: the phase accumulator
    // (advancePhaseOnly, so the next gate-on attack picks up mid-cycle
    // instead of glitching/restarting - phase must NEVER freeze) and the
    // gain smoother (so it keeps ramping toward gainTarget instead of
    // stalling silently). Self feedback still needs the real per-sample
    // carrier value even while inaudible (renderModulatedLoop's `previous`
    // chases the true waveform), so feedbackActive excludes this path.
    // External FM does NOT need to be excluded: fmIn only perturbs the READ
    // phase offset each sample, never phase_ itself (see PhaseOsc's
    // comment), so it has no bearing on what advancePhaseOnly must do here.
    // Noise/PinkNoise are excluded via phaseModulatable (no phase_ to
    // advance meaningfully for them, see plan section 2 point 7).
    if (phaseModulatable && !feedbackActive && ampConstantForBlock && ampConstantValue == 0.0f)
    {
        s.phaseGen.advancePhaseOnly(phaseIncrementInt, frames);
        if (!gainFed && !gainConverged)
        {
            for (size_t i = 0; i < frames; ++i)
                s.gainSmooth.process(gainTarget);
        }
        memset(inst.outL, 0, frames * sizeof(float));
        inst.outIsConstant = true;
        inst.outConstantValue = 0.0f;
        return;
    }

    // P2 continued (fm-plan.md section 3): amplitude actively ramping
    // (ADSR attack/decay) rather than block-constant - if the producer
    // tagged it a whole-block linear ramp (see NodeInstance.h's
    // rampPortMask, same mechanism GainNode.h/DetuneNode.h/ArithmeticNodes.h
    // already consume), fold baseAmp/kInvMaxVolts in once here instead of
    // per sample - see renderRampAmpFastLoop above.
    const NodeInstance::RampDescriptor *ampRamp = ampConstantForBlock ? nullptr : rampForPort(inst, 0);
    const bool hasAmpRamp = ampRamp != nullptr;
    const float ampRampStart = hasAmpRamp ? baseAmp + ampRamp->start * cv::kInvMaxVolts : 0.0f;
    const float ampRampStep = hasAmpRamp ? ampRamp->inc * cv::kInvMaxVolts : 0.0f;

    FastLoopCtx ctx{&inst,           &s.phaseGen,        &s.gainSmooth, ampBuf,
                    gainBuf,         fmInBuf,            ampScalar,     baseAmp,
                    ampConstantValue, gainTarget,        phaseIncrementInt, phaseOffsetScale,
                    feedbackAmount,  &s.feedbackSample,  gainFed,       gainConverged,
                    ampConstantForBlock, fmActive,       feedbackActive,
                    hasAmpRamp,      ampRampStart,       ampRampStep,
                    dutyThresholdInt};
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
    auto &s = inst.fmOsc;
    s.wave = p.waveType;
    s.tuneVolts = p.tuneVolts;
    s.amplitude = p.amplitude;
    s.gainCv = p.gain;
    s.fmIndexCv = p.fmIndex;
    s.feedbackCv = p.oscillatorFeedback;
    s.dutyCycleCv = p.dutyCycle;
    // Same cutoff/reason as Oscillator's gainSmooth - see NodeState.h.
    s.gainSmooth.setCutoff(300.0f, static_cast<float>(synth::kSampleRate));
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr)
    {
        return false;
    }
    auto &s = inst.fmOsc;
    if (strcmp(name, "tune") == 0)
    {
        s.tuneVolts = value;
        return true;
    }
    if (strcmp(name, "amplitude") == 0)
    {
        s.amplitude = value;
        return true;
    }
    if (strcmp(name, "gain") == 0)
    {
        s.gainCv = value;
        return true;
    }
    if (strcmp(name, "fmIndex") == 0)
    {
        s.fmIndexCv = value;
        return true;
    }
    if (strcmp(name, "feedback") == 0)
    {
        s.feedbackCv = value;
        return true;
    }
    if (strcmp(name, "dutyCycle") == 0)
    {
        s.dutyCycleCv = value;
        return true;
    }
    if (strcmp(name, "gate") == 0)
    {
        const bool gateNow = value > cv::kGateThresholdVolts;
        if (gateNow && !s.gatePrev && s.wave != WaveType::Noise &&
            s.wave != WaveType::PinkNoise)
        {
            s.phaseGen.resetPhase();
            s.feedbackSample = 0.0f;
        }
        s.gatePrev = gateNow;
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
    out.dutyCycle = presetparser::getFloat(p, "dutyCycle", 5.0f);
}

} // namespace nodes::fmoscillator
