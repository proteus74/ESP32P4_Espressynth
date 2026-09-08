#pragma once

#include <cstring>

#include "NodeTypes.h"

// C++ port of static/js/main.js's AUDIO_PORTS/getPortKind table - which
// ports on which node type are audio-rate and connectable. Only audio-rate
// connections from a preset become real DSP graph edges (see GraphBuilder);
// everything else (gate/waveType/filterType/... control params) is set via
// widget params or MIDI-in ControlRoutes, never a connectable port here.
//
// Kept as one central table rather than split per node type: every function
// here is inherently a cross-type classification (audioInIndex/
// audioOutIndex/audioInPortCount switch over every NodeType; isControlOnlyInput/
// isGlobalOnly/hasNoOutputs/isLinkType are sparse-exception predicates whose
// whole point is a single place to see which types are exceptions) - scattering
// that across 44 files would fragment a table meant to be auditable at a
// glance, not clarify it.
namespace ports
{

namespace detail
{
// Matches "in1".."in8" (1-based in the name, 0-based return), else -1.
inline int indexedInPort(const char *portName)
{
    if (portName == nullptr || strncmp(portName, "in", 2) != 0)
    {
        return -1;
    }
    const char *numPart = portName + 2;
    if (numPart[0] < '1' || numPart[0] > '9' || numPart[1] != '\0')
    {
        return -1;
    }
    const int n = numPart[0] - '0';
    return (n >= 1 && n <= 8) ? (n - 1) : -1;
}

inline bool is(const char *a, const char *b)
{
    return a != nullptr && strcmp(a, b) == 0;
}
} // namespace detail

// Index (0..kMaxAudioInPorts-1) of `portName` as an audio-input port on
// `type`, or -1 if `portName` isn't a known audio-rate input for that type.
inline int audioInIndex(NodeType type, const char *portName)
{
    using namespace detail;
    switch (type)
    {
    case NodeType::Oscillator:
        if (is(portName, "amplitude"))
            return 0;
        if (is(portName, "pitchCv"))
            return 1;
        if (is(portName, "tune"))
            return 2;
        // "waveform" is a real, but control-only audio-rate port (see
        // isControlOnlyInput) - 0-10V CV, 2V bands in WaveType order (0-2V
        // Sine, 2-4V Square, 4-6V Triangle, 6-8V Sawtooth, 8-10V Noise, see
        // NodeInstance.cpp's waveTypeFromCv()). Not wired: falls back to the
        // waveform dropdown value (OscillatorState::wave), as before.
        if (is(portName, "waveform"))
            return 3;
        // "gain" is a real audio-rate port (like Gain's "amount") - no
        // entry in isControlOnlyInput needed, see NodeInstance.cpp.
        if (is(portName, "gain"))
            return 4;
        return -1;
    case NodeType::Lfo:
        if (is(portName, "sync"))
            return 0;
        if (is(portName, "oneShot"))
            return 1;
        // "global" is a real, but control-only audio-rate port (see
        // isControlOnlyInput) - >=kGateThresholdVolts (or the "Global" UI
        // toggle, when not wired) makes voice 0 of this widget authoritative,
        // see LfoState::global/NodeInstance.cpp.
        if (is(portName, "global"))
            return 2;
        // "zeroPoint" is a real, but control-only audio-rate port (see
        // isControlOnlyInput) - 0-10V CV, replaces the manual "Fixed Zero
        // Point" slider outright (same pattern as Oscillator's "tune"), but
        // only takes effect when zeroPointFixed is active (see
        // NodeInstance.cpp/LfoState::zeroPointFixed).
        if (is(portName, "zeroPoint"))
            return 3;
        return -1;
    case NodeType::Detune:
        // cv only - amount isn't a real port (Knob-mappable by name via
        // setFloatFieldByName/paramName, same as Filter's cutoff/resonance).
        return is(portName, "cv") ? 0 : -1;
    case NodeType::Switch:
        if (is(portName, "in1"))
            return 0;
        if (is(portName, "in2"))
            return 1;
        return -1;
    case NodeType::Filter:
        // "cutoff"/"resonance" are real, but control-only audio-rate ports
        // (see isControlOnlyInput) - any source may feed them via a DSP edge;
        // NodeInstance.cpp's Filter case evaluates them sample-accurately
        // under a genuine DSP edge (real frequency modulation possible).
        if (is(portName, "in"))
            return 0;
        if (is(portName, "cutoff"))
            return 1;
        if (is(portName, "resonance"))
            return 2;
        // "global" - same pattern as Gain's "global" above (control-only,
        // see isControlOnlyInput), see FilterState::global.
        if (is(portName, "global"))
            return 3;
        return -1;
    case NodeType::Gain:
        // "amount" (CV, 0-10V) is a real audio-rate port, not a ControlRoute
        // - any source can feed it sample-accurately (Gain is deliberately
        // NOT listed in ports::isControlOnlyInput, unlike e.g. Detune's
        // "cv"). When no DSP edge is present, NodeInstance.cpp's Gain case
        // falls back to the manual/knob-driven amountCv scalar (see
        // GainState).
        if (is(portName, "in"))
            return 0;
        if (is(portName, "amount"))
            return 1;
        // "global" - same pattern as Lfo's "global" above (control-only,
        // see isControlOnlyInput), see GainState::global.
        if (is(portName, "global"))
            return 2;
        return -1;
    case NodeType::EspEq:
    case NodeType::EspAlc:
    case NodeType::EspChannelConvert:
    case NodeType::EspDrc:
    case NodeType::EspMbc:
    case NodeType::EspSonic:
        if (is(portName, "inL"))
            return 0;
        if (is(portName, "inR"))
            return 1;
        return -1;
    case NodeType::EspMixer:
        if (is(portName, "in1L")) return 0;
        if (is(portName, "in1R")) return 1;
        if (is(portName, "in2L")) return 2;
        if (is(portName, "in2R")) return 3;
        return -1;
    case NodeType::Mixer:
    case NodeType::Multiplier:
    case NodeType::Adder:
        return indexedInPort(portName);
    case NodeType::Divide:
    {
        // Fixed 2 inputs (in1/in2) - unlike Mixer/Multiplier/Adder's
        // configurable 2-8, so bound indexedInPort's 8-way range down to 2.
        const int idx = indexedInPort(portName);
        return (idx >= 0 && idx < 2) ? idx : -1;
    }
    case NodeType::FrequencyDivider:
        if (is(portName, "hz"))
            return 0;
        if (is(portName, "divider"))
            return 1;
        return -1;
    case NodeType::Abs:
    case NodeType::ValueMapper:
        return is(portName, "in") ? 0 : -1;
    case NodeType::AudioToValue:
        return is(portName, "audio") ? 0 : -1;
    case NodeType::Chorus:
        return is(portName, "in") ? 0 : -1;
    case NodeType::Delay:
        // "time"/"feedback"/"mix" are real, but control-only audio-rate
        // CV ports (see isControlOnlyInput) - same pattern as Filter's
        // cutoff/resonance.
        if (is(portName, "inL"))
            return 0;
        if (is(portName, "inR"))
            return 1;
        if (is(portName, "time"))
            return 2;
        if (is(portName, "feedback"))
            return 3;
        if (is(portName, "mix"))
            return 4;
        return -1;
    case NodeType::Reverb:
        // "roomSize"/"damping"/"wetLevel"/"dryLevel"/"preDelay" are real,
        // but control-only audio-rate CV ports (see isControlOnlyInput).
        if (is(portName, "inL"))
            return 0;
        if (is(portName, "inR"))
            return 1;
        if (is(portName, "bypass"))
            return 2;
        if (is(portName, "roomSize"))
            return 3;
        if (is(portName, "damping"))
            return 4;
        if (is(portName, "wetLevel"))
            return 5;
        if (is(portName, "dryLevel"))
            return 6;
        if (is(portName, "preDelay"))
            return 7;
        return -1;
    case NodeType::ReverbLight:
        // Like Reverb, but without "preDelay" (see ReverbLightState).
        if (is(portName, "inL"))
            return 0;
        if (is(portName, "inR"))
            return 1;
        if (is(portName, "bypass"))
            return 2;
        if (is(portName, "roomSize"))
            return 3;
        if (is(portName, "damping"))
            return 4;
        if (is(portName, "wetLevel"))
            return 5;
        if (is(portName, "dryLevel"))
            return 6;
        return -1;
    case NodeType::AudioOutputLink:
        if (is(portName, "inL"))
            return 0;
        if (is(portName, "inR"))
            return 1;
        return -1;
    case NodeType::AudioInputLink:
        // No user-facing input port - the implicit output-link->input-link
        // bridge edges target port index 0/1 directly (see GraphBuilder).
        return -1;
    case NodeType::AudioOut:
    case NodeType::UsbOut:
    case NodeType::Es8311Out:
    case NodeType::Wm8960Out:
        if (is(portName, "left"))
            return 0;
        if (is(portName, "right"))
            return 1;
        if (is(portName, "gain"))
            return 2;
        if (is(portName, "muted"))
            return 3;
        return -1;
    case NodeType::VuMeter:
    case NodeType::Spectrum:
        return is(portName, "in") ? 0 : -1;
    case NodeType::Oscilloscope:
        if (is(portName, "in1"))
            return 0;
        if (is(portName, "in2"))
            return 1;
        return -1;
    case NodeType::WavePlayer:
        if (is(portName, "amplitude"))
            return 0;
        if (is(portName, "note"))
            return 1;
        if (is(portName, "gate"))
            return 2;
        return -1;
    case NodeType::SoundFontPlayer:
        if (is(portName, "bank")) return 0;
        if (is(portName, "program")) return 1;
        if (is(portName, "enableCc")) return 2;
        if (is(portName, "enableModWheel")) return 3;
        if (is(portName, "enablePitchWheel")) return 4;
        // "gain" is a real audio-rate port (like Oscillator's "gain") - no
        // entry in isControlOnlyInput needed, see NodeInstance.cpp.
        if (is(portName, "gain")) return 5;
        return -1;
    case NodeType::Clock:
        // "tempo" is a real, but control-only audio-rate port (see
        // isControlOnlyInput) - same pattern as Filter's cutoff/resonance.
        return is(portName, "tempo") ? 0 : -1;
    case NodeType::Adsr:
        // "att"/"dec"/"sus"/"rel" are real, but control-only audio-rate
        // ports (see isControlOnlyInput) - "gate"/"velocity" remain pure
        // ControlRoutes (MidiGate/MidiVelocity), need no audioInIndex.
        if (is(portName, "att"))
            return 0;
        if (is(portName, "dec"))
            return 1;
        if (is(portName, "sus"))
            return 2;
        if (is(portName, "rel"))
            return 3;
        return -1;
    case NodeType::MidiIn:
    case NodeType::MidiSplit:
    case NodeType::Const:
    case NodeType::Knob:
    case NodeType::Button:
    case NodeType::UpDownButton:
    case NodeType::Unknown:
    default:
        return -1;
    }
}

// Index (0 = primary, 1 = secondary/right) of `portName` as an audio-output
// port on `type`, or -1 if not a known audio-rate output for that type.
inline int audioOutIndex(NodeType type, const char *portName)
{
    using namespace detail;
    switch (type)
    {
    case NodeType::Oscillator:
        return is(portName, "value") ? 0 : -1;
    case NodeType::Lfo:
        // "value" here - unlike Knob below - is ALWAYS a real audio port: an
        // LFO is by nature a continuous signal, not an occasionally changed
        // value.
        return is(portName, "value") ? 0 : -1;
    case NodeType::Detune:
        if (is(portName, "f1"))
            return 0;
        if (is(portName, "f2"))
            return 1;
        return -1;
    case NodeType::Knob:
        // "value" is deliberately NEVER a real audio port: a Knob typically
        // changes a few times per second, not 48000x - it is delivered
        // exclusively via ControlRoute (see GraphBuilder.cpp/
        // Graph::pushControlRouteValue), no additional edge-summing pass per
        // block needed.
        return -1;
    case NodeType::Const:
        return is(portName, "audio") ? 0 : -1;
    case NodeType::Button:
    case NodeType::UpDownButton:
        // Like Knob: current value is delivered exclusively via ControlRoute
        // and is already available there at graph build time.
        return -1;
    case NodeType::Switch:
        return is(portName, "value") ? 0 : -1;
    case NodeType::Adsr:
        return is(portName, "out") ? 0 : -1;
    case NodeType::Filter:
    case NodeType::Gain:
        return is(portName, "out") ? 0 : -1;
    case NodeType::EspEq:
    case NodeType::EspAlc:
    case NodeType::EspChannelConvert:
    case NodeType::EspMixer:
    case NodeType::EspSonic:
    case NodeType::EspDrc:
    case NodeType::EspMbc:
        if (is(portName, "outL"))
            return 0;
        if (is(portName, "outR"))
            return 1;
        return -1;
    case NodeType::Mixer:
        return is(portName, "out") ? 0 : -1;
    case NodeType::Multiplier:
    case NodeType::Adder:
    case NodeType::Divide:
        return is(portName, "value") ? 0 : -1;
    case NodeType::FrequencyDivider:
        return is(portName, "hz") ? 0 : -1;
    case NodeType::Abs:
        return is(portName, "value") ? 0 : -1;
    case NodeType::ValueMapper:
        return is(portName, "out") ? 0 : -1;
    case NodeType::AudioToValue:
        return is(portName, "value") ? 0 : -1;
    case NodeType::Chorus:
        if (is(portName, "left"))
            return 0;
        if (is(portName, "right"))
            return 1;
        return -1;
    case NodeType::Delay:
    case NodeType::Reverb:
    case NodeType::ReverbLight:
        if (is(portName, "outL"))
            return 0;
        if (is(portName, "outR"))
            return 1;
        return -1;
    case NodeType::AudioOutputLink:
        // No user-facing output port - internal bridge only (source index 0/1).
        return -1;
    case NodeType::AudioInputLink:
        if (is(portName, "outL"))
            return 0;
        if (is(portName, "outR"))
            return 1;
        return -1;
    case NodeType::WavePlayer:
        if (is(portName, "left"))
            return 0;
        if (is(portName, "right"))
            return 1;
        return -1;
    case NodeType::SoundFontPlayer:
        if (is(portName, "left")) return 0;
        if (is(portName, "right")) return 1;
        return -1;
    case NodeType::Clock:
        return is(portName, "pulse") ? 0 : -1;
    case NodeType::AudioOut:
    case NodeType::VuMeter:
    case NodeType::Oscilloscope:
    case NodeType::Spectrum:
    case NodeType::MidiIn:
    case NodeType::MidiSplit:
    case NodeType::Unknown:
    default:
        return -1;
    }
}

// Number of audio-input ports `type` has (0..kMaxAudioInPorts).
inline int audioInPortCount(NodeType type)
{
    switch (type)
    {
    case NodeType::Oscillator:
        return 5; // "amplitude" (CV) + "pitchCv" + "tune" + "waveform" (CV) + "gain" (CV)
    case NodeType::Lfo:
        return 4; // "sync" + "oneShot" + "global" + "zeroPoint" (CV)
    case NodeType::Detune:
        return 1;
    case NodeType::Switch:
        return 2;
    case NodeType::Filter:
        return 4; // "in" + "cutoff" + "resonance" + "global" (CV)
    case NodeType::Gain:
        return 3; // "in" + "amount" (CV) + "global" (CV)
    case NodeType::EspEq:
    case NodeType::EspAlc:
    case NodeType::EspChannelConvert:
    case NodeType::EspSonic:
    case NodeType::EspDrc:
    case NodeType::EspMbc:
        return 2;
    case NodeType::EspMixer:
        return 4;
    case NodeType::Mixer:
    case NodeType::Multiplier:
    case NodeType::Adder:
        return 8;
    case NodeType::Divide:
        return 2;
    case NodeType::FrequencyDivider:
        return 2;
    case NodeType::Abs:
    case NodeType::ValueMapper:
        return 1;
    case NodeType::AudioToValue:
        return 1;
    case NodeType::Chorus:
        return 1;
    case NodeType::Delay:
        return 5; // "inL"+"inR"+"time"+"feedback"+"mix" (CV)
    case NodeType::Reverb:
        return 8; // "inL"+"inR"+"bypass"+"roomSize"+"damping"+"wetLevel"+"dryLevel"+"preDelay" (CV)
    case NodeType::ReverbLight:
        return 7; // like Reverb, without "preDelay"
    case NodeType::AudioOutputLink:
        return 2;
    case NodeType::AudioInputLink:
        // Not user-connectable from a preset (audioInIndex() returns -1 for
        // it), but still needs two real port buffers: GraphBuilder wires the
        // implicit output-link->input-link bridge straight to port index 0/1.
        return 2;
    case NodeType::AudioOut:
    case NodeType::UsbOut:
    case NodeType::Es8311Out:
    case NodeType::Wm8960Out:
        return 4;
    case NodeType::VuMeter:
    case NodeType::Spectrum:
        return 1;
    case NodeType::Oscilloscope:
        return 2;
    case NodeType::WavePlayer:
        return 3;
    case NodeType::SoundFontPlayer:
        return 6; // "bank"+"program"+"enableCc"+"enableModWheel"+"enablePitchWheel"+"gain" (CV)
    case NodeType::Clock:
        return 1; // "tempo" (CV)
    case NodeType::Adsr:
        return 4; // "att"+"dec"+"sus"+"rel" (CV)
    case NodeType::MidiIn:
    case NodeType::MidiSplit:
    case NodeType::Const:
    case NodeType::Knob:
    case NodeType::Button:
    case NodeType::UpDownButton:
    case NodeType::Unknown:
    default:
        return 0;
    }
}

// Value an audio-input port renders as while structurally unconnected (no
// preset edge targets it) - 0.0f for most types (Adder/Mixer/...), but the
// neutral element of the operation for Multiplier (1.0f, any port) and
// Divide's divisor in2 (1.0f, to avoid dividing by an unconnected 0).
inline float audioInDefault(NodeType type, int portIdx)
{
    switch (type)
    {
    case NodeType::Multiplier:
        // Unconnected factor must not silence the whole product.
        return 1.0f;
    case NodeType::Divide:
        // in1 (Dividend) defaults to 0, in2 (Divisor) defaults to 1 - dividing
        // by an unconnected port must not divide by 0.
        return portIdx == 1 ? 1.0f : 0.0f;
    case NodeType::FrequencyDivider:
        return 0.0f;
    case NodeType::SoundFontPlayer:
        return portIdx == 0 ? 1.0f : 0.0f;
    default:
        return 0.0f;
    }
}

// BFS boundary predicates for the voiced/global classification (see
// GraphBuilder): link widgets and any widget with zero outputs.
inline bool isLinkType(NodeType type)
{
    return type == NodeType::AudioOutputLink || type == NodeType::AudioInputLink;
}

inline bool hasNoOutputs(NodeType type)
{
    return type == NodeType::AudioOut || type == NodeType::UsbOut || type == NodeType::Es8311Out ||
           type == NodeType::Wm8960Out || type == NodeType::VuMeter || type == NodeType::Oscilloscope ||
           type == NodeType::Spectrum || type == NodeType::MidiIn;
    // Note: Button/UpDownButton/Switch are deliberately NOT listed here even
    // though they may have no *audio* output port - hasNoOutputs() gates the
    // voiced/global BFS sink check, and these types still produce a value
    // (via ControlRoute or, for Switch, a real audio out port).
}

// Heavy post-mix effects that must never be multiplied per synth voice.
inline bool isGlobalOnly(NodeType type)
{
    return type == NodeType::EspEq || type == NodeType::EspAlc || type == NodeType::EspChannelConvert ||
           type == NodeType::EspMixer || type == NodeType::EspSonic || type == NodeType::EspDrc ||
           type == NodeType::EspMbc || type == NodeType::SoundFontPlayer || type == NodeType::Delay ||
           type == NodeType::Reverb || type == NodeType::ReverbLight;
}

// True if input port `portIdx` on `type` must never be fed by a genuinely
// variable audio-rate signal (Oscillator/Filter/LFO/...), even though it has
// a real audioInIndex of its own (needed so a ControlRoute-delivered value -
// e.g. a Knob, or a Multiplier/Adder/Divide result - can still reach it
// efficiently as a per-sample-constant edge). Multiplier/Adder/Divide's
// inputs (any index), Oscillator's "pitchCv"/"tune" (indices 1/2, NOT
// "amplitude"/index 0 which stays genuinely audio-rate), Detune's "cv"
// (index 0), and Lfo's "sync" (index 0) are control-only this way. Enforced
// in GraphBuilder's connection resolution together with a source-type
// allowlist (MidiIn/Lfo/Knob/Button/UpDownButton/Const/Multiplier/Adder/
// Divide/Detune) - anything else silently fails to wire, exactly like an
// unsupported connection.
inline bool isControlOnlyInput(NodeType type, int portIdx)
{
    switch (type)
    {
    case NodeType::Multiplier:
    case NodeType::Adder:
    case NodeType::Divide:
    case NodeType::FrequencyDivider:
    case NodeType::Abs:
    case NodeType::ValueMapper:
        return true; // all inputs
    case NodeType::AudioToValue:
        return false; // input stays real audio, output is control-rate
    case NodeType::Oscillator:
        return portIdx == 1 || portIdx == 2 || portIdx == 3; // "pitchCv"+"tune"+"waveform" - "amplitude" (index 0) stays real audio-rate
    case NodeType::Detune:
        return portIdx == 0; // "cv" - same reasoning as Oscillator's pitchCv
    case NodeType::Gain:
        return portIdx == 2; // "global" - "amount" (index 1) stays real audio-rate
    case NodeType::Filter:
        return portIdx == 1 || portIdx == 2 || portIdx == 3; // "cutoff"+"resonance"+"global"
    case NodeType::Clock:
        return portIdx == 0; // "tempo"
    case NodeType::Adsr:
        return true; // "att"/"dec"/"sus"/"rel" - all four
    case NodeType::Lfo:
        return portIdx == 0 || portIdx == 1 || portIdx == 2 || portIdx == 3; // "sync"+"oneShot"+"global"+"zeroPoint" - trigger-/gate-/slider-like signals, control-rate suffices
    case NodeType::WavePlayer:
        return portIdx == 1 || portIdx == 2; // "note"+"gate" - "amplitude" (index 0) stays audio-rate
    case NodeType::SoundFontPlayer:
        return portIdx != 5; // event/scalar inputs, control-only - except "gain" (index 5), real audio-rate like Oscillator's
    case NodeType::Delay:
        return portIdx == 2 || portIdx == 3 || portIdx == 4; // "time"+"feedback"+"mix"
    case NodeType::Reverb:
        return portIdx >= 2; // "bypass"+"roomSize"+"damping"+"wetLevel"+"dryLevel"+"preDelay"
    case NodeType::ReverbLight:
        return portIdx >= 2; // "bypass"+"roomSize"+"damping"+"wetLevel"+"dryLevel"
    case NodeType::AudioOut:
    case NodeType::UsbOut:
    case NodeType::Es8311Out:
    case NodeType::Wm8960Out:
        return portIdx >= 2; // gain + muted are scalar CV inputs
    default:
        return false;
    }
}

} // namespace ports
