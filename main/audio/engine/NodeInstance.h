#pragma once

#include <cstddef>
#include <cstdint>
#include <new>

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

namespace nodes::dxenvelope
{
struct DxEnvelopeState;
}
using nodes::dxenvelope::DxEnvelopeState;

namespace nodes::steparp
{
struct StepArpState;
}
using nodes::steparp::StepArpState;

namespace nodes::filter
{
struct FilterState;
}
using nodes::filter::FilterState;

namespace nodes::curtisfilter
{
struct CurtisFilterState;
}
using nodes::curtisfilter::CurtisFilterState;

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
// voice count, a global widget has exactly 1). Small self-contained states
// share one inline union; larger or separately-defined states share one
// typed-pointer union and are allocated only for their matching NodeType.
// This prevents every FM/DX voice from paying for every unrelated node
// state while preserving internal-RAM placement of render-hot data.
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

    // Which audio-input ports are a borrowed VIEW into another instance's
    // output buffer rather than an owned accumulator (bit i = inPorts[i]) -
    // set once in GraphBuilder::build()'s post-pass (plan section 3, P2:
    // "Bei nur einer unskalierten Quelle und einem ausschließlich lesenden
    // Ziel auf den Quellpuffer verweisen"), only for a port fed by exactly
    // one unscaled, non-monitor-tap edge whose source is guaranteed (by the
    // build's topological sort) to render before this instance every block.
    // Graph::renderBlock must skip both the per-block accumulator clear and
    // the edge-summing add/scale for a bit set here (the pointer already IS
    // the source's fresh data, nothing to accumulate into); Graph::destroy()
    // must skip freeing it (it doesn't own this memory - the source
    // instance's own outL/outR/link bus allocation does). Always a subset of
    // audioConnectedPortMask; never set for a port with more than one
    // source, a scaled edge, a monitor tap, or an edge whose source/
    // destination widget isn't part of the safely-topologically-sorted
    // (non-cyclic) prefix of renderOrder.
    synth::PortMask viewPortMask = 0;

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

    // Third per-port signal state (see Constant/Buffer in
    // audioPortConstantForBlock's comment below) - a linear ramp
    // start+i*inc for this whole block, published by a producer whose own
    // output happens to be exactly that shape (currently AdsrNode and
    // DxEnvelopeNode during active linear segments) and consumed only by node types
    // that explicitly check it (see noderender::rampForPort below). NOT
    // the same thing as controlRampPortMask above (that one is a transient
    // "don't treat as constant" safety flag for the MIDI-glide fill,
    // carries no value/step itself, and is unrelated to this mechanism -
    // do not conflate the two).
    synth::PortMask rampPortMask = 0;
    struct RampDescriptor
    {
        uint8_t port = 0xFF;
        float start = 0.0f;
        float inc = 0.0f;
    };
    // Small fixed cap, not kMaxAudioInPorts(17) wide - mirrors
    // RampScheduler's own segs_[2] precedent: a ramp is advertised on at
    // most a couple of ports of a handful of instances per block in
    // practice. Overflow (a 3rd simultaneously-ramped port on the same
    // instance) simply isn't tagged - falls back to the always-correct
    // existing scan/loop path, never a correctness risk, just a missed
    // fast path in a rare case.
    static constexpr uint8_t kMaxRampedPorts = 2;
    RampDescriptor rampDesc[kMaxRampedPorts] = {};

    // Input side of outIsConstant/outConstantValue above: which audio-input
    // ports had their whole accumulated block tagged constant this block by
    // Graph::renderBlock's edge-summing pass (see its view-port tag-copy
    // site - same eligibility as rampPortMask above: exactly one unscaled,
    // non-monitor-tap edge from an already-tagged-constant producer). Same
    // small fixed cap and same "falls back to the always-correct
    // audioPortConstantForBlock scan when unset or overflowed, never a
    // correctness risk" contract as rampPortMask/rampDesc - see
    // noderender::audioPortConstantForBlock below.
    synth::PortMask constantPortMask = 0;
    struct ConstDescriptor
    {
        uint8_t port = 0xFF;
        float value = 0.0f;
    };
    static constexpr uint8_t kMaxConstantPorts = 2;
    ConstDescriptor constDesc[kMaxConstantPorts] = {};
    // Output side: does THIS instance's own outL currently also happen to
    // be a whole-block linear ramp (set only by AdsrNode.h's render() for
    // now). outL itself is ALWAYS still fully materialized regardless -
    // this is purely additional metadata a Ramp-aware consumer may use
    // instead of re-deriving the same thing via a per-sample scan.
    bool outIsRamp = false;
    struct
    {
        float start = 0.0f;
        float inc = 0.0f;
    } outRamp;

    // Same idea as outIsRamp/outRamp above, for the other common per-block
    // output shape: one repeated value for the whole block (fm-plan.md
    // section 3, P3 "Graph-weite Konstanz-Propagierung"). Set by a producer
    // that already knows this for free - AdsrNode.h's isHolding() fast path
    // (sustain plateau or fully idle), Oscillator's/FmOscillatorNode's
    // memset-0 silent-skip fast paths - never inferred by scanning outL
    // here. outL itself is ALWAYS still fully materialized regardless -
    // this is purely additional metadata a constant-aware consumer may use
    // instead of re-deriving the same fact via audioPortConstantForBlock's
    // per-sample scan (see that function below).
    bool outIsConstant = false;
    float outConstantValue = 0.0f;

    // Exactly one NodeType can use exactly one of these small inline states.
    // Keeping them as separate members made every instance pay for every
    // unrelated node type. The anonymous union preserves existing access
    // syntax while reducing this storage to the largest member. build()
    // activates the matching member after assigning type.
    union
    {
        uint8_t emptyInlineState;
        OscillatorState osc;
        FmOscillatorState fmOsc;
        LfoState lfo;
        DetuneState detune;
        FrequencyDividerState frequencyDivider;
        GainState gain;
        BitcrusherState bitcrusher;
        WaveFolderState waveFolder;
        MixerState mixer;
        MultiplexerState multiplexer;
        AudioOutState audioOut;
        MeterState meter;
        MidiInState midiIn;
        MidiSplitState midiSplit;
        ConstState constant;
        SwitchState switchState;
        WavePlayerState wavePlayer;
        SoundFontPlayerState soundFontPlayer;
        ClockState clock;
        SampleHoldState sampleHold;
    };
    // Type-specific states below are heap-allocated (GraphBuilder::build)
    // only for an instance whose type actually needs them, and freed in
    // Graph::destroy -
    // unlike every other state member above, which stays a plain value
    // (trivial lifetime, no manual alloc/free). They are by far the largest
    // of the ~25 per-type payloads (ReverbState alone: 26 embedded DelayLine
    // descriptors, ~550 B), and were previously always-present value members
    // here, meaning every single instance - an Oscillator, an LFO, anything
    // - paid their combined size even though at most one type ever applies.
    // That padding was the dominant cost pushing NodeInstance's per-instance
    // size up high enough to force high-voice-count presets out of internal
    // RAM entirely. nullptr for every instance whose type isn't the matching
    // one - dereference only after checking inst.type.
    // Exactly one of these pointers can be meaningful because one instance
    // has exactly one NodeType. Sharing the storage removes 16 permanently
    // null pointers from every instance. typeState supplies the single
    // default initializer; assigning the matching typed pointer during
    // GraphBuilder::build starts that trivial union member's lifetime.
    union
    {
        void *typeState = nullptr;
        ChorusState *chorus;
        DelayState *delay;
        ReverbState *reverb;
        ReverbLightState *reverbLight;
        EspEffectState *espEffect;
        AdsrState *adsr;
        DxEnvelopeState *dxEnvelope;
        StepArpState *stepArp;
        FilterState *filter;
        CurtisFilterState *curtisFilter;
        EspEqState *espEq;
        KnobState *knob;
        ButtonState *button;
        UpDownButtonState *upDownButton;
        // Only audio page-link widgets need the six extra bus buffers.
        LinkState *link;
        ValueMapperState *valueMapper;
        HammondState *hammond;
    };
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

    // Build-time proof that every consumer of this instance's output can
    // consume outIsConstant/outIsRamp metadata without reading outL. Used
    // by DX Envelope to advance its scheduler while avoiding redundant
    // sample-buffer writes. False is the safe fallback for every topology.
    bool outputMetadataOnly = false;

    NodeInstance() : emptyInlineState(0) {}

    void constructInlineState()
    {
        switch (type)
        {
        case NodeType::Oscillator:
            new (&osc) OscillatorState();
            break;
        case NodeType::FmOscillator:
            new (&fmOsc) FmOscillatorState();
            break;
        case NodeType::Lfo:
            new (&lfo) LfoState();
            break;
        case NodeType::Detune:
            new (&detune) DetuneState();
            break;
        case NodeType::FrequencyDivider:
            new (&frequencyDivider) FrequencyDividerState();
            break;
        case NodeType::Gain:
            new (&gain) GainState();
            break;
        case NodeType::Bitcrusher:
            new (&bitcrusher) BitcrusherState();
            break;
        case NodeType::WaveFolder:
            new (&waveFolder) WaveFolderState();
            break;
        case NodeType::Mixer:
            new (&mixer) MixerState();
            break;
        case NodeType::Multiplexer:
            new (&multiplexer) MultiplexerState();
            break;
        case NodeType::AudioOut:
        case NodeType::UsbOut:
        case NodeType::Es8311Out:
        case NodeType::Wm8960Out:
            new (&audioOut) AudioOutState();
            break;
        case NodeType::VuMeter:
        case NodeType::Oscilloscope:
        case NodeType::Spectrum:
            new (&meter) MeterState();
            break;
        case NodeType::MidiIn:
            new (&midiIn) MidiInState();
            break;
        case NodeType::MidiSplit:
            new (&midiSplit) MidiSplitState();
            break;
        case NodeType::Const:
            new (&constant) ConstState();
            break;
        case NodeType::Switch:
            new (&switchState) SwitchState();
            break;
        case NodeType::WavePlayer:
            new (&wavePlayer) WavePlayerState();
            break;
        case NodeType::SoundFontPlayer:
            new (&soundFontPlayer) SoundFontPlayerState();
            break;
        case NodeType::Clock:
            new (&clock) ClockState();
            break;
        case NodeType::SampleHold:
            new (&sampleHold) SampleHoldState();
            break;
        default:
            emptyInlineState = 0;
            break;
        }
    }
};

inline float *outputPortBuffer(NodeInstance &inst, uint8_t port)
{
    if (port == 0)
        return inst.outL;
    if (port == 1)
        return inst.outR;
    const uint8_t extra = port - 2;
    return inst.link != nullptr && extra < synth::kLinkBusCount - 2 ? inst.link->busOut[extra] : nullptr;
}

inline const float *outputPortBuffer(const NodeInstance &inst, uint8_t port)
{
    if (port == 0)
        return inst.outL;
    if (port == 1)
        return inst.outR;
    const uint8_t extra = port - 2;
    return inst.link != nullptr && extra < synth::kLinkBusCount - 2 ? inst.link->busOut[extra] : nullptr;
}

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

// True if `port` resolves to one repeated value for this whole block,
// regardless of why: no buffer at all (unconnected, or ControlRoute-filled
// - see Graph::renderBlock - trivially constant, O(1), no scan needed), a
// genuine DSP edge that happens to carry an unchanging value (e.g. a held
// note through a Frequency Divider), or - just as correctly, since this
// scans the actual buffer content rather than trusting a connectivity flag
// - a currently active MIDI-glide ramp (Graph::renderBlock's
// midiPitchRampPort fill is a real, genuinely varying per-sample sequence,
// not distinguished by audioConnectedPortMask at all - see documentation/
// native-synth-optimization-plan-2026-09-09.md section 6, finding 1).
// `value` receives the constant value whenever this returns true. Shared
// by every node type that wants a block-constant fast path (originally
// Oscillator's pitch/tune - see OscillatorNode.h - now general since P2
// asks several more node types to do the same, see allPortsConstantForBlock
// below).
inline bool audioPortConstantForBlock(const NodeInstance &inst, int port, size_t frames, float &value)
{
    // A metadata-only producer may deliberately leave its shared output
    // buffer untouched while advertising the current block as a ramp. The
    // bytes behind inPorts[port] then belong to an older block and must not
    // be scanned as if they described the current one. Ramp-aware consumers
    // call rampForPort() after this returns false; other consumers retain
    // their conservative fallback behavior.
    if ((inst.rampPortMask & synth::portBit(port)) != 0)
    {
        return false;
    }

    const float *samples = inst.inPorts[port];
    if (samples == nullptr || frames == 0)
    {
        value = inst.controlValue[port];
        return true;
    }

    // Fast path (fm-plan.md section 3, P3): a producer already told us, via
    // Graph::renderBlock's edge tag-copy, that this port's whole
    // accumulated buffer is one repeated value this block - skip the
    // O(frames) rescan below entirely instead of re-discovering the same
    // fact from scratch every block (the exact "vermeidbare FM-lokale
    // Scans" the plan calls out). Only ever set when this port had exactly
    // one eligible edge (see NodeInstance.h's constantPortMask comment), so
    // this can never be stale from a second, non-constant edge summed in
    // afterward - falls through to the ordinary scan whenever unset.
    if ((inst.constantPortMask & synth::portBit(port)) != 0)
    {
        for (uint8_t k = 0; k < NodeInstance::kMaxConstantPorts; ++k)
        {
            if (inst.constDesc[k].port == static_cast<uint8_t>(port))
            {
                value = inst.constDesc[k].value;
                return true;
            }
        }
    }

    value = samples[0];
    for (size_t i = 1; i < frames; ++i)
    {
        if (samples[i] != value)
            return false;
    }
    return true;
}

// Convenience for a node whose every audio input port (0..portCount-1)
// must be block-constant before it can skip its per-sample loop (plan
// section 3, P2: "Frequency Divider, Detune, Adder, Multiplier, Divide und
// Abs propagieren diese Information"). Fills values[0..portCount) as it
// goes (only the entries up to the first non-constant port are meaningful
// when this returns false) and stops scanning as soon as one port turns
// out non-constant, in the SAME port order the caller's own per-sample
// loop already used - so falling back to values[p] for every p is bit-
// identical to what that loop would have computed for any one sample once
// every port genuinely is constant (no summation/order change, just fewer
// repeats of the same computation).
inline bool allPortsConstantForBlock(const NodeInstance &inst, int portCount, size_t frames, float *values)
{
    for (int p = 0; p < portCount; ++p)
    {
        if (!audioPortConstantForBlock(inst, p, frames, values[p]))
        {
            return false;
        }
    }
    return true;
}

// Returns the tagged Ramp descriptor for `port` this block, or nullptr if
// that port isn't a tagged ramp right now (either genuinely not one, or
// the NodeInstance::kMaxRampedPorts cap was hit - see that constant's
// comment). A Ramp-aware node checks this ONLY after audioPortConstantForBlock
// already returned false for the same port (a tagged ramp is never also
// block-constant, see Graph.h's tag-copy site) - never a substitute for
// that existing check, an additional third branch alongside it.
inline const NodeInstance::RampDescriptor *rampForPort(const NodeInstance &inst, int port)
{
    if ((inst.rampPortMask & synth::portBit(port)) == 0)
    {
        return nullptr;
    }
    for (uint8_t k = 0; k < NodeInstance::kMaxRampedPorts; ++k)
    {
        if (inst.rampDesc[k].port == static_cast<uint8_t>(port))
        {
            return &inst.rampDesc[k];
        }
    }
    return nullptr;
}
} // namespace noderender

// processNodeBlock() (the switch-dispatch DSP render for one block) lives in
// NodeRender.h, not here - it needs every nodes/*.h's full definition, and
// every nodes/*.h needs NodeInstance fully defined, so it can't live in this
// header without a genuine include cycle. See NodeRender.h.
