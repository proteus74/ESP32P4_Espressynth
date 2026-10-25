#pragma once

#include "DspPrimitives.h"
#include "EspAudioEffectsAdapter.h"
#include "NodeTypes.h"
#include "SynthConfig.h"

#include <cstdint>
#include <cstring>

// One POD-ish state struct per node type - see NodeInstance.h for how these
// are held (a flat struct, not a union, to keep lifetime/placement-new
// concerns out of code that can't be compile-tested interactively).

// tuneVolts: the widget's own "Tune" slider, expressed in the same 0-10V
// pitch-CV domain as the "pitchCv" input port (see CvConventions.h) - 5V is
// the center/no-transpose point. A connected "tune" CV input substitutes
// this value outright (same pattern as Filter's cutoffCv/resonanceCv), it
// does not add to it - see processNodeBlock's Oscillator case. Actual
// playback pitch is pitchCv (in, if connected, else center) + (tuneVolts -
// center), converted to Hz via cv::hzFromPitchCv().
// detuneCents/detuneMul are a separate, purely multiplicative fine-tune
// (unrelated to the Detune *node* in this file below) still delivered via
// ParamDispatch's "detune" field.
struct OscillatorState
{
    PhaseOsc phaseGen;
    WaveType wave = WaveType::Sine;
    float tuneVolts = 5.0f;
    float detuneCents = 0.0f;
    float detuneMul = 1.0f;
    float amplitude = 1.0f;
    bool gatePrev = false;
    // "gain" (Port-Index 4, real audio-rate port, see PortTables.cpp) -
    // 0-10V CV, 10V = unchanged output signal, 0V = silent (same
    // cv::vcaGain convention as Gain's "amount", see GainState). Scales the
    // finished oscillator sample multiplicatively, independent of the
    // additive "amplitude" envelope above. amountSmooth pattern like Gain:
    // only smoothed PER SAMPLE in the unwired case, to avoid a block-
    // constant jump (staircase/metallic sound) - see NodeInstance.cpp's
    // Oscillator case.
    float gainCv = 10.0f;
    OnePoleLowpass gainSmooth;
    // Internal one-sample feedback. feedbackSample stores the raw waveform
    // before amplitude/gain so loudness controls do not alter timbre.
    float feedbackCv = 0.0f;
    float feedbackSample = 0.0f;
};

namespace wavetable
{
struct Bank;
}

// Single-operator phase-modulation FM oscillator (see
// nodes/FmOscillatorNode.h). Carrier is a plain PhaseOsc, exactly like
// OscillatorState; "fmIn" (a modulator signal, typically another
// Oscillator's "value") is applied as PhaseOsc::process()'s phaseOffset
// argument each sample, scaled by fmIndexCv - no change to the carrier's hz
// itself, so this is genuine phase modulation, not frequency modulation.
// fmIndexCv follows the same smoothed-when-unwired pattern as gainCv above.
struct FmOscillatorState
{
    PhaseOsc phaseGen;
    WaveType wave = WaveType::Sine;
    float tuneVolts = 5.0f;
    float amplitude = 1.0f;
    bool gatePrev = false;
    float gainCv = 10.0f;
    OnePoleLowpass gainSmooth;
    // "fmIndex" (Port-Index 6, control-only CV, see PortTables.h) - 0-10V,
    // 0V = no FM (node sounds identical to a plain Oscillator), 10V = max
    // phase deviation (see kFmIndexMaxPhaseDeviation in FmOscillatorNode.h).
    // Read once per block, same un-smoothed substitution pattern as
    // tuneVolts above - not per-sample like gainCv, since it's classified
    // control-only (see PortTables.h::isControlOnlyInput).
    float fmIndexCv = 0.0f;
    float feedbackCv = 0.0f;
    float feedbackSample = 0.0f;
    // "dutyCycle" (Port-Index 8, control-only CV, see PortTables.h) - 0-10V,
    // 5V (center, same default convention as tuneVolts) = classic 50%
    // square, only audible when wave==Square (see PhaseOsc::sampleAt's
    // dutyThresholdInt in DspPrimitives.h). Read once per block, same
    // un-smoothed substitution pattern as fmIndexCv above.
    float dutyCycleCv = 5.0f;
    // SuperSaw-only controls (ports 9/10). Center voice remains phaseGen;
    // these six compact accumulators hold the three detuned voice pairs.
    float superSawDetuneCv = 5.0f;
    float superSawMixCv = 5.0f;
    uint32_t superSawSidePhase[6] = {};
    // WaveType::Wavetable: the widget's bank (shared by all voices, counted reference - see
    // Wavetable.h) and the manual morph position 0-10V, overridden by the "morph" port (12).
    wavetable::Bank *wavetableBank = nullptr;
    float morphCv = 0.0f;
    // DX7-style frequency ratio on top of Tune (ports 13/14, control-only, 0-10V): coarse snaps
    // to 0.5, 1, 2 ... 31 (32 equal steps), fine adds 0..99 %. Default = 1:1. Ignored while a
    // cable/route feeds "tune" (see FmOscillatorNode.h's ratioVolts()).
    float ratioCoarseCv = 10.0f / 31.0f;
    float ratioFineCv = 0.0f;
    // "detune" (port 15, control-only 0-10V, 5V = none): +-10 cents, see detuneVoltsFromCv().
    float detuneCv = 5.0f;
};

namespace wavetable
{
struct Bank;
}

struct LfoState
{
    PhaseOsc phaseGen;
    // LFO delay: seconds to fade the swing in after a reset edge (0 = off), and the time since then.
    float delaySec = 0.0f;
    float delayElapsed = 0.0f;
    WaveType wave = WaveType::Sine;
    float rate = 2.0f;
    float maxRateHz = 50.0f;
    // "Gain", 0-10V (CV convention, see CvConventions.h) - the peak swing: in
    // "auto" zero-point mode the output ranges [0, gain] (center = gain/2,
    // e.g. gain=7 -> swings 0-7V, centered at 3.5V); in "fixed" mode the
    // swing is still +/-(gain/2) but centered on zeroPoint instead. Default
    // 10V + auto mode = full 0-10V swing (halfSwing=5, center=5).
    float gain = 10.0f;
    // Phase offset in degrees (-180..180, see LFO_SLIDER_CONFIG in main.js) -
    // added on every evaluation (not written into phaseGen itself), so a
    // reset (phaseGen.resetPhase() -> phase_=0) automatically lands exactly
    // at this offset - see processNodeBlock's Lfo case.
    float phaseOffsetDeg = 0.0f;
    bool zeroPointFixed = false; // false = auto (center = amplitude/2)
    float zeroPoint = 5.0f;      // 0-10V, used only when zeroPointFixed
    // Rising-edge state for the "reset" input (control-rate, see PortTables) -
    // a rising edge across kGateThresholdVolts restarts phaseGen from 0 (see
    // processNodeBlock's Lfo case).
    bool resetPrev = false;
    // One-shot: once armed by a reset edge, plays exactly one full cycle then
    // freezes (stops advancing) until the next reset edge re-arms it.
    // oneShotDone starts true so a freshly loaded/booted preset with oneShot
    // already enabled doesn't fire a phantom cycle before the first real
    // reset - see processNodeBlock's Lfo case.
    bool oneShot = false;
    bool oneShotDone = true;
    float oneShotAccum = 0.0f; // unwrapped phase accumulated since the last reset edge
    // LinearUp/LinearDown position 0..1: wraps while free-running, runs once
    // from the last reset edge and holds at 1 in One Shot (phaseGen wraps, so
    // it can't carry the "finished" state).
    float linearPos = 0.0f;
    // Global mode bookkeeping, only meaningful on voice 0 of a multi-voice
    // widget and written by nodes::lfo::globalUpdate() (Graph::renderBlock):
    // globalManaged = Graph currently drives this LFO as the shared master
    // (its own reset input is then ignored, the OR over all voices is used);
    // globalRunning = at least one voice sounded last block (its rising edge
    // is the "first note" that (re)starts the shared LFO); globalResetPrev =
    // previous block's OR of all voices' reset inputs.
    bool globalManaged = false;
    bool globalRunning = false;
    bool globalResetPrev = false;
    // Global/per-voice switch: false (default, old behavior) = each voice
    // renders its own independent phase. true = voice 0 is authoritative, all
    // other voices of this widget simply mirror its already rendered output
    // (see NodeInstance.cpp's Lfo case and NodeInstance::primary). Only used
    // when the "mode" port (see PortTables.cpp) is not wired via DSP edge/
    // ControlRoute - otherwise its voltage (>=kGateThresholdVolts = global)
    // decides per block.
    bool global = false;
    // Wavetable widget only (NodeType::Wavetable, see nodes/WavetableNode.h
    // - it reuses LfoState for its phase/reset/one-shot/global machinery).
    // `morph` is the manual 0-10V fallback for its "morph" port (index 3,
    // audio-rate). `wavetableBank` is shared by all voices of the widget and
    // reference counted (acquired in build(), released in Graph::destroy()).
    float morph = 0.0f;
    wavetable::Bank *wavetableBank = nullptr;
};

// cv (port "cv", index 0) in -> out (outL) out: one pitch-CV signal, cv plus
// a settings-dialog-scaled offset - same "knob's full 0-10V travel rescaled
// to +/-maxOffset, then rounded to a step grid" convention as
// StepArpeggiator's per-step pitch knob (nodes/StepArpeggiatorNode.h's
// triggerStep): amountTarget's own 5V center means 0 offset, 0V/10V mean
// -/+maxOffsetCents. stepCents is the rounding grid - 100 for semitone
// steps, 1 for cent steps (settings-dialog choice) - so the result always
// lands on a musically exact step, never an arbitrary knob-position
// fraction. amountSmooth is a one-pole ramp toward amountTarget (one step
// per block, see processNodeBlock's Detune case) so dragging the amount
// slider mid-note doesn't zipper the pitch.
struct DetuneState
{
    float amountTarget = 5.0f;
    OnePoleLowpass amountSmooth;
    float maxOffsetCents = 1200.0f;
    float stepCents = 100.0f;
};

struct FrequencyDividerState
{
    float divider = 2.0f;
    // Frequency / N equals pitch CV - log2(N). Cached for the normal case
    // where the divider knob is fixed, so renderFrequencyDivider never
    // evaluates log2f() in its audio loop.
    float pitchOffsetVolts = 1.0f;
};

// Audio-page links retain six extra output buffers for buses 3-8. Buses 1
// and 2 use NodeInstance::outL/outR, so this state is allocated only for
// actual link widgets instead of making every polyphonic node instance grow.
struct LinkState
{
    float *busOut[synth::kLinkBusCount - 2] = {};
    uint8_t activeBusCount = 8;
    // Audio Outputs' per-bus level controls. Raw 0-10V CV; 10V is unity.
    float gainCv[synth::kLinkBusCount] = {10, 10, 10, 10, 10, 10, 10, 10};
};

// One midi-split zone: an inclusive note range [fromNote, toNote] plus its
// own whole-octave transpose, applied to any note landing inside it.
struct MidiSplitZone
{
    int8_t fromNote = 0;
    int8_t toNote = 127;
    int8_t octaveTranspose = 0;
};

// midi-in's zone-split counterpart: routes each incoming note to the first
// zone (of up to 4) whose [fromNote, toNote] range contains it, applying
// that zone's whole-octave transpose to the note/frequency it forwards. A
// note falling in a gap between zones is silently dropped (no forward). No
// DSP state of its own - see Graph::deliverNoteControlRoutes's MidiSplit
// case for how these get used.
struct MidiSplitState
{
    uint8_t zoneCount = 0; // starts empty - no zone until the user adds one
    MidiSplitZone zones[4];
};

// AdsrState is defined (state + render/build/destroy/setParam logic
// together) in nodes/AdsrNode.h.

// FilterState is defined (state + render/build/destroy/setParam logic
// together) in nodes/FilterNode.h.

// First ESP_AUDIO_EFFECTS-backed node. One peaking band keeps the initial
// preset/UI contract small; the wrapper can later grow into a filter bank
// without changing the graph's stereo ports or S32 conversion boundary.
struct EspEqState
{
    void *handle = nullptr;
    espae::S32PlanarScratch scratch;
    float frequencyHz = 1000.0f;
    float q = 1.0f;
    float gainDb = 0.0f;
    EqFilterKind filterType = EqFilterKind::Peak;
    bool ready = false;
    bool bypass = false;
};

// Second ESP_AUDIO_EFFECTS-backed node - an 8-band graphic EQ. Same S32
// scratch/handle plumbing as EspEqState, just filter_num=8 instead of 1 (see
// GraphicEqEffect.h). Q is fixed per band (not user-editable, no port for
// it); only each band's gain (settings-fixed frequency, CV/slider-editable
// gain) and a final output trim are live-controllable.
struct GraphicEqState
{
    static constexpr int kBandCount = 8;
    void *handle = nullptr;
    espae::S32PlanarScratch scratch;
    float frequencyHz[kBandCount] = {63.0f, 125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 14000.0f};
    float gainDb[kBandCount] = {};
    // CV mapping for the "bandN" ports (see PortTables.h / NodeRender.h).
    float bandMin[kBandCount] = {};
    float bandMax[kBandCount] = {};
    bool bandLog[kBandCount] = {};
    float outputGainCv = 10.0f; // 0-10V, 10V = unity (cv::vcaGain)
    bool ready = false;
    bool bypass = false;
};

struct GainState
{
    // "amount" (Output Gain). Raw CV (0-10V, see CvConventions.h): 0V =
    // silent, 5V = 0.5x, 10V = unity gain. The actual multiplier is computed
    // via cv::vcaGain() from this value.
    float amountCv = 10.0f;
    // "curve" - independent second VCA stage. Raw CV (0-10V): 0V = silent,
    // 5V = unity (1x), 10V = 2x. See cv::curveGain(). Multiplies onto
    // "amount" rather than shaping it (replaces the former linear/
    // exponential toggle).
    float curveCv = 5.0f;
    // Global/per-voice switch, same pattern as LfoState::global above:
    // false (default) = each voice applies its own "amount" CV to its own
    // "in" signal (current behavior). true = voice 0's "amount" (input port
    // OR manual amountCv scalar) applies to all voices of this widget - "in"
    // always remains each voice's own signal, only the gain is shared (see
    // NodeInstance.cpp's Gain case).
    bool global = false;
    // Smooths the multiplier computed via vcaGain() PER SAMPLE (not block-
    // wise like DetuneState.amountSmooth - see NodeInstance.cpp for the
    // reasoning): without it an amountCv change would make the multiplier
    // jump hard (click), and even block-wise smoothed a ~344Hz staircase
    // function would remain, modulating onto the signal as sidebands/
    // metallic sound - Gain multiplies directly onto every sample, unlike
    // Detune's pure pitch-CV smoothing.
    OnePoleLowpass amountSmooth;
    // Same per-sample smoothing as amountSmooth above, for "curve" when it's
    // manual/unwired.
    OnePoleLowpass curveSmooth;
};

struct BitcrusherState
{
    float crush = 5.0f;
    float heldSample = 0.0f;
    uint32_t holdCounter = 0;
    bool bypass = false;
};

// Wave Folder controls use the shared 0-10V CV convention. Symmetry's
// centre is 5V (0V DC offset); Drive starts at 0V (1x); Gain/Mix at 10V.
struct WaveFolderState
{
    float drive = 0.0f;
    float symmetry = 5.0f;
    float gain = 10.0f;
    float mix = 10.0f;
};

// Overdrive/Distortion controls (0-10V CV) plus DSP memory. The drive factor
// and filter coefficients are cached and recomputed only when their raw CV
// changes, see OverdriveNode.h.
struct OverdriveState
{
    float mode = 0.0f;
    float drive = 5.0f;
    float tone = 7.0f;
    float bias = 5.0f;
    float mix = 10.0f;
    float gain = 10.0f;

    float prevIn = 0.0f; // last shaper input (2x oversampling)
    float preLp = 0.0f;  // Soft mode's 720 Hz pre-emphasis split
    float dcX1 = 0.0f, dcY1 = 0.0f;
    float toneZ1 = 0.0f;
    float inPow = 0.0f, wetPow = 0.0f; // auto-level power trackers

    float cachedDriveCv = -1.0f, cachedToneCv = -1.0f;
    float driveFactor = 1.0f, toneCoeff = 1.0f;
};

// Equal-power stereo pan. Centre (5V) = hard center, see PanNode.h.
struct PanState
{
    float pan = 5.0f;
};

// Envelope Follower: attack/release/preGain all use the shared 0-10V CV
// convention, mapped via cv::cvToRange() with a log curve (see
// EnvelopeFollowerNode.h) - 5V (the default for all three) lands at
// attack≈22ms, release≈141ms, preGain=1.0x exactly (geometric centre of
// 0.1x..10x). `envelope` is the running one-pole state carried across
// blocks (not reset between blocks, unlike the CV controls above).
struct EnvelopeFollowerState
{
    float attackCv = 5.0f;
    float releaseCv = 5.0f;
    float preGainCv = 5.0f;
    float envelope = 0.0f;
    // Settings-dialog field (main.js's envelopeFollowerCoeffStrideSelect),
    // same idea/range as FilterState::coeffStride: when attack/release/
    // preGain is genuinely audio-rate-fed, the (expf()-based) attack/
    // release coefficients only get recomputed every coeffStride-th sample
    // instead of every sample - envelope tracking itself (the cheap part)
    // still runs every sample regardless. Default 4.
    uint8_t coeffStride = 4;
    // Cached derived values (see FilterState::coeffsDirty/audioCoeffsInitialized).
    float coeffAttack = 0.0f;
    float coeffRelease = 0.0f;
    float preGain = 1.0f;
    bool coeffsDirty = true;
};

// Free-running pulse generator, one per Clock-type node instance - mirrors
// main.js's Clock widget (fireClockPulse/restartClockTimer), but
// sample-accurate instead of per-JS setInterval. bpm itself is NOT stored
// here anymore: every instance reads the single device-wide
// synth::masterClock() (MasterClock.h) so all Clock outputs stay in tempo
// with each other; only pulseMs (this instance's own pulse width) and the
// phase accumulator are per-instance. phaseSec is 0..period duration instead
// of a FreeRTOS timer - hence does not drift and needs no periodic task
// switch.
struct ClockState
{
    float pulseMs = 100.0f;
    float phaseSec = 0.0f;
    bool pulseActive = false;
};

// Burst Generator / Trigger Delay: on a rising edge of "in", either fires a
// retriggered burst of "count" fast, decaying pulses spaced "time" ms apart
// (drum-roll/complex-modulation use), or a single pulse "time" ms after the
// edge (trigger delay). All four params use the shared 0-10V CV convention;
// "mode" is a plain threshold switch (<5V burst, >=5V delay) exactly like
// Delay's bypass (see DelayNode.h/DelayState) rather than a string preset
// field, so it stays live-modulatable/CV-controllable like every other knob
// here. pendingSamples/pendingAmplitude is a small unordered set of
// scheduled-but-not-yet-fired pulses (swap-removed as they fire, see
// BurstGeneratorNode.h::render) - burst mode schedules up to kMaxPending at
// once on the triggering edge, delay mode always schedules exactly one.
struct BurstGeneratorState
{
    float modeCv = 0.0f;
    float countCv = 5.0f;
    float timeCv = 5.0f;
    float decayCv = 5.0f;

    static constexpr uint8_t kMaxPending = 8;
    uint32_t pendingSamples[kMaxPending] = {};
    float pendingAmplitude[kMaxPending] = {};
    uint8_t pendingCount = 0;

    bool prevTriggerAbove = false;
    uint32_t outSamplesLeft = 0;
    float outAmplitude = 0.0f;
};

// CV In (ADS1115) / CV Out (GP8413) - External Hardware widgets. Port/address/channel are
// resolved once in build() into a Feature::IO slot index (see nodes/CvInNode.h/CvOutNode.h);
// re-resolving here would need a FreeRTOS include in this otherwise plain-POD state header,
// so only the small resolved int is cached. CvInState::lastVolts is only updated when the
// I2C bus manager's latest sample also reports found=true, so one dropped transaction doesn't
// snap the output to 0 - see NodeInstance::ioDeviceFound for the separate found/missing signal.
struct CvInState
{
    int8_t port = -1;
    uint8_t address = 0;
    uint8_t channel = 0;
    int8_t slotIndex = -1; // Feature::IO::resolve_cv_in_slot() result, -1 = unconfigured
    float lastVolts = 0.0f;
};
struct CvOutState
{
    int8_t port = -1;
    uint8_t address = 0;
    uint8_t channel = 0;
    int8_t slotIndex = -1; // Feature::IO::resolve_cv_out_slot() result, -1 = unconfigured
};

// Digital In/Out (raw GPIO) - External Hardware widgets. Pin is resolved once in build()
// against Feature::IO::gpioSettings() (see nodes/DigitalInNode.h/DigitalOutNode.h). Unlike
// CV, there's no bus-manager slot/mailbox indirection - render() calls
// Feature::IO::gpio_read_level()/gpio_write_level() directly every block, since a GPIO
// register op is cheap enough not to need a separate task (see features/io/gpio_manager.hpp).
struct DigitalInState
{
    int8_t pin = -1;
    bool lastLevel = false;
    bool invert = false; // cached from Feature::IO::GpioSettings at build() time
};
struct DigitalOutState
{
    int8_t pin = -1;
    bool invert = false; // cached from Feature::IO::GpioSettings at build() time
};

// RGB LED - maps its input voltage through a sorted stop list to a 0xRRGGBB color, which
// render() hands to Feature::LedStrip (see nodes/RgbLedNode.h). The browser computes its own
// on-widget color from its preview engine - nothing is reported back over the network.
constexpr uint8_t kRgbLedMaxStops = 8;
struct RgbLedState
{
    int16_t ledIndex = 0;
    uint8_t stopCount = 0;
    float stopVolts[kRgbLedMaxStops] = {};
    uint32_t stopColor[kRgbLedMaxStops] = {};
    bool stopFade[kRgbLedMaxStops] = {};
};

// Samples "in" exactly once for each below-to-at/above-threshold transition
// on "trigger", then keeps that CV at "out" until the next transition.
struct SampleHoldState
{
    float threshold = 5.0f;
    float heldValue = 0.0f;
    bool triggerWasAbove = false;
};

struct CounterState
{
    uint32_t count = 0;
    bool countWasHigh = false;
};

struct MonoflopState
{
    float threshold = 5.0f;
    float pulseMs = 10.0f;
    uint32_t samplesLeft = 0;
    bool wasAbove = false;
};

struct MixerState
{
    float inputGain[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    float outputGain = 1.0f;
};

// CV-selectable N-of-M (2-16) audio passthrough, state+render/build/setParam
// logic together in nodes/MultiplexerNode.h. manualSelect is the widget's own
// vertical-slider override (0-based) - a genuine "select" CV connection
// substitutes it outright, same "connected CV wins" convention as
// Oscillator's "tune"/Filter's cutoff (see nodes/MultiplexerNode.h::render).
struct MultiplexerState
{
    uint8_t inputCount = 2;
    uint8_t manualSelect = 0;
    float softFadeMs = 5.0f;
    uint8_t activeIndex = 0;
    uint8_t fadeFrom = 0;
    uint8_t fadeTo = 0;
    uint32_t fadeSamples = 0;
    uint32_t fadePosition = 0;
};

// Mirror image of MultiplexerState: one audio input routed to whichever of
// up to kOutputMultiplexerMaxOutputs outputs is currently selected (same
// "select" CV convention as MultiplexerState::manualSelect/inputCount, just
// applied to the output side) - see nodes/OutputMultiplexerNode.h. No
// soft-fade (unlike Multiplexer): the point of this node is that every
// non-selected output holds its own last value rather than crossfading, so
// switching the active output is instantaneous. held[] is per-output,
// updated every sample while that output is active and frozen otherwise -
// persisted into the preset (WidgetParams::outputMuxHeld) so a reload shows
// the same values a viewer/downstream node last saw, not silence.
constexpr uint8_t kOutputMultiplexerMaxOutputs = 16;
struct OutputMultiplexerState
{
    uint8_t outputCount = 2;
    uint8_t manualSelect = 0;
    uint8_t activeIndex = 0;
    float held[kOutputMultiplexerMaxOutputs] = {};
    // Ports 0/1 are this instance's own outL/outR (see outputPortBuffer() in
    // NodeInstance.h); this array holds ports 2.. only - same split LinkState
    // ::busOut uses for AudioOutputLink/AudioInputLink's extra buses.
    float *outPorts[kOutputMultiplexerMaxOutputs - 2] = {};
};

struct DrumSequencerPattern
{
    uint8_t active[(synth::kDrumSequencerMaxCells + 7) / 8] = {};
    uint8_t velocity[synth::kDrumSequencerMaxCells] = {};
};

struct DrumSequencerState
{
    uint8_t stepCount = 16;
    uint8_t trackCount = 1;
    uint16_t currentStep = 0;
    bool clockPrev = false;
    bool gatePrev = false;
    float trackPulseMs[synth::kDrumSequencerMaxTracks] = {50, 50, 50, 50, 50, 50, 50, 50};
    float internalClockCvVolts = 0.0f;
    float internalClockMaxBpm = 120.0f;
    bool internalClockRoundBpm = false;
    float internalPhaseSec = 0.0f;
    DrumSequencerPattern patterns[synth::kDrumSequencerMaxPatterns];
    uint8_t patternCount = 1;             // 1..kDrumSequencerMaxPatterns
    uint8_t activePattern = 0;            // pattern currently playing/edited
    uint8_t manualPatternSelect = 0;      // requested index when "pattern" port is unwired
    bool switchAtEnd = false;             // "switchAtEnd" port unwired fallback: false = swap immediately, true = swap at step==0 wrap
    uint8_t pendingPattern = 0xFF;        // 0xFF = no deferred switch armed
    float heldVelocity[synth::kDrumSequencerMaxTracks] = {};
    uint32_t pulseSamplesRemaining[synth::kDrumSequencerMaxTracks] = {};
    uint32_t pendingPulseSamples[synth::kDrumSequencerMaxTracks] = {};
    uint8_t retriggerGapRemaining[synth::kDrumSequencerMaxTracks] = {};
    float *outPorts[synth::kDrumSequencerMaxTracks * 2 - 2] = {};
};

// ChorusState is defined (state + render/build/destroy/setParam logic
// together) in nodes/ChorusNode.h.

// DelayState is defined (state + render/build/destroy/setParam logic
// together) in nodes/DelayNode.h.

// ReverbLightState is defined (state + render/build/destroy/setParam logic
// together) in nodes/ReverbNode.h.

// HammondState is defined (state + render/build/destroy/setParam logic
// together) in nodes/HammondNode.h.

struct AudioOutState
{
    SoftLimiter limiter;
    float leftLevel = 1.0f;
    float rightLevel = 1.0f;
    float gain = 10.0f;
    bool muted = false;
    float muteSmoothed = 1.0f;
};

// vu-meter / oscilloscope: no web consumer wired up yet.
struct MeterState
{};

// "keyMask": 32 hex characters, byte i = notes 8i..8i+7 (bit 0 = lowest), high nibble first.
// Returns false and leaves `out` untouched when the string is missing or malformed.
inline bool parseKeyMaskHex(const char *hex, uint32_t out[4])
{
    if (hex == nullptr)
        return false;
    uint32_t words[4] = {0, 0, 0, 0};
    for (int i = 0; i < 32; ++i)
    {
        const char c = hex[i];
        uint32_t v;
        if (c >= '0' && c <= '9')
            v = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f')
            v = static_cast<uint32_t>(10 + c - 'a');
        else if (c >= 'A' && c <= 'F')
            v = static_cast<uint32_t>(10 + c - 'A');
        else
            return false; // also catches a string shorter than 32 characters
        const int byteIdx = i / 2;
        const int shift = (byteIdx % 4) * 8 + ((i & 1) != 0 ? 0 : 4);
        words[byteIdx / 4] |= v << shift;
    }
    memcpy(out, words, sizeof(words));
    return true;
}

struct MidiInState
{
    int8_t channel = -1; // -1 = omni, else 0-based MIDI channel
    uint8_t voices = 1;
    OverflowMode overflowMode = OverflowMode::StealOldest;
    bool glide = false;
    float glideTime = 0.0f;
    // Pitch-bend range in cents (MIDI-standard default: 200 = 2 semitones).
    // Stored in cents for preset compatibility; UI displays ±V, centered at 0V.
    // 1V/octave = 1200 cents/volt; maximum ±5V (6000 cents).
    float pitchBendRangeCents = 200.0f;
    // When on, the pitch-bend CV snaps to the nearest whole cent (nearest
    // 1/1200 V) instead of a continuous voltage - so it always lands on a
    // voltage that means an exact number of cents, matching pitchCv's own
    // discrete-note steps, rather than a smooth in-between value.
    bool pitchBendQuantizeCents = false;
    // Last Program Change (0-127) received on this Midi-In's channel; drives the "program"
    // output. Persisted as the "program" param so a graph rebuild does not reset it to 0.
    float program = 0.0f;
    // Keys this Midi-In reacts to (bit n of the 128-bit mask = MIDI note n). Default: all keys.
    // Only note-ons are filtered; a note-off always passes so nothing can get stuck.
    uint32_t keyMask[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    bool noteAllowed(uint8_t note) const
    {
        return note > 127 || ((keyMask[note >> 5] >> (note & 31)) & 1u) != 0;
    }
    bool sustainPedalDown = false; // runtime only - CC64 state, never persisted/loaded
    // "voices" CV port (runtime only, see nodes::midiin::render): candidate voice count and
    // for how many blocks it has been stable, plus the last count already requested.
    uint8_t voicesCvCandidate = 0;
    uint16_t voicesCvStableBlocks = 0;
    uint8_t voicesCvRequested = 0;
};

struct ConstState
{
    float value = 0.0f;
};

// KnobState is defined (state + render/build/destroy/setParam logic
// together) in nodes/KnobNode.h.

// ButtonState/UpDownButtonState are defined (state + render/build/destroy/
// setParam logic together) in nodes/ButtonNode.h.

// CC-learnable selector: direct value mapping (CC 0 -> In 1, CC >=64 -> In
// 2, no press-edge/toggle), which chooses whether "in1" or "in2" (both real
// audio-rate ports, see PortTables) reaches "value" - see
// processNodeBlock's Switch case. Hard switch, no crossfade.
struct SwitchState
{
    int16_t cc = -1;
    int8_t channel = -1;
    bool selectIn2 = false;
    // Settings-dialog-configured fallback used instead of the usual 0V
    // default whenever the SELECTED input has no DSP edge (see
    // nodes/SwitchNode.h's render()) - lets a patch route "no cable on In 1/
    // In 2" to a deliberate value instead of silence/0.
    float in1Default = 0.0f;
    float in2Default = 0.0f;
};

// Mirror image of SwitchState: one audio input routed to whichever of two
// outputs is currently selected (same MIDI-CC-learnable toggle convention as
// SwitchState::selectIn2/cc/channel) - see nodes/OutputSwitchNode.h. held1/
// held2 freeze the NON-selected output at its own last value instead of the
// fixed configured fallback Switch's in1Default/in2Default use - persisted
// into the preset (WidgetParams::outputSwitchHeld1/2) so a reload shows the
// same values a downstream node last saw, not 0V.
struct OutputSwitchState
{
    int16_t cc = -1;
    int8_t channel = -1;
    bool selectOut2 = false;
    float held1 = 0.0f;
    float held2 = 0.0f;
};

// Per-voice one-shot sample playback. sampleData/frameCount/channels point
// into a widget-owned buffer decoded ONCE at graph-build time (see
// Graph::waveSampleCache / GraphBuilder's initWavePlayer) - never allocated
// or freed per-instance, so N voice instances of the same widget can share
// it safely.
struct WavePlayerState
{
    const int16_t *sampleData = nullptr; // interleaved stereo PCM16
    uint32_t frameCount = 0;
    uint8_t channels = 0;

    int rootNoteMidi = 60; // C4 (scientific pitch notation, A4=69)

    float position = 0.0f; // fractional playback position, in source frames
    bool gatePrev = false;
    bool playing = false; // true from gate rising-edge until frameCount is reached (one-shot)

    // 20ms linear release fade so gate-off never produces a hard-edge
    // discontinuity (audible click) - snapped to 1 on retrigger, ramps to 0
    // while gate stays low. Independent of "amplitude" (multiplied on top of
    // it, see processNodeBlock) - playback itself still runs to the
    // sample's natural end regardless of gate, only volume is shaped.
    float fadeGain = 1.0f;

    // "gain" (real audio-rate port) - 0-10V CV, 10V = unchanged output
    // signal, same cv::vcaGain convention as Oscillator's/SoundFontPlayer's
    // "gain". Scales the finished stereo sample multiplicatively, on top of
    // "amplitude" and fadeGain. gainSmooth: only smoothed PER SAMPLE in the
    // unwired case, to avoid a block-constant jump.
    float gainCv = 10.0f;
    OnePoleLowpass gainSmooth;
};

// Per-voice pitch-CV sample playback with start/end and a sustain loop (nodes/SamplerNode.h).
// Shares WavePlayer's widget-owned, build-time-decoded buffer (Graph::waveSampleCache).
// Markers are fractions 0..1 of the sample length, so they survive the load-time resample.
struct SamplerState
{
    const int16_t *sampleData = nullptr; // interleaved stereo PCM16
    uint32_t frameCount = 0;

    int rootNoteMidi = 60;
    float start = 0.0f;
    float end = 1.0f;
    float loopStart = 0.0f;
    float loopEnd = 1.0f;
    bool loop = false;

    // Integer frame + fraction: a single float loses sub-frame precision past ~1M frames.
    uint32_t index = 0;
    float frac = 0.0f;
    bool gatePrev = false;
    bool playing = false;
    bool heard = false; // amplitude*gain was audible since the last gate - see SamplerNode.h
    // Retrigger while playing: the previous read position, faded out over fadeRemaining frames.
    uint32_t fadeIndex = 0;
    float fadeFrac = 0.0f;
    uint16_t fadeRemaining = 0;

    float gainCv = 10.0f;
    OnePoleLowpass gainSmooth;
};

struct tsf;

// One playback state per graph voice. tsf_copy() shares the immutable decoded
// SoundFont bank while keeping envelopes/active notes independent.
struct SoundFontPlayerState
{
    tsf *synth = nullptr;
    int presetIndex = -1;
    int bank = 0;
    int program = 0;
    bool enableCc = false;
    bool enableModWheel = false;
    bool enablePitchWheel = false;
    int currentNote = 60;
    bool gatePrev = false;
    bool pendingNoteOn = false;
    // "gain" (real audio-rate port, see PortTables.cpp) - 0-10V CV, 10V =
    // unchanged output signal, same cv::vcaGain convention as Oscillator's
    // "gain" (see OscillatorState). Scales the finished rendered stereo
    // signal multiplicatively. gainSmooth pattern like Oscillator: only
    // smoothed PER SAMPLE in the unwired case, to avoid a block-constant
    // jump.
    float gainCv = 10.0f;
    OnePoleLowpass gainSmooth;
};

// Per-widget state for the MDA Piano generator. The engine pointer remains
// opaque here so NodeState.h does not depend on the GPL DSP component.
struct MdaPianoState
{
    void *engine = nullptr;
    bool enableModWheel = false;
    bool enablePitchWheel = false;
    int currentNote = 60;
    bool gatePrev = false;
    bool pendingNoteOn = false;
    float pendingVelocity = 1.0f;
    float modWheel = 1.0f;
    int pitchBend14 = 8192;
    float voiceTypeCv = 0.0f;
    float driveCv = 0.0f;
    float tremWahModeCv = 0.0f;
    float tremWahDepthCv = 0.0f;
    float tremWahRateCv = 5.0f;
    float choPhaModeCv = 0.0f;
    float choPhaDepthCv = 0.0f;
    float choPhaSpeedCv = 5.0f;
    float preGainCv = 10.0f;
    float gainCv = 10.0f;
    OnePoleLowpass gainSmooth;
};
