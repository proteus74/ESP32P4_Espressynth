#pragma once

#include <cstring>

#include "CvConventions.h"
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
// Matches "in1".."inN" (1-based in the name, 0-based return), else -1.
inline int indexedInPort(const char *portName, int maxInputs = 8)
{
    if (portName == nullptr || strncmp(portName, "in", 2) != 0)
    {
        return -1;
    }
    const char *numPart = portName + 2;
    if (numPart[0] < '1' || numPart[0] > '9')
    {
        return -1;
    }
    int n = numPart[0] - '0';
    if (numPart[1] != '\0')
    {
        if (numPart[1] < '0' || numPart[1] > '9' || numPart[2] != '\0')
            return -1;
        n = n * 10 + (numPart[1] - '0');
    }
    return (n >= 1 && n <= maxInputs) ? (n - 1) : -1;
}

// Matches "out1".."outN" (1-based in the name, 0-based return), else -1 -
// output-side counterpart of indexedInPort() above, used by OutputSwitch/
// OutputMultiplexer (nodes/OutputSwitchNode.h, nodes/OutputMultiplexerNode.h).
inline int indexedOutPort(const char *portName, int maxOutputs)
{
    if (portName == nullptr || strncmp(portName, "out", 3) != 0)
    {
        return -1;
    }
    const char *numPart = portName + 3;
    if (numPart[0] < '1' || numPart[0] > '9')
    {
        return -1;
    }
    int n = numPart[0] - '0';
    if (numPart[1] != '\0')
    {
        if (numPart[1] < '0' || numPart[1] > '9' || numPart[2] != '\0')
            return -1;
        n = n * 10 + (numPart[1] - '0');
    }
    return (n >= 1 && n <= maxOutputs) ? (n - 1) : -1;
}

// Matches "gain1".."gain8" and returns a 0-based channel index.
inline int indexedGainPort(const char *portName)
{
    if (portName == nullptr || strncmp(portName, "gain", 4) != 0)
    {
        return -1;
    }
    const char *number = portName + 4;
    return number[0] >= '1' && number[0] <= '8' && number[1] == '\0' ? number[0] - '1' : -1;
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
        // isControlOnlyInput) - 0-10V CV, nine equal bands in WaveType order
        // (Sine, Linear Up, Linear Down, Square, Triangle, Sawtooth, falling
        // Sawtooth, white Noise, Pink Noise; see nodes/OscillatorNode.h's
        // waveTypeFromCv()). Not wired: falls back
        // to the waveform dropdown value (OscillatorState::wave), as before.
        if (is(portName, "waveform"))
            return 3;
        // "gain" is a real audio-rate port (like Gain's "amount") - no
        // entry in isControlOnlyInput needed, see NodeInstance.cpp.
        if (is(portName, "gain"))
            return 4;
        if (is(portName, "feedback"))
            return 5;
        return -1;
    case NodeType::FmOscillator:
        // Oscillator's port set plus "fmIn"/"fmIndex" for phase-modulation FM
        // synthesis (see nodes/FmOscillatorNode.h). "fmIn" is a genuine
        // audio-rate port (NOT in isControlOnlyInput) - the whole point is a
        // per-sample phase offset from whatever is patched in (typically
        // another Oscillator's "value"). "fmIndex" is control-only, like
        // Oscillator's "tune".
        if (is(portName, "amplitude"))
            return 0;
        if (is(portName, "pitchCv"))
            return 1;
        if (is(portName, "tune"))
            return 2;
        if (is(portName, "waveform"))
            return 3;
        if (is(portName, "gain"))
            return 4;
        if (is(portName, "fmIn"))
            return 5;
        if (is(portName, "fmIndex"))
            return 6;
        if (is(portName, "feedback"))
            return 7;
        // "dutyCycle" - control-only CV, like "fmIndex"/"feedback" above.
        // Only affects WaveType::Square (see PhaseOsc::sampleAt's
        // dutyThresholdInt in DspPrimitives.h) - harmless no-op for every
        // other waveform.
        if (is(portName, "dutyCycle"))
            return 8;
        if (is(portName, "superSawDetune"))
            return 9;
        if (is(portName, "superSawMix"))
            return 10;
        return -1;
    case NodeType::Hammond:
        // Oscillator's port set minus "waveform" (see nodes/HammondNode.h -
        // the nine drawbar sines replace the waveform selector). The nine
        // drawbar registers deliberately DO have real audio-rate ports here
        // (unlike Mixer's "gain1".."gain8", which are plain scalars) - each
        // is independently CV-controllable, exactly like Oscillator's own
        // "gain". This is the node the 8->16 kMaxAudioInPorts widening
        // exists for (see SynthConfig.h).
        if (is(portName, "amplitude"))
            return 0;
        if (is(portName, "pitchCv"))
            return 1;
        if (is(portName, "tune"))
            return 2;
        if (is(portName, "gain"))
            return 3;
        if (strncmp(portName, "drawbar", 7) == 0)
        {
            const char *n = portName + 7;
            if (n[0] >= '1' && n[0] <= '9' && n[1] == '\0')
                return 4 + (n[0] - '1');
        }
        return -1;
    case NodeType::Lfo:
        if (is(portName, "reset"))
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
    case NodeType::Wavetable:
        // reset/oneShot/global: same indices and semantics as the LFO's
        // (nodes::lfo::globalUpdate drives both). "morph" - table position
        // 0-10V - is deliberately a genuine audio-rate port: a second LFO or
        // an envelope patched here sweeps the bank per sample.
        if (is(portName, "reset"))
            return 0;
        if (is(portName, "oneShot"))
            return 1;
        if (is(portName, "global"))
            return 2;
        if (is(portName, "morph"))
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
    case NodeType::OutputSwitch:
        // Mirror of Switch: one audio input routed to whichever of "out1"/
        // "out2" is selected - see nodes/OutputSwitchNode.h.
        return is(portName, "in") ? 0 : -1;
    case NodeType::Filter:
    case NodeType::CurtisFilter:
        // "cutoff"/"resonance" are real, but control-only audio-rate ports
        // (see isControlOnlyInput) - any source may feed them via a DSP edge;
        // NodeInstance.cpp's Filter case evaluates them sample-accurately
        // under a genuine DSP edge (real frequency modulation possible).
        if (is(portName, "in"))
            return 0;
        // "filterType"/"slope" are control-only 0-10V discrete selectors.
        if (is(portName, "filterType"))
            return 1;
        if (is(portName, "slope"))
            return 2;
        if (is(portName, "cutoff"))
            return 3;
        if (is(portName, "resonance"))
            return 4;
        // "global" - same pattern as Gain's "global" above (control-only,
        // see isControlOnlyInput), see FilterState::global.
        if (is(portName, "global"))
            return 5;
        return -1;
    case NodeType::Gain:
        // "amount" (Output Gain) and "curve" (independent 2nd VCA stage,
        // 0-10V, 5V = unity - see cv::curveGain) are both real audio-rate
        // ports, not ControlRoutes - any source can feed either
        // sample-accurately (neither is listed in ports::isControlOnlyInput,
        // unlike e.g. Detune's "cv"). When no DSP edge is present,
        // GainNode.h's render() falls back to the manual/knob-driven
        // amountCv/curveCv scalars (see GainState).
        if (is(portName, "in"))
            return 0;
        if (is(portName, "amount"))
            return 1;
        if (is(portName, "curve"))
            return 2;
        // "global" - same pattern as Lfo's "global" above (control-only,
        // see isControlOnlyInput), see GainState::global.
        if (is(portName, "global"))
            return 3;
        return -1;
    case NodeType::EspEq:
        if (is(portName, "inL"))
            return 0;
        if (is(portName, "inR"))
            return 1;
        return -1;
    case NodeType::GraphicEq:
        if (is(portName, "inL"))
            return 0;
        if (is(portName, "inR"))
            return 1;
        return -1;
    case NodeType::Mixer:
    case NodeType::Multiplier:
    case NodeType::Adder:
    case NodeType::LogicAnd:
    case NodeType::LogicOr:
        return indexedInPort(portName);
    case NodeType::Multiplexer:
        // "select" (control-only, see isControlOnlyInput) picks which of
        // "in1".."in16" reaches "value" - see nodes/MultiplexerNode.h.
        if (is(portName, "select"))
            return 16;
        return indexedInPort(portName, 16);
    case NodeType::OutputMultiplexer:
        // Mirror of Multiplexer: one audio input ("in", port 0) routed to
        // whichever of "out1".."out16" is selected via "select" (port 1,
        // control-only) - see nodes/OutputMultiplexerNode.h.
        if (is(portName, "in"))
            return 0;
        if (is(portName, "select"))
            return 1;
        return -1;
    case NodeType::Divide:
    case NodeType::LogicXor:
    {
        // Fixed 2 inputs (in1/in2) - unlike Mixer/Multiplier/Adder's
        // configurable 2-8, so bound indexedInPort's range down to 2.
        const int idx = indexedInPort(portName);
        return (idx >= 0 && idx < 2) ? idx : -1;
    }
    case NodeType::FrequencyDivider:
        if (is(portName, "cv") || is(portName, "hz"))
            return 0;
        if (is(portName, "divider"))
            return 1;
        return -1;
    case NodeType::Abs:
    case NodeType::ValueMapper:
    case NodeType::LogicNot:
        return is(portName, "in") ? 0 : -1;
    case NodeType::Bitcrusher:
        if (is(portName, "in")) return 0;
        if (is(portName, "bypass")) return 1;
        if (is(portName, "crush")) return 2;
        return -1;
    case NodeType::WaveFolder:
        if (is(portName, "in")) return 0;
        if (is(portName, "drive")) return 1;
        if (is(portName, "symmetry")) return 2;
        if (is(portName, "gain")) return 3;
        if (is(portName, "mix")) return 4;
        return -1;
    case NodeType::EnvelopeFollower:
        if (is(portName, "in")) return 0;
        if (is(portName, "attack")) return 1;
        if (is(portName, "release")) return 2;
        if (is(portName, "preGain")) return 3;
        return -1;
    case NodeType::BurstGenerator:
        if (is(portName, "in")) return 0;
        if (is(portName, "mode")) return 1;
        if (is(portName, "count")) return 2;
        if (is(portName, "time")) return 3;
        if (is(portName, "decay")) return 4;
        return -1;
    case NodeType::Pan:
        if (is(portName, "in")) return 0;
        if (is(portName, "pan")) return 1;
        return -1;
    case NodeType::SampleHold:
        if (is(portName, "in")) return 0;
        if (is(portName, "trigger")) return 1;
        if (is(portName, "threshold")) return 2;
        return -1;
    case NodeType::Chorus:
        return is(portName, "in") ? 0 : -1;
    case NodeType::CvOut:
    case NodeType::DigitalOut:
        return is(portName, "in") ? 0 : -1;
    case NodeType::Delay:
        // "time"/"feedback"/"mix" are real, but control-only audio-rate
        // CV ports (see isControlOnlyInput) - same pattern as Filter's
        // cutoff/resonance. "bypass" - same pattern as ReverbLight's "bypass".
        if (is(portName, "inL"))
            return 0;
        if (is(portName, "inR"))
            return 1;
        if (is(portName, "bypass"))
            return 2;
        if (is(portName, "time"))
            return 3;
        if (is(portName, "feedback"))
            return 4;
        if (is(portName, "mix"))
            return 5;
        return -1;
    case NodeType::ReverbLight:
        // "roomSize"/"damping"/"wetLevel"/"dryLevel" are real, but
        // control-only audio-rate CV ports (see isControlOnlyInput).
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
        if (is(portName, "inL")) // legacy bus 1
            return 0;
        if (is(portName, "inR")) // legacy bus 2
            return 1;
        return indexedInPort(portName, synth::kLinkBusCount);
    case NodeType::AudioInputLink:
    {
        // Buses 1-8 are internal page-link edges. gain1..gain8 are visible
        // 0-10V level-CV inputs on the Audio Outputs widget.
        const int gain = indexedGainPort(portName);
        return gain >= 0 ? static_cast<int>(synth::kLinkBusCount) + gain : -1;
    }
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
    case NodeType::LinkedInput:
        if (is(portName, "in")) return 0;
        return indexedInPort(portName, synth::kLinkBusCount);
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
        // "gain" - real audio-rate port (like Oscillator's/SoundFontPlayer's
        // "gain"), scales the finished sample multiplicatively.
        if (is(portName, "gain"))
            return 3;
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
    case NodeType::MdaPiano:
        if (is(portName, "enableModWheel")) return 0;
        if (is(portName, "enablePitchWheel")) return 1;
        if (is(portName, "voiceType")) return 2;
        if (is(portName, "drive")) return 3;
        if (is(portName, "tremWahMode")) return 4;
        if (is(portName, "tremWahDepth")) return 5;
        if (is(portName, "tremWahRate")) return 6;
        if (is(portName, "choPhaMode")) return 7;
        if (is(portName, "choPhaDepth")) return 8;
        if (is(portName, "choPhaSpeed")) return 9;
        if (is(portName, "preGain")) return 10;
        if (is(portName, "gain")) return 11;
        return -1;
    case NodeType::Adsr:
        // "att"/"dec"/"sus"/"rel" are real, but control-only audio-rate
        // ports (see isControlOnlyInput). "gate" (index 4) is ALSO a real,
        // control-only audio-rate port - a non-MIDI source (e.g. a CV
        // Step-Arpeggiator's "gateOut") must be able to retrigger this
        // envelope directly, same as WavePlayer's own "gate" port;
        // NodeInstance.cpp's Adsr case still honors the MIDI-derived
        // ControlRoute gate path unchanged whenever nothing is wired here
        // (see AdsrNode.h's gateFed check). "velocity" (index 5) is the
        // same pattern: wiring any real audio-rate source (e.g. a
        // Multiplexer, so a CV can pick between two preset velocities) now
        // overrides once per block; the MIDI-derived ControlRoute delivery
        // (Graph.h's MidiVelocity case) and the Knob/Button/Const paramName
        // path (AdsrNode.h::setParam) remain exactly as before whenever
        // this port isn't wired (see AdsrNode.h's velFed check) - without a
        // port index here, only that narrow set of source types could ever
        // reach velocity at all.
        if (is(portName, "att"))
            return 0;
        if (is(portName, "dec"))
            return 1;
        if (is(portName, "sus"))
            return 2;
        if (is(portName, "rel"))
            return 3;
        if (is(portName, "gate"))
            return 4;
        if (is(portName, "velocity"))
            return 5;
        return -1;
    case NodeType::DxEnvelope:
        // level1-4/time1-4 (0-7) are real, but control-only audio-rate
        // ports. "gate" (index 8) is likewise real/control-only - same
        // reasoning and same NodeInstance.cpp fallback pattern as Adsr's
        // "gate" above (see DxEnvelopeNode.h's gateFed check).
        if (is(portName, "level1")) return 0;
        if (is(portName, "level2")) return 1;
        if (is(portName, "level3")) return 2;
        if (is(portName, "level4")) return 3;
        if (is(portName, "time1")) return 4;
        if (is(portName, "time2")) return 5;
        if (is(portName, "time3")) return 6;
        if (is(portName, "time4")) return 7;
        if (is(portName, "gate")) return 8;
        return -1;
    case NodeType::StepArpeggiator:
        if (is(portName, "clock"))
            return 0;
        if (is(portName, "gate"))
            return 1;
        if (is(portName, "pitch"))
            return 2;
        if (is(portName, "direction"))
            return 3;
        // "pulseTime" (index 20, after the 16 step ports) - gateOut's pulse
        // width, 0-10V mapped via pulseTimeMinSec/pulseTimeMaxSec (see
        // StepArpState) to seconds, same cv::cvToRange pattern as Delay's
        // own "time" port (DelayNode.h) - only consulted when actually
        // wired, the local pulseMs scalar (already in real ms, see
        // setParam()) remains the fallback otherwise.
        if (is(portName, "pulseTime"))
            return 20;
        // "rate" (index 21) - the internal fallback clock's own tempo CV,
        // used only while "clock" (index 0) itself is unwired - same
        // wired-CV-or-local-slider split as "pulseTime" above (see
        // StepArpeggiatorNode.h's render()).
        if (is(portName, "rate"))
            return 21;
        if (strncmp(portName, "step", 4) == 0)
        {
            // "step1".."step16" (1-based in the name, 0-based return) - same
            // one-or-two-digit parsing as detail::indexedInPort's "inN", just
            // with the "step" prefix instead of "in".
            const char *numPart = portName + 4;
            if (numPart[0] < '1' || numPart[0] > '9')
                return -1;
            int n = numPart[0] - '0';
            if (numPart[1] != '\0')
            {
                if (numPart[1] < '0' || numPart[1] > '9' || numPart[2] != '\0')
                    return -1;
                n = n * 10 + (numPart[1] - '0');
            }
            return (n >= 1 && n <= 16) ? 3 + n : -1;
        }
        return -1;
    case NodeType::DrumSequencer:
        if (is(portName, "clock")) return 0;
        if (is(portName, "gate")) return 1;
        if (is(portName, "pattern")) return 2;
        if (is(portName, "rate")) return 3;
        if (is(portName, "switchAtEnd")) return 4;
        return -1;
    case NodeType::Knob:
    case NodeType::Button:
        return is(portName, "layer") ? 0 : -1;
    case NodeType::MidiIn:
    case NodeType::MidiSplit:
    case NodeType::Const:
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
    case NodeType::FmOscillator:
        return is(portName, "value") ? 0 : -1;
    case NodeType::Hammond:
        return is(portName, "value") ? 0 : -1; // mono, like Oscillator - only outL is ever written
    case NodeType::Lfo:
    case NodeType::Wavetable:
        // "value" here - unlike Knob below - is ALWAYS a real audio port: an
        // LFO is by nature a continuous signal, not an occasionally changed
        // value.
        return is(portName, "value") ? 0 : -1;
    case NodeType::Detune:
        return is(portName, "out") ? 0 : -1;
    case NodeType::Knob:
        // "value" is deliberately NEVER a real audio port: a Knob typically
        // changes a few times per second, not 48000x - it is delivered
        // exclusively via ControlRoute (see GraphBuilder.cpp/
        // Graph::pushControlRouteValue), no additional edge-summing pass per
        // block needed.
        return -1;
    case NodeType::Const:
        // Const exposes only its scalar "value" through ControlRoute.
        return -1;
    case NodeType::Button:
    case NodeType::UpDownButton:
        // Like Knob: current value is delivered exclusively via ControlRoute
        // and is already available there at graph build time.
        return -1;
    case NodeType::Switch:
        return is(portName, "value") ? 0 : -1;
    case NodeType::OutputSwitch:
        return indexedOutPort(portName, 2);
    case NodeType::Adsr:
        return is(portName, "out") ? 0 : -1;
    case NodeType::DxEnvelope:
        return is(portName, "out") ? 0 : -1;
    case NodeType::Filter:
    case NodeType::CurtisFilter:
    case NodeType::Gain:
        return is(portName, "out") ? 0 : -1;
    case NodeType::EspEq:
        if (is(portName, "outL"))
            return 0;
        if (is(portName, "outR"))
            return 1;
        return -1;
    case NodeType::GraphicEq:
        if (is(portName, "outL"))
            return 0;
        if (is(portName, "outR"))
            return 1;
        return -1;
    case NodeType::Mixer:
        return is(portName, "out") ? 0 : -1;
    case NodeType::Multiplexer:
        return is(portName, "value") ? 0 : -1;
    case NodeType::OutputMultiplexer:
        return indexedOutPort(portName, 16); // NodeState.h's kOutputMultiplexerMaxOutputs
    case NodeType::Multiplier:
    case NodeType::Adder:
    case NodeType::Divide:
    case NodeType::LogicAnd:
    case NodeType::LogicOr:
    case NodeType::LogicXor:
    case NodeType::LogicNot:
        return is(portName, "value") ? 0 : -1;
    case NodeType::FrequencyDivider:
        return (is(portName, "cv") || is(portName, "hz")) ? 0 : -1;
    case NodeType::Abs:
        return is(portName, "value") ? 0 : -1;
    case NodeType::ValueMapper:
        return is(portName, "out") ? 0 : -1;
    case NodeType::Bitcrusher:
        return is(portName, "out") ? 0 : -1;
    case NodeType::WaveFolder:
        return is(portName, "out") ? 0 : -1;
    case NodeType::EnvelopeFollower:
        return is(portName, "out") ? 0 : -1;
    case NodeType::BurstGenerator:
        return is(portName, "out") ? 0 : -1;
    case NodeType::Pan:
        if (is(portName, "out1"))
            return 0;
        if (is(portName, "out2"))
            return 1;
        return -1;
    case NodeType::SampleHold:
        return is(portName, "out") ? 0 : -1;
    case NodeType::Chorus:
        if (is(portName, "left"))
            return 0;
        if (is(portName, "right"))
            return 1;
        return -1;
    case NodeType::Delay:
    case NodeType::ReverbLight:
        if (is(portName, "outL"))
            return 0;
        if (is(portName, "outR"))
            return 1;
        return -1;
    case NodeType::AudioOutputLink:
        // No user-facing output port - internal bridge only.
        return -1;
    case NodeType::LinkedInput:
        // No visible output port - the hidden linked-io edge sources from
        // this widget's inPorts[0] directly (see GraphBuilder.h), never from
        // a named audioOutIndex lookup.
        return -1;
    case NodeType::LinkedOutput:
        if (is(portName, "out")) return 0;
        return indexedOutPort(portName, synth::kLinkBusCount);
    case NodeType::AudioInputLink:
        if (is(portName, "outL")) // legacy bus 1
            return 0;
        if (is(portName, "outR")) // legacy bus 2
            return 1;
        if (portName != nullptr && strncmp(portName, "out", 3) == 0)
        {
            const char *number = portName + 3;
            if (number[0] >= '1' && number[0] <= '8' && number[1] == '\0')
                return number[0] - '1';
        }
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
    case NodeType::MdaPiano:
        if (is(portName, "left")) return 0;
        if (is(portName, "right")) return 1;
        return -1;
    case NodeType::Clock:
        if (is(portName, "pulse"))
            return 0;
        if (is(portName, "frequency"))
            return 1;
        return -1;
    case NodeType::CvIn:
    case NodeType::DigitalIn:
        return is(portName, "value") ? 0 : -1;
    case NodeType::StepArpeggiator:
        if (is(portName, "cv"))
            return 0;
        if (is(portName, "gateOut"))
            return 1;
        return -1;
    case NodeType::DrumSequencer:
        if (portName != nullptr)
        {
            const bool trigger = strncmp(portName, "trigger", 7) == 0;
            const bool velocity = strncmp(portName, "velocity", 8) == 0;
            const char *number = trigger ? portName + 7 : (velocity ? portName + 8 : nullptr);
            if (number != nullptr && number[0] >= '1' && number[0] <= '8' && number[1] == '\0')
            {
                const int track = number[0] - '1';
                return track * 2 + (velocity ? 1 : 0);
            }
        }
        return -1;
    case NodeType::AudioOut:
    case NodeType::VuMeter:
    case NodeType::Oscilloscope:
    case NodeType::Spectrum:
    case NodeType::MidiIn:
    case NodeType::MidiSplit:
    case NodeType::CvOut:
    case NodeType::DigitalOut:
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
        return 6; // existing five inputs + internal-feedback depth (CV)
    case NodeType::FmOscillator:
        return 11; // existing nine + SuperSaw detune/mix
    case NodeType::Hammond:
        return 13; // "amplitude"+"pitchCv"+"tune"+"gain" (CV) + "drawbar1".."drawbar9" (CV)
    case NodeType::Lfo:
        return 4; // "reset" + "oneShot" + "global" + "zeroPoint" (CV)
    case NodeType::Wavetable:
        return 4; // "reset" + "oneShot" + "global" + "morph" (CV)
    case NodeType::Detune:
        return 1;
    case NodeType::Switch:
        return 2;
    case NodeType::OutputSwitch:
        return 1; // "in"
    case NodeType::Filter:
    case NodeType::CurtisFilter:
        return 6; // "in" + "filterType" + "slope" + "cutoff" + "resonance" + "global" (CV)
    case NodeType::Gain:
        return 4; // "in" + "amount" (CV) + "curve" (CV) + "global" (CV)
    case NodeType::EspEq:
        return 2;
    case NodeType::GraphicEq:
        return 2;
    case NodeType::Mixer:
    case NodeType::Multiplier:
    case NodeType::Adder:
    case NodeType::LogicAnd:
    case NodeType::LogicOr:
        return 8;
    case NodeType::Multiplexer:
        return 17; // "in1".."in16" + "select" (CV)
    case NodeType::OutputMultiplexer:
        return 2; // "in" + "select" (CV)
    case NodeType::Divide:
    case NodeType::LogicXor:
        return 2;
    case NodeType::FrequencyDivider:
        return 2;
    case NodeType::Abs:
    case NodeType::ValueMapper:
    case NodeType::LogicNot:
        return 1;
    case NodeType::Bitcrusher:
        return 3;
    case NodeType::WaveFolder:
        return 5;
    case NodeType::EnvelopeFollower:
        return 4; // "in" + "attack" + "release" + "preGain" (CV)
    case NodeType::BurstGenerator:
        return 5; // "in" + "mode" + "count" + "time" + "decay" (CV)
    case NodeType::Pan:
        return 2; // "in" + "pan"
    case NodeType::SampleHold:
        return 3; // "in" + "trigger" + "threshold", all audio-rate
    case NodeType::Chorus:
        return 1;
    case NodeType::Delay:
        return 6; // "inL"+"inR"+"bypass"+"time"+"feedback"+"mix" (CV)
    case NodeType::ReverbLight:
        return 7; // "inL"+"inR"+"bypass"+"roomSize"+"damping"+"wetLevel"+"dryLevel" (CV)
    case NodeType::AudioOutputLink:
        return synth::kLinkBusCount;
    case NodeType::AudioInputLink:
        // Eight page-link buses plus eight visible level-CV inputs.
        return synth::kLinkBusCount * 2;
    case NodeType::AudioOut:
    case NodeType::UsbOut:
    case NodeType::Es8311Out:
    case NodeType::Wm8960Out:
        return 4;
    case NodeType::CvOut:
    case NodeType::DigitalOut:
        return 1; // "in"
    case NodeType::VuMeter:
    case NodeType::Spectrum:
        return 1;
    case NodeType::LinkedInput:
    case NodeType::LinkedOutput:
        // LinkedInput's slot 0 is its visible "in"; LinkedOutput's slot 0
        // is the hidden landing point for its resolved linked-io edge
        // (never reached via audioInIndex by name - only via a raw-index
        // wireConnection() call from GraphBuilder, same as the fixed 8-bus
        // link's hidden bus slots).
        return synth::kLinkBusCount;
    case NodeType::Oscilloscope:
        return 2;
    case NodeType::WavePlayer:
        return 4; // "amplitude"+"note"+"gate"+"gain" (CV)
    case NodeType::SoundFontPlayer:
        return 6; // "bank"+"program"+"enableCc"+"enableModWheel"+"enablePitchWheel"+"gain" (CV)
    case NodeType::MdaPiano:
        return 12; // wheel switches + voice/effect controls + final gain
    case NodeType::Adsr:
        return 6; // "att"+"dec"+"sus"+"rel"+"gate"+"velocity" (CV)
    case NodeType::DxEnvelope:
        return 9; // four level plus four time CV inputs, plus "gate" (CV)
    case NodeType::StepArpeggiator:
        return 22; // "clock"+"gate"+"pitch"+"direction"+"pulseTime"+"rate" + "step1".."step16" (CV)
    case NodeType::DrumSequencer:
        return 5; // "clock" + "gate" + "pattern" (control-only) + internal-fallback "rate" + "switchAtEnd" (control-only)
    case NodeType::Knob:
    case NodeType::Button:
        return 1; // externally controlled layer selector
    case NodeType::MidiIn:
    case NodeType::MidiSplit:
    case NodeType::Const:
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
    case NodeType::LogicAnd:
        // Neutral element for AND (like Multiplier's 1.0f) - an unconnected
        // input must not force the whole gate false.
        return cv::kGateOnVolts;
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
           type == NodeType::Spectrum || type == NodeType::MidiIn || type == NodeType::CvOut ||
           type == NodeType::DigitalOut;
    // Note: Button/UpDownButton/Switch are deliberately NOT listed here even
    // though they may have no *audio* output port - hasNoOutputs() gates the
    // voiced/global BFS sink check, and these types still produce a value
    // (via ControlRoute or, for Switch, a real audio out port).
    //
    // LinkedInput is ALSO deliberately not listed (removed - it used to be
    // here): it has no *named* audioOutIndex (see audioOutIndex(LinkedInput,
    // ...) below), but it is not a dead end - it still reaches a
    // linkId-paired LinkedOutput via the hidden edge GraphBuilder's
    // "Linked-IO resolution" pass wires up. Treating it as a hard BFS
    // boundary here meant a per-voice source (e.g. MidiIn's "gate") routed
    // through a linked-input/linked-output pair lost its per-voice identity
    // - GraphBuilder::classifyVoiced()'s own special-case for
    // NodeType::LinkedInput (see GraphBuilder.h) is what actually carries
    // voicedness across the linkId pairing now; this predicate just needs to
    // stop treating it as a stop sign.
}

// Heavy post-mix effects that must never be multiplied per synth voice.
inline bool isGlobalOnly(NodeType type)
{
    return type == NodeType::EspEq || type == NodeType::GraphicEq || type == NodeType::SoundFontPlayer || type == NodeType::MdaPiano || type == NodeType::Delay ||
           type == NodeType::ReverbLight || type == NodeType::Bitcrusher || type == NodeType::WaveFolder ||
           type == NodeType::DrumSequencer || type == NodeType::EnvelopeFollower;
}

// True if input port `portIdx` on `type` must never be fed by a genuinely
// variable audio-rate signal (Oscillator/Filter/LFO/...), even though it has
// a real audioInIndex of its own (needed so a ControlRoute-delivered value -
// e.g. a Knob, or a Multiplier/Adder/Divide result - can still reach it
// efficiently as a per-sample-constant edge). Multiplier/Adder/Divide's
// inputs (any index), Oscillator's "pitchCv"/"tune" (indices 1/2, NOT
// "amplitude"/index 0 which stays genuinely audio-rate), Detune's "cv"
// (index 0), and Lfo's "reset" (index 0) are control-only this way. Enforced
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
    case NodeType::LogicAnd:
    case NodeType::LogicOr:
    case NodeType::LogicXor:
    case NodeType::LogicNot:
        return true; // all inputs
    case NodeType::Oscillator:
        return portIdx == 1 || portIdx == 2 || portIdx == 3 || portIdx == 5; // pitch/tune/waveform/feedback
    case NodeType::FmOscillator:
        // "pitchCv"(1)+"tune"(2)+"waveform"(3)+"fmIndex"(6)+"feedback"(7)+
        // "dutyCycle"(8)+SuperSaw detune/mix(9/10) - "amplitude"(0),
        // "gain"(4) AND "fmIn"(5) stay genuinely audio-rate; "fmIn" being
        // read per-sample is the entire point of this node.
        return portIdx == 1 || portIdx == 2 || portIdx == 3 || portIdx == 6 || portIdx == 7 ||
               portIdx == 8 || portIdx == 9 || portIdx == 10;
    case NodeType::Hammond:
        // Only "pitchCv"(1)/"tune"(2) are control-only, exactly like
        // Oscillator. "amplitude"(0), "gain"(3) AND all nine drawbars
        // (4-12) stay genuinely audio-rate - the drawbars are the whole
        // point of the port-limit widening (SynthConfig.h); a block-rate
        // read here would defeat it.
        return portIdx == 1 || portIdx == 2;
    case NodeType::Detune:
        return portIdx == 0; // "cv" - same reasoning as Oscillator's pitchCv
    case NodeType::Gain:
        return portIdx == 3; // "global" - "amount"(1) and "curve"(2) both stay real audio-rate
    case NodeType::Filter:
    case NodeType::CurtisFilter:
        return portIdx >= 1; // "filterType"+"slope"+"cutoff"+"resonance"+"global"
    case NodeType::Multiplexer:
        return portIdx == 16; // "select" - discrete pick, not a modulatable audio-rate signal (see nodes/MultiplexerNode.h)
    case NodeType::OutputMultiplexer:
        return portIdx == 1; // "select" - same reasoning as Multiplexer's own "select" (see nodes/OutputMultiplexerNode.h)
    case NodeType::Knob:
        return true; // "layer" is a discrete selector
    case NodeType::DrumSequencer:
        return portIdx == 2 || portIdx == 4; // "pattern" (discrete pick) + "switchAtEnd" (on/off, same reasoning as LFO's oneShot/global)
    case NodeType::Adsr:
        return true; // "att"/"dec"/"sus"/"rel"/"gate"/"velocity" - all six
    case NodeType::DxEnvelope:
        return true; // four level, four time, and "gate" inputs, all block-rate
    case NodeType::Lfo:
        return portIdx == 0 || portIdx == 1 || portIdx == 2 || portIdx == 3; // "reset"+"oneShot"+"global"+"zeroPoint" - trigger-/gate-/slider-like signals, control-rate suffices
    case NodeType::Wavetable:
        return portIdx <= 2; // "reset"+"oneShot"+"global" like the LFO - "morph"(3) stays audio-rate
    case NodeType::WavePlayer:
        return portIdx == 1 || portIdx == 2; // "note"+"gate" - "amplitude" (index 0) stays audio-rate
    case NodeType::SoundFontPlayer:
        return portIdx != 5; // event/scalar inputs, control-only - except "gain" (index 5), real audio-rate like Oscillator's
    case NodeType::MdaPiano:
        return portIdx != 11; // all controls except final gain are block-rate
    case NodeType::Delay:
        return portIdx >= 2; // "bypass"+"time"+"feedback"+"mix"
    case NodeType::Bitcrusher:
        return portIdx >= 1; // "bypass" + "crush"
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
