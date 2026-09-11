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

// Phase-accumulator oscillator (sine/square/triangle/sawtooth/noise) driven
// by pitchCv/tune (1V/oct-ish CV, see CvConventions.h). OscillatorState
// (value member) stays in NodeState.h.
namespace nodes::oscillator
{

namespace
{
// Oscillator's "waveform" CV port (see PortTables.h): 0-10V split into 5
// equal bands, in WaveType enum order (Sine/Square/Triangle/Sawtooth/white
// noise/pink noise) - matches main.js's mapWaveformValue() 1:1. 10V itself
// must still resolve to pink noise (last band), not fall off the end.
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

// One full cycle at 10V. Feedback uses the previous raw oscillator sample,
// giving the loop its required one-sample delay without creating a graph
// cycle or disturbing block rendering.
constexpr float kSelfFeedbackMaxPhaseDeviation = 1.0f;

// audioPortConstantForBlock() now lives in NodeInstance.h's noderender
// namespace (shared by every node type that wants this fast path, not just
// Oscillator - see its comment there and allPortsConstantForBlock() next to
// it, used below and by ArithmeticNodes.h/DetuneNode.h/ValueMapperNode.h).

// Everything the fast (non-audio-rate-pitch) per-sample loop below needs,
// bundled so the templated loop function itself stays a short parameter
// list. Plain pointers/values, no ownership - lifetime is just the one
// render() call that builds this on its stack.
struct FastLoopCtx
{
    NodeInstance *inst;
    PhaseOsc *phaseGen;
    OnePoleLowpass *gainSmooth;
    const float *ampBuf;
    const float *gainBuf;
    float ampScalar;
    float baseAmp;
    float ampConstantValue;
    float gainTarget;
    float phaseIncrement;
    float feedbackAmount;
    float *feedbackSample;
    bool gainFed;
    bool gainConverged;
    bool ampConstantForBlock;
};

// The actual per-sample loop, one shared definition specialized per
// waveform at compile time (PhaseOsc::processWithIncrementT<W>() - see its
// comment in DspPrimitives.h) instead of six hand-written copies. Dispatch
// to the right instantiation happens once per block via the switch in
// render() below, not once per sample.
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

template <WaveType W>
IRAM_ATTR inline void renderFeedbackLoop(FastLoopCtx &c, size_t frames)
{
    float previous = *c.feedbackSample;
    for (size_t i = 0; i < frames; ++i)
    {
        const float amp = c.ampConstantForBlock ? c.ampConstantValue
                                                 : c.baseAmp + cv::vcaGain(c.ampBuf != nullptr ? c.ampBuf[i] : c.ampScalar, false);
        const float gainMul = c.gainFed ? cv::vcaGain(c.gainBuf[i], false)
                                         : (c.gainConverged ? c.gainTarget : c.gainSmooth->process(c.gainTarget));
        const float sample =
            c.phaseGen->processWithIncrementT<W, true>(c.phaseIncrement, previous * c.feedbackAmount);
        previous = sample;
        c.inst->outL[i] = sample * amp * gainMul * cv::kAudioPeakVolts;
    }
    *c.feedbackSample = previous;
}

template <WaveType W>
IRAM_ATTR inline void renderSelectedFastLoop(FastLoopCtx &c, size_t frames, bool feedbackActive)
{
    if (feedbackActive)
        renderFeedbackLoop<W>(c, frames);
    else
        renderFastLoop<W>(c, frames);
}
} // namespace

// IRAM_ATTR: confirmed hottest single node render() on real hardware (see
// documentation/native-synth-optimization-plan-2026-09-09.md section 1c) -
// see DspPrimitives.h's sineLookup() comment for the full reasoning.
IRAM_ATTR inline void render(NodeInstance &inst, size_t frames)
{
    using namespace noderender;
    const float kSampleRate = static_cast<float>(synth::kSampleRate);
    auto &s = inst.osc;
    // pitchCv (port 1) and tune (port 2): a genuine DSP edge
    // (audioConnectedPortMask) now re-evaluates hz every sample below - true
    // audio-rate FM/vibrato (see the pitchAudioFed/tuneAudioFed branch
    // further down). A ControlRoute-only pitchCv (MidiIn's pitchCv -
    // controlConnectedPortMask, MIDI note held for the whole block) or an
    // unconnected/manual tune still only need one evaluation per block - no
    // audible benefit from finer resolution there. pitchCv defaults to
    // center (no note played) when unconnected. tune works the same as
    // Filter's cutoff/resonance: a connected CV SUBSTITUTES the Tune
    // slider's own tuneVolts outright (not an additional offset on top of
    // it), so feeding a given voltage sounds identical to dragging the
    // slider to that same voltage, and the web UI mirrors this by moving
    // the slider to match (see main.js's oscillator "tune" port handling).
    //
    // tune must NOT check controlPortConnected: a ControlRoute here
    // (Knob/Button by paramName) already lands directly in s.tuneVolts via
    // ParamDispatch's "tune" case, and Graph::renderBlock deliberately
    // skips filling inPorts[2]/controlValue for it (its
    // oscillatorDedicatedField special-case, to avoid double-applying a
    // Knob's value once the additive model above was replaced) - so
    // portSample(2) would read a stale unconnected-default, not the Knob's
    // live value. Only a real DSP edge (e.g. an LFO/Oscillator wired
    // directly for vibrato/FM) needs the portSample() read here.
    constexpr float kCenterVolts = cv::kMaxVolts * 0.5f;
    // Cleared unconditionally up front, same reasoning as AdsrNode.h's
    // outIsRamp reset - a downstream Constant-aware consumer (see
    // NodeInstance.h's constantPortMask) must never read a stale true left
    // over from a previous block that DID take the silent-skip path below.
    inst.outIsConstant = false;
    const bool pitchAudioFed = (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 1)) != 0;
    const bool pitchControlRamping = (inst.controlRampPortMask & static_cast<uint8_t>(1u << 1)) != 0;
    const bool pitchControlFed = controlPortConnected(inst, 1);
    const bool tuneAudioFed = (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 2)) != 0;
    float constantPitchCv = kCenterVolts;
    const bool pitchAudioConstant = pitchAudioFed && audioPortConstantForBlock(inst, 1, frames, constantPitchCv);
    const bool pitchNeedsAudioRate = (pitchAudioFed && !pitchAudioConstant) || pitchControlRamping;
    // Tune (port 2) gets the identical constant-for-block check as pitch
    // above - a Tune cable fed from a Frequency Divider or a held-note
    // source is just as likely to carry one repeated CV value for the whole
    // block. Closes the gap the plan calls out explicitly (documentation/
    // native-synth-optimization-plan-2026-09-09.md, section 2): pitch had
    // this fast path already, tune didn't.
    float constantTuneVolts = s.tuneVolts;
    const bool tuneAudioConstant = tuneAudioFed && audioPortConstantForBlock(inst, 2, frames, constantTuneVolts);
    const bool tuneNeedsAudioRate = tuneAudioFed && !tuneAudioConstant;
    const bool needsAudioRate = pitchNeedsAudioRate || tuneNeedsAudioRate;
    if (pitchNeedsAudioRate) ++enginestats::current.oscPitchAudioRate;
    else ++enginestats::current.oscPitchConstant;
    if (tuneAudioFed)
    {
        if (tuneNeedsAudioRate) ++enginestats::current.oscTuneAudioRate;
        else ++enginestats::current.oscTuneConstant;
    }
    // "waveform" (port 3) is control-only too (see isControlOnlyInput) -
    // stays read once per block: an audio-rate switch between discrete
    // waveforms has no meaningful "signal shape" (unlike cutoff/pitch), it
    // would just glitch. Not wired: falls back to the widget's own waveform
    // dropdown (s.wave), unchanged from before.
    const bool waveFed = controlPortConnected(inst, 3) || (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 3)) != 0;
    const WaveType wave = waveFed ? waveTypeFromCv(portSample(inst, 3, 0)) : s.wave;
    const bool feedbackFed =
        controlPortConnected(inst, 5) || (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 5)) != 0;
    const float feedbackCv = feedbackFed ? portSample(inst, 5, 0) : s.feedbackCv;
    const float feedbackAmount = cv::vcaGain(feedbackCv, false) * kSelfFeedbackMaxPhaseDeviation;
    const bool feedbackActive = feedbackAmount != 0.0f;
    // "gain" (Port-Index 4) is a real audio-rate port (like Gain's
    // "amount") - 0-10V CV, 10V = unchanged output signal. When wired, the
    // CV already arrives natively per sample (no smoothing needed); when
    // unwired, the manual gainCv scalar is smoothed PER SAMPLE, same
    // pattern/reason as GainState::amountSmooth.
    const bool gainFed = (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 4)) != 0;
    const float gainTarget = cv::vcaGain(s.gainCv, false);
    // Gain knob itself parked at 0 (not a live envelope/LFO feeding it) AND
    // the smoother has already ramped down to inaudible - the oscillator's
    // output is silence either way, so skip the actual waveform synthesis
    // (phaseGen.process(), the real per-sample cost here) instead of
    // computing samples nobody will hear. Checked once per block, not per
    // sample, and gated on the SMOOTHED value (not just gainCv) so a knob
    // turned down THIS block still fades out through the normal path below
    // exactly as before - this fast path only engages once already silent,
    // so it never introduces a click. Phase freezes while skipped, same as
    // Graph::renderBlock already does project-wide for an idle ADSR voice -
    // nothing else in this engine reads another oscillator's raw phase, so
    // that's inaudible here too.
    constexpr float kSilentGain = 1e-4f;
    if (!gainFed && gainTarget <= 0.0f && s.gainSmooth.current() <= kSilentGain)
    {
        memset(inst.outL, 0, frames * sizeof(float));
        inst.outIsConstant = true;
        inst.outConstantValue = 0.0f;
        ++enginestats::current.oscSilentSkipped;
        return;
    }

    // Gain-smoothing convergence stop (plan section 3, "P1: WaveGen-
    // Schleife vereinfachen", item 5): once the unwired gain smoother has
    // settled within epsilon of its target, snap it there and skip the
    // per-sample OnePoleLowpass::process() call for the rest of this block.
    // The next real gain change still starts its ramp from the (now exact)
    // target instead of a stale near-target remainder.
    constexpr float kGainConvergeEps = 1e-4f;
    const bool gainConverged = !gainFed && fabsf(s.gainSmooth.current() - gainTarget) <= kGainConvergeEps;
    if (gainConverged)
    {
        s.gainSmooth.snapTo(gainTarget);
        ++enginestats::current.oscGainConverged;
    }

    // Amplitude/gain port pointers resolved once per block instead of
    // portSample()'s per-sample inPorts[]-null-check + fallback (plan item
    // 4). gainBuf is only ever dereferenced under gainFed, which - like
    // every other audio-connected port in this engine - guarantees
    // GraphBuilder allocated its buffer (edges only ever write into a
    // non-null inPorts[] slot, see Graph::renderBlock's edge-summing pass).
    const float *ampBuf = inst.inPorts[0];
    const float ampScalar = inst.controlValue[0];
    const float *gainBuf = gainFed ? inst.inPorts[4] : nullptr;

    const float baseAmp = controlPortConnected(inst, 0) ? s.amplitude : 0.0f;
    // "amplitude" (port 0) block-constant detection, same technique as
    // pitch/tune above (plan section 3, P2: extend the constant-for-block
    // fast path to more ports). It's read via vcaGain() every sample
    // regardless of the pitch/tune fast path, since it's an independent
    // modulation source (typically an ADSR envelope) - but an ADSR sitting
    // on its sustain plateau (or simply unconnected) produces exactly the
    // same repeated value all block, so this skips the redundant per-sample
    // vcaGain() call once that's detected.
    float constantAmpCv = ampScalar;
    const bool ampConstantForBlock = audioPortConstantForBlock(inst, 0, frames, constantAmpCv);
    const float ampConstantValue = baseAmp + cv::vcaGain(constantAmpCv, false);
    if (ampBuf != nullptr)
    {
        if (ampConstantForBlock) ++enginestats::current.oscAmpConstant;
        else ++enginestats::current.oscAmpAudioRate;
    }
    if (needsAudioRate)
    {
        for (size_t i = 0; i < frames; ++i)
        {
            const float pitchCv =
                pitchNeedsAudioRate ? portSample(inst, 1, i) : (pitchAudioFed ? constantPitchCv : (pitchControlFed ? portSample(inst, 1, 0) : kCenterVolts));
            const float tuneVolts =
                tuneNeedsAudioRate ? portSample(inst, 2, i) : (tuneAudioFed ? constantTuneVolts : s.tuneVolts);
            const float hz = cv::hzFromPitchCv(pitchCv + (tuneVolts - kCenterVolts)) * s.detuneMul;
            // A genuine DSP edge on "amplitude" (e.g. an ADSR/LFO output) is
            // 0-10V CV like every other CV-carrying port (see
            // CvConventions.h) - vcaGain() turns it back into the 0..1
            // multiplier this additive amp expects. Unconnected (or
            // ControlRoute-only, e.g. MIDI velocity via the dedicated
            // s.amplitude field above) reads 0V here -> vcaGain(0)=0, i.e.
            // no change from before.
            const float amp = ampConstantForBlock ? ampConstantValue
                                                   : baseAmp + cv::vcaGain(ampBuf != nullptr ? ampBuf[i] : ampScalar, false);
            const float gainMul =
                gainFed ? cv::vcaGain(gainBuf[i], false) : (gainConverged ? gainTarget : s.gainSmooth.process(gainTarget));
            const float phaseOffset = feedbackActive ? s.feedbackSample * feedbackAmount : 0.0f;
            const float sample = s.phaseGen.process(wave, hz, kSampleRate, phaseOffset);
            if (feedbackActive)
                s.feedbackSample = sample;
            inst.outL[i] = sample * amp * gainMul * cv::kAudioPeakVolts;
        }
        return;
    }
    const float pitchCv = pitchAudioFed ? constantPitchCv : (pitchControlFed ? portSample(inst, 1, 0) : kCenterVolts);
    const float tuneVoltsConst = tuneAudioFed ? constantTuneVolts : s.tuneVolts;
    const float hz = cv::hzFromPitchCv(pitchCv + (tuneVoltsConst - kCenterVolts)) * s.detuneMul;
    // Phase increment precomputed once for the whole block (plan item 2):
    // advance() would otherwise redo this same `hz * invSampleRate` multiply
    // on every sample even though hz is constant for the entire block here.
    const float phaseIncrement = hz / kSampleRate;
    // Waveform dispatch happens exactly once here, for the whole block -
    // not once per sample the way PhaseOsc::process()'s internal `switch`
    // would (plan section 3, P1 item 3). See renderFastLoop()/FastLoopCtx
    // above.
    FastLoopCtx ctx{&inst,          &s.phaseGen,      &s.gainSmooth, ampBuf,
                    gainBuf,        ampScalar,        baseAmp,       ampConstantValue,
                    gainTarget,     phaseIncrement,   feedbackAmount, &s.feedbackSample,
                    gainFed,        gainConverged,    ampConstantForBlock};
    switch (wave)
    {
    case WaveType::Sine:
        renderSelectedFastLoop<WaveType::Sine>(ctx, frames, feedbackActive);
        break;
    case WaveType::Square:
        renderSelectedFastLoop<WaveType::Square>(ctx, frames, feedbackActive);
        break;
    case WaveType::Triangle:
        renderSelectedFastLoop<WaveType::Triangle>(ctx, frames, feedbackActive);
        break;
    case WaveType::Sawtooth:
        renderSelectedFastLoop<WaveType::Sawtooth>(ctx, frames, feedbackActive);
        break;
    case WaveType::Noise:
        renderSelectedFastLoop<WaveType::Noise>(ctx, frames, feedbackActive);
        break;
    case WaveType::PinkNoise:
        renderSelectedFastLoop<WaveType::PinkNoise>(ctx, frames, feedbackActive);
        break;
    }
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    inst.osc.wave = p.waveType;
    inst.osc.tuneVolts = p.tuneVolts;
    inst.osc.detuneCents = p.detune;
    inst.osc.detuneMul = powf(2.0f, p.detune / 1200.0f);
    inst.osc.amplitude = p.amplitude;
    inst.osc.gainCv = p.gain;
    inst.osc.feedbackCv = p.oscillatorFeedback;
    // Same cutoff/reason as Gain's amountSmooth - see NodeState.h.
    inst.osc.gainSmooth.setCutoff(300.0f, static_cast<float>(synth::kSampleRate));
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr)
    {
        return false;
    }
    if (strcmp(name, "tune") == 0)
    {
        inst.osc.tuneVolts = value;
        return true;
    }
    if (strcmp(name, "amplitude") == 0)
    {
        inst.osc.amplitude = value;
        return true;
    }
    if (strcmp(name, "detune") == 0)
    {
        inst.osc.detuneCents = value;
        inst.osc.detuneMul = powf(2.0f, value / 1200.0f);
        return true;
    }
    if (strcmp(name, "gain") == 0)
    {
        inst.osc.gainCv = value;
        return true;
    }
    if (strcmp(name, "feedback") == 0)
    {
        inst.osc.feedbackCv = value;
        return true;
    }
    return false;
}

inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.waveType = waveTypeFromString(presetparser::getString(p, "waveType", "sine"));
    // main.js's WIDGET_ENGINES.oscillator serializes the Tune slider under
    // the key "tune" (0-10V pitch-CV domain, 5V default).
    out.tuneVolts = presetparser::getFloat(p, "tune", 5.0f);
    out.detune = presetparser::getFloat(p, "detune", 0.0f);
    out.amplitude = presetparser::getFloat(p, "amplitude", 1.0f);
    out.gain = presetparser::getFloat(p, "gain", 10.0f);
    out.oscillatorFeedback = presetparser::getFloat(p, "feedback", 0.0f);
}

} // namespace nodes::oscillator
