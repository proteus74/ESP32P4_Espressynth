#pragma once

#include <cstddef>
#include <cstdint>

#include "NodeState.h"
#include "NodeTypes.h"
#include "SynthConfig.h"

// Full definitions + all logic for these heap-allocated pointer members live
// in nodes/*.h (only need to be forward-declared here). The using-
// declarations bring them into the global namespace under their established
// bare names, matching every other state type's naming.
namespace nodes::valuemapper
{
struct ValueMapperState;
}
using nodes::valuemapper::ValueMapperState;

namespace nodes::knob
{
struct KnobState;
}
using nodes::knob::KnobState;

namespace nodes::button
{
struct ButtonState;
struct UpDownButtonState;
}
using nodes::button::ButtonState;
using nodes::button::UpDownButtonState;

namespace nodes::adsr
{
struct AdsrState;
}
using nodes::adsr::AdsrState;

namespace nodes::filter
{
struct FilterState;
}
using nodes::filter::FilterState;

namespace nodes::chorus
{
struct ChorusState;
}
using nodes::chorus::ChorusState;

namespace nodes::delay
{
struct DelayState;
}
using nodes::delay::DelayState;

namespace nodes::reverb
{
struct ReverbState;
struct ReverbLightState;
}
using nodes::reverb::ReverbState;
using nodes::reverb::ReverbLightState;

namespace nodes::hammond
{
struct HammondState;
}
using nodes::hammond::HammondState;

// One instance of one widget (a voiced widget has the preset's configured
// voice count, a global widget has exactly 1). Flat struct (not a union of
// state types) - costs a bit more memory than the minimum, but keeps
// allocation and lifetime trivial (plain `new`/`delete`, real constructors
// run, default member initializers apply) with no placement-new/manual-
// lifetime subtleties for most of the ~25 per-type payloads below. The
// thirteen largest ones (chorus/delay/reverb/reverbLight/espEffect/hammond,
// plus adsr/filter/espEq/knob/button/upDownButton/valueMapper) are the
// deliberate exception - heap-allocated only for a matching instance, see
// their comment below - because leaving them as flat value members like the others made
// every instance of every type pay their combined size regardless of whether
// it ever used them. The last four (knob/button/upDownButton/valueMapper)
// are dominated by their kValueListMax-sized valueList[] array (64 B each) -
// small individually, but paid by every Oscillator/ADSR/Filter/... instance
// too when left as value members, since only actual Knob/Button/... widgets
// ever populate them.
struct NodeInstance
{
    NodeType type = NodeType::Unknown;
    uint16_t widgetIndex = 0;
    uint8_t voiceIndex = 0;
    uint8_t audioInPortCount = 0;

    synth::PortMask controlConnectedPortMask = 0;

    // Which audio-input ports (bit i = inPorts[i]) actually have an incoming
    // preset edge - set once in GraphBuilder::build(). Lets Graph::renderBlock
    // tell "no signal yet this block" (connected, clears to 0 as accumulator
    // base) apart from "no cable here at all" (unconnected, clears to that
    // type's audioInDefault() instead - see PortTables).
    synth::PortMask audioConnectedPortMask = 0;

    // Voice 0 of this same widget (self-pointer when this instance IS voice
    // 0) - set once in GraphBuilder::build(), stable for the Graph's whole
    // lifetime (the instances array is never reallocated after build). Lets
    // a node type opt into a "Global" mode (Lfo/Gain - see LfoState::global/
    // GainState::global) where every other voice defers to voice 0's result
    // instead of computing its own, without touching the voiced/global BFS
    // classification (classifyVoiced/isGlobalOnly in PortTables.cpp) - the
    // widget stays instanced/wired exactly as before, only the per-block DSP
    // result some voices use changes. nullptr only for the placeholder
    // instance GraphBuilder allocates when a build fails before this gets set.
    NodeInstance *primary = nullptr;

    // Latest scalar delivered by a non-audio-rate ControlRoute (MidiIn's
    // frequency/gate/velocity, or a Knob/Button) targeting audio-input port
    // p (bit p of controlConnectedPortMask), keyed by ControlRoute::targetPort.
    // Graph::renderBlock fills inPorts[p] from here instead of that type's
    // audioInDefault() when the port has a ControlRoute but no audio-rate
    // edge - lets e.g. a Knob or MIDI note feed a Multiplier/Adder/Divide
    // input directly (those only ever read inPorts, unlike Oscillator's
    // frequency/amplitude which already have their own dedicated fields -
    // see processNodeBlock - so this array is written but never consulted
    // for Oscillator targets).
    float controlValue[synth::kMaxAudioInPorts] = {};

    float midiPitchCv = 5.0f;
    float midiPitchTargetCv = 5.0f;
    uint32_t midiPitchSamplesRemaining = 0;
    int8_t midiPitchRampPort = -1;
    bool midiPitchInitialized = false;
    synth::PortMask controlRampPortMask = 0;

    OscillatorState osc;
    LfoState lfo;
    DetuneState detune;
    FrequencyDividerState frequencyDivider;
    GainState gain;
    MixerState mixer;
    // These twelve are heap-allocated (GraphBuilder::build) only for an
    // instance whose type actually needs them, and freed in Graph::destroy -
    // unlike every other state member above, which stays a plain value
    // (trivial lifetime, no manual alloc/free). They are by far the largest
    // of the ~25 per-type payloads (ReverbState alone: 26 embedded DelayLine
    // descriptors, ~550 B), and were previously always-present value members
    // here, meaning every single instance - an Oscillator, an LFO, anything
    // - paid their combined size even though at most one type ever applies.
    // That padding was the dominant cost pushing NodeInstance's per-instance
    // size up high enough to force high-voice-count presets out of internal
    // RAM entirely. nullptr for every instance whose type isn't the matching
    // one - dereference only after checking inst.type, exactly like the
    // original five (chorus/delay/reverb/reverbLight/espEffect).
    ChorusState *chorus = nullptr;
    DelayState *delay = nullptr;
    ReverbState *reverb = nullptr;
    ReverbLightState *reverbLight = nullptr;
    EspEffectState *espEffect = nullptr;
    AdsrState *adsr = nullptr;
    FilterState *filter = nullptr;
    EspEqState *espEq = nullptr;
    KnobState *knob = nullptr;
    ButtonState *button = nullptr;
    UpDownButtonState *upDownButton = nullptr;
    // ValueMapperState is defined (state + render/build/destroy/setParam
    // logic together) in nodes/ValueMapperNode.h, not NodeState.h - pointer
    // member only needs the forward declaration below.
    ValueMapperState *valueMapper = nullptr;
    // HammondState is defined (state + render/build/destroy/setParam logic
    // together) in nodes/HammondNode.h, not NodeState.h.
    HammondState *hammond = nullptr;
    AudioOutState audioOut;
    MeterState meter;
    MidiInState midiIn;
    MidiSplitState midiSplit;
    ConstState constant;
    SwitchState switchState;
    WavePlayerState wavePlayer;
    SoundFontPlayerState soundFontPlayer;
    ClockState clock;
    // multiplier/adder/divide/abs/link types carry no extra state.

    // Per-audio-input-port accumulator buffers. Only ports with sample-rate
    // edges (plus raw-pointer ESP effects) allocate a block; scalar defaults
    // and ControlRoutes stay in controlValue.
    float *inPorts[synth::kMaxAudioInPorts] = {};

    // Rendered output for this block. Most types only use outL (their one
    // output port); chorus/delay-stereo/echo-stereo/esp-eq use both.
    float *outL = nullptr;
    float *outR = nullptr;

    // Voice-skipping (see Graph::renderBlock): true while this instance was
    // actually processed on the last block. Starts true so the very first
    // idle block still runs the one-time zero-out below instead of trusting
    // the buffers' initial state.
    bool wasRenderedLastBlock = true;
};

// Small render-time helpers shared by every node type's render() - kept
// here (not duplicated per node header) since every nodes/*.h needs
// NodeInstance fully defined anyway.
namespace noderender
{
inline float portSample(const NodeInstance &inst, int portIdx, size_t i)
{
    return inst.inPorts[portIdx] != nullptr ? inst.inPorts[portIdx][i] : inst.controlValue[portIdx];
}

inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

inline bool controlPortConnected(const NodeInstance &inst, int portIdx)
{
    return (inst.controlConnectedPortMask & synth::portBit(portIdx)) != 0;
}
} // namespace noderender

// processNodeBlock() (the switch-dispatch DSP render for one block) lives in
// NodeRender.h, not here - it needs every nodes/*.h's full definition, and
// every nodes/*.h needs NodeInstance fully defined, so it can't live in this
// header without a genuine include cycle. See NodeRender.h.
