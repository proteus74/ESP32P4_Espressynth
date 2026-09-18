#pragma once

#include "DspPrimitives.h"
#include "EspAudioEffectsAdapter.h"
#include "NodeTypes.h"
#include "SynthConfig.h"

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
};

struct LfoState
{
    PhaseOsc phaseGen;
    WaveType wave = WaveType::Sine;
    float rate = 2.0f;
    float maxRateHz = 50.0f;
    // 0-10V (CV convention, see CvConventions.h) - the peak swing: in "auto"
    // zero-point mode the output ranges [0, amplitude] (center = amplitude/2,
    // e.g. amplitude=7 -> swings 0-7V, centered at 3.5V); in "fixed" mode the
    // swing is still +/-(amplitude/2) but centered on zeroPoint instead.
    // Default 10V + auto mode reproduces the old fixed-5V-center/full-swing
    // behavior exactly (halfSwing=5, center=5).
    float amplitude = 10.0f;
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
    // Global/per-voice switch: false (default, old behavior) = each voice
    // renders its own independent phase. true = voice 0 is authoritative, all
    // other voices of this widget simply mirror its already rendered output
    // (see NodeInstance.cpp's Lfo case and NodeInstance::primary). Only used
    // when the "mode" port (see PortTables.cpp) is not wired via DSP edge/
    // ControlRoute - otherwise its voltage (>=kGateThresholdVolts = global)
    // decides per block.
    bool global = false;
};

// cv (port "cv", index 0) in -> f1/f2 (outL/outR) out: two pitch-CV signals
// spread symmetrically around cv, offset by +/-(amount*maxVolts). 1V/oct is
// already logarithmic, so unlike the old Hz-domain Cent math this is a plain
// volt addition. amountTarget is the raw -1..1 knob value; amountSmooth is a
// one-pole ramp toward it (one step per block, see processNodeBlock's Detune
// case) so dragging the amount slider mid-note doesn't zipper the pitch.
struct DetuneState
{
    float amountTarget = 0.0f;
    OnePoleLowpass amountSmooth;
    float maxVolts = 0.5f;
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

struct GainState
{
    // Raw CV (0-10V, see CvConventions.h): 0V = silent, 5V = 0.5x, 10V =
    // unity gain. The actual multiplier is computed in NodeInstance.cpp via
    // cv::vcaGain() from this value + exponential.
    float amountCv = 10.0f;
    bool exponential = false;
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

// Equal-power stereo pan. Centre (5V) = hard center, see PanNode.h.
struct PanState
{
    float pan = 5.0f;
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

// Samples "in" exactly once for each below-to-at/above-threshold transition
// on "trigger", then keeps that CV at "out" until the next transition.
struct SampleHoldState
{
    float threshold = 5.0f;
    float heldValue = 0.0f;
    bool triggerWasAbove = false;
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

struct MidiInState
{
    int8_t channel = -1; // -1 = omni, else 0-based MIDI channel
    uint8_t voices = 1;
    OverflowMode overflowMode = OverflowMode::StealOldest;
    bool glide = false;
    float glideTime = 0.0f;
    // Pitch-bend range in cents (MIDI-standard default: 200 = 2 semitones).
    // Cents, not percent - matches cv's 1V/octave = 1200 cents/volt exactly.
    float pitchBendRangeCents = 200.0f;
    // When on, the pitch-bend CV snaps to the nearest whole cent (nearest
    // 1/1200 V) instead of a continuous voltage - so it always lands on a
    // voltage that means an exact number of cents, matching pitchCv's own
    // discrete-note steps, rather than a smooth in-between value.
    bool pitchBendQuantizeCents = false;
    bool sustainPedalDown = false; // runtime only - CC64 state, never persisted/loaded
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
