#pragma once

#include <cmath>
#include <cstring>

#include "EspEqEffect.h"
#include "NodeInstance.h"
#include "NodeTypes.h"
#include "SoundFontPlayer.h"
#include "SynthConfig.h"

#include "nodes/AdsrNode.h"
#include "nodes/ArithmeticNodes.h"
#include "nodes/ButtonNode.h"
#include "nodes/BitcrusherNode.h"
#include "nodes/WaveFolderNode.h"
#include "nodes/PanNode.h"
#include "nodes/ChorusNode.h"
#include "nodes/ClockNode.h"
#include "nodes/SampleHoldNode.h"
#include "nodes/ConstNode.h"
#include "nodes/DelayNode.h"
#include "nodes/DetuneNode.h"
#include "nodes/DxEnvelopeNode.h"
#include "nodes/FilterNode.h"
#include "nodes/CurtisFilterNode.h"
#include "nodes/FmOscillatorNode.h"
#include "nodes/GainNode.h"
#include "nodes/HammondNode.h"
#include "nodes/KnobNode.h"
#include "nodes/LfoNode.h"
#include "nodes/LinkNodes.h"
#include "nodes/MidiSplitNode.h"
#include "nodes/MixerNode.h"
#include "nodes/MultiplexerNode.h"
#include "nodes/OscillatorNode.h"
#include "nodes/OutputMultiplexerNode.h"
#include "nodes/OutputNodes.h"
#include "nodes/ReverbNode.h"
#include "nodes/StepArpeggiatorNode.h"
#include "nodes/ValueMapperNode.h"

namespace
{
inline bool paramDispatchIs(const char *a, const char *b)
{
    return a != nullptr && strcmp(a, b) == 0;
}
} // namespace

// Writes a single named float param onto `inst` (whichever state struct
// matches `inst`'s type), without touching topology or reallocating any
// buffers - safe to call on a live, currently-rendering instance. Shared by
// both live paths that mutate a running Graph in place: Graph::
// handleMidiEvent's knob-CC-to-target routing, and Graph::applyLiveParams's
// browser-WS param patches. Returns false if `name` isn't a recognized
// float field for `type` (unknown param name, or a non-float field like
// waveType/filterType that the WS path handles separately).
inline bool setFloatFieldByName(NodeInstance &inst, NodeType type, const char *name, float value)
{
    switch (type)
    {
    case NodeType::Lfo:
        return nodes::lfo::setParam(inst, name, value);

    case NodeType::Oscillator:
        return nodes::oscillator::setParam(inst, name, value);

    case NodeType::FmOscillator:
        return nodes::fmoscillator::setParam(inst, name, value);

    case NodeType::Hammond:
        return nodes::hammond::setParam(inst, name, value);

    case NodeType::Detune:
        return nodes::detune::setParam(inst, name, value);

    case NodeType::FrequencyDivider:
        return nodes::arithmetic::setParamFrequencyDivider(inst, name, value);

    case NodeType::MidiSplit:
        return nodes::midisplit::setParam(inst, name, value);

    case NodeType::Filter:
        // "cutoffCurve"/"resonanceCurve" are strings ("linear"/"log"), same
        // pattern as "filterType" below - see Graph.cpp's applyLiveParams.
        return nodes::filter::setParam(inst, name, value);

    case NodeType::CurtisFilter:
        return nodes::curtisfilter::setParam(inst, name, value);

    case NodeType::Gain:
        // "curve" ("linear"/"exponential") is a string, same pattern as
        // Filter's "cutoffCurve" - see Graph.cpp's applyLiveParams.
        return nodes::gain::setParam(inst, name, value);

    case NodeType::Clock:
        return nodes::clock::setParam(inst, name, value);

    case NodeType::SampleHold:
        return nodes::samplehold::setParam(inst, name, value);

    case NodeType::WavePlayer:
        if (paramDispatchIs(name, "gain"))
        {
            inst.wavePlayer.gainCv = value;
            return true;
        }
        return false;

    case NodeType::SoundFontPlayer:
        if (paramDispatchIs(name, "bank"))
            return soundfont::setBankProgram(inst.soundFontPlayer, static_cast<int>(value + 0.5f),
                                             inst.soundFontPlayer.program);
        if (paramDispatchIs(name, "program"))
            return soundfont::setBankProgram(inst.soundFontPlayer, inst.soundFontPlayer.bank,
                                             static_cast<int>(value + 0.5f));
        if (paramDispatchIs(name, "enableCc"))
        {
            inst.soundFontPlayer.enableCc = value >= 0.5f;
            return true;
        }
        if (paramDispatchIs(name, "enableModWheel"))
        {
            inst.soundFontPlayer.enableModWheel = value >= 0.5f;
            return true;
        }
        if (paramDispatchIs(name, "enablePitchWheel"))
        {
            inst.soundFontPlayer.enablePitchWheel = value >= 0.5f;
            return true;
        }
        if (paramDispatchIs(name, "gain"))
        {
            inst.soundFontPlayer.gainCv = value;
            return true;
        }
        return false;

    case NodeType::EspEq:
    {
        auto &eq = *inst.espEq;
        if (paramDispatchIs(name, "bypass"))
        {
            eq.bypass = value >= 0.5f;
            return true;
        }
        if (paramDispatchIs(name, "frequency"))
            return updateEspEq(eq, eq.filterType, value, eq.q, eq.gainDb);
        if (paramDispatchIs(name, "q"))
            return updateEspEq(eq, eq.filterType, eq.frequencyHz, value, eq.gainDb);
        if (paramDispatchIs(name, "gainDb"))
            return updateEspEq(eq, eq.filterType, eq.frequencyHz, eq.q, value);
        return false;
    }
    case NodeType::Mixer:
        return nodes::mixer::setParam(inst, name, value);

    case NodeType::Multiplexer:
        return nodes::multiplexer::setParam(inst, name, value);

    case NodeType::OutputMultiplexer:
        return nodes::outputmultiplexer::setParam(inst, name, value);

    case NodeType::Bitcrusher:
        return nodes::bitcrusher::setParam(inst, name, value);

    case NodeType::WaveFolder:
        return nodes::wavefolder::setParam(inst, name, value);

    case NodeType::Pan:
        return nodes::pan::setParam(inst, name, value);

    case NodeType::Chorus:
        return nodes::chorus::setParam(inst, name, value);

    case NodeType::Delay:
        return nodes::delay::setParam(inst, name, value);


    case NodeType::ReverbLight:
        return nodes::reverb::setParamReverbLight(inst, name, value);

    case NodeType::Adsr:
        return nodes::adsr::setParam(inst, name, value);

    case NodeType::DxEnvelope:
        return nodes::dxenvelope::setParam(inst, name, value);

    case NodeType::StepArpeggiator:
        return nodes::steparp::setParam(inst, name, value);

    case NodeType::AudioOut:
    case NodeType::UsbOut:
    case NodeType::Es8311Out:
    case NodeType::Wm8960Out:
        return nodes::output::setParam(inst, name, value);

    case NodeType::AudioInputLink:
        return nodes::link::setParam(inst, name, value);

    case NodeType::Knob:
        return nodes::knob::setParam(inst, name, value);

    case NodeType::ValueMapper:
        return nodes::valuemapper::setParam(inst, name, value);

    case NodeType::Const:
        return nodes::constant::setParam(inst, name, value);

    case NodeType::Button:
        return nodes::button::setParamButton(inst, name, value);

    case NodeType::UpDownButton:
        return nodes::button::setParamUpDownButton(inst, name, value);

    default:
        return false;
    }
}
