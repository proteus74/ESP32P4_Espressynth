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
};

struct LfoState
{
    PhaseOsc phaseGen;
    WaveType wave = WaveType::Sine;
    float rate = 2.0f;
    // 0-10V (CV convention, see CvConventions.h) - the peak swing: in "auto"
    // zero-point mode the output ranges [0, amplitude] (center = amplitude/2,
    // e.g. amplitude=7 -> swings 0-7V, centered at 3.5V); in "fixed" mode the
    // swing is still +/-(amplitude/2) but centered on zeroPoint instead.
    // Default 10V + auto mode reproduces the old fixed-5V-center/full-swing
    // behavior exactly (halfSwing=5, center=5).
    float amplitude = 10.0f;
    // Phase offset in degrees (-180..180, see LFO_SLIDER_CONFIG in main.js) -
    // added on every evaluation (not written into phaseGen itself), so a sync
    // reset (phaseGen.resetPhase() -> phase_=0) automatically lands exactly
    // at this offset - see processNodeBlock's Lfo case.
    float phaseOffsetDeg = 0.0f;
    bool zeroPointFixed = false; // false = auto (center = amplitude/2)
    float zeroPoint = 5.0f;      // 0-10V, used only when zeroPointFixed
    // false (default) = unipolar CV convention (0-10V, see above). true =
    // bipolar audio convention (center 0V, swing +/-cv::kAudioPeakVolts) -
    // makes this Lfo behave like an audio-rate signal (see CvConventions.h)
    // instead of a CV signal; zeroPointFixed/zeroPoint are ignored in this
    // mode (center is always 0V). See LfoNode.h::render().
    bool bipolar = false;
    // Rising-edge state for the "sync" input (control-rate, see PortTables) -
    // a rising edge across kGateThresholdVolts restarts phaseGen from 0 (see
    // processNodeBlock's Lfo case).
    bool syncPrev = false;
    // One-shot: once armed by a sync edge, plays exactly one full cycle then
    // freezes (stops advancing) until the next sync edge re-arms it.
    // oneShotDone starts true so a freshly loaded/booted preset with oneShot
    // already enabled doesn't fire a phantom cycle before the first real
    // sync - see processNodeBlock's Lfo case.
    bool oneShot = false;
    bool oneShotDone = true;
    float oneShotAccum = 0.0f; // unwrapped phase accumulated since the last sync edge
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
};

struct EspEffectState
{
    void *handle = nullptr;
    espae::S32PlanarScratch scratch;
    // Extra interleaved/FIFO storage used by Sonic and source scratch used
    // by ESP Mixer. All allocated at graph build time.
    int32_t *extra[4] = {};
    float params[16] = {};
    uint8_t kind = 0;
    uint8_t inputCount = 2;
    bool ready = false;
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

// Free-running BPM pulse generator - mirrors main.js's Clock widget
// (fireClockPulse/restartClockTimer), but sample-accurate instead of per-JS
// setInterval. cvVolts (0-10V, "tempo" input) scales linearly between
// minBpm/maxBpm (see NodeInstance.cpp), phaseSec is the phase accumulator
// (0..period duration) instead of a FreeRTOS timer - hence does not drift
// and needs no periodic task switch.
struct ClockState
{
    float cvVolts = 0.0f;
    float minBpm = 0.0f;
    float maxBpm = 240.0f;
    float pulseMs = 100.0f;
    float phaseSec = 0.0f;
    bool pulseActive = false;
};

struct MixerState
{
    float inputGain[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    float outputGain = 1.0f;
};

// ChorusState is defined (state + render/build/destroy/setParam logic
// together) in nodes/ChorusNode.h.

// DelayState is defined (state + render/build/destroy/setParam logic
// together) in nodes/DelayNode.h.

// ReverbState/ReverbLightState are defined (state + render/build/destroy/
// setParam logic together) in nodes/ReverbNode.h.

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
