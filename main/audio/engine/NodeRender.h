#pragma once

#include <cstddef>
#include <cstring>

#include "esp_attr.h"

#include "CvConventions.h"
#include "EspAdditionalEffects.h"
#include "EspEqEffect.h"
#include "NodeInstance.h"
#include "SoundFontPlayer.h"
#include "SynthConfig.h"

#include "nodes/AdsrNode.h"
#include "nodes/ArithmeticNodes.h"
#include "nodes/ButtonNode.h"
#include "nodes/ChorusNode.h"
#include "nodes/ClockNode.h"
#include "nodes/CurtisFilterNode.h"
#include "nodes/ConstNode.h"
#include "nodes/DelayNode.h"
#include "nodes/DetuneNode.h"
#include "nodes/DxEnvelopeNode.h"
#include "nodes/FilterNode.h"
#include "nodes/FmOscillatorNode.h"
#include "nodes/GainNode.h"
#include "nodes/HammondNode.h"
#include "nodes/KnobNode.h"
#include "nodes/LfoNode.h"
#include "nodes/LinkNodes.h"
#include "nodes/MixerNode.h"
#include "nodes/MultiplexerNode.h"
#include "nodes/OscillatorNode.h"
#include "nodes/OutputNodes.h"
#include "nodes/ReverbNode.h"
#include "nodes/SwitchNode.h"
#include "nodes/ValueMapperNode.h"
#include "nodes/LinkedIONodes.h"
#include "nodes/WavePlayerNode.h"

// Switch-dispatch DSP render for one block, aggregating every node type's
// own render() (see nodes/*.h) into one call site. Reads inst.inPorts
// (already filled by Graph::renderBlock's connection-summing pass) and
// writes inst.outL/outR. Kept out of NodeInstance.h: it needs every
// nodes/*.h's full definition, but every nodes/*.h needs NodeInstance fully
// defined first - putting the dispatcher here (downstream of both) avoids
// that include cycle.
// IRAM_ATTR: this dispatcher runs once per rendered instance per block for
// every node type in the engine - see documentation/native-synth-
// optimization-plan-2026-09-09.md section 1c / DspPrimitives.h's
// sineLookup() comment for why IRAM placement is worth it here.
IRAM_ATTR inline void processNodeBlock(NodeInstance &inst, size_t frames)
{
    switch (inst.type)
    {
    case NodeType::Oscillator:
        nodes::oscillator::render(inst, frames);
        break;
    case NodeType::FmOscillator:
        nodes::fmoscillator::render(inst, frames);
        break;
    case NodeType::Hammond:
        nodes::hammond::render(inst, frames);
        break;
    case NodeType::Lfo:
        nodes::lfo::render(inst, frames);
        break;
    case NodeType::Clock:
        nodes::clock::render(inst, frames);
        break;
    case NodeType::Detune:
        nodes::detune::render(inst, frames);
        break;
    case NodeType::Const:
        nodes::constant::render(inst, frames);
        break;
    case NodeType::Knob:
        nodes::knob::render(inst, frames);
        break;
    case NodeType::Button:
        nodes::button::renderButton(inst, frames);
        break;
    case NodeType::UpDownButton:
        nodes::button::renderUpDownButton(inst, frames);
        break;
    case NodeType::Switch:
        nodes::switchnode::render(inst, frames);
        break;
    case NodeType::Adsr:
        nodes::adsr::render(inst, frames);
        break;
    case NodeType::DxEnvelope:
        nodes::dxenvelope::render(inst, frames);
        break;
    case NodeType::Filter:
        nodes::filter::render(inst, frames);
        break;
    case NodeType::CurtisFilter:
        nodes::curtisfilter::render(inst, frames);
        break;
    case NodeType::Gain:
        nodes::gain::render(inst, frames);
        break;
    case NodeType::Mixer:
        nodes::mixer::render(inst, frames);
        break;
    case NodeType::Multiplexer:
        nodes::multiplexer::render(inst, frames);
        break;
    case NodeType::EspEq:
        processEspEq(*inst.espEq, inst.inPorts[0], inst.inPorts[1], inst.outL, inst.outR, frames);
        break;
    case NodeType::EspAlc:
    case NodeType::EspChannelConvert:
    case NodeType::EspMixer:
    case NodeType::EspSonic:
    case NodeType::EspDrc:
    case NodeType::EspMbc:
        processEspEffect(*inst.espEffect, inst.inPorts, inst.audioInPortCount, inst.outL, inst.outR, frames);
        break;
    case NodeType::Multiplier:
        nodes::arithmetic::renderMultiplier(inst, frames);
        break;
    case NodeType::Adder:
        nodes::arithmetic::renderAdder(inst, frames);
        break;
    case NodeType::Divide:
        nodes::arithmetic::renderDivide(inst, frames);
        break;
    case NodeType::FrequencyDivider:
        nodes::arithmetic::renderFrequencyDivider(inst, frames);
        break;
    case NodeType::Abs:
        nodes::arithmetic::renderAbs(inst, frames);
        break;
    case NodeType::ValueMapper:
        nodes::valuemapper::render(inst, frames);
        break;
    case NodeType::Chorus:
        nodes::chorus::render(inst, frames);
        break;
    case NodeType::Delay:
        nodes::delay::render(inst, frames);
        break;
    case NodeType::Reverb:
        nodes::reverb::renderReverb(inst, frames);
        break;
    case NodeType::ReverbLight:
        nodes::reverb::renderReverbLight(inst, frames);
        break;
    case NodeType::AudioOutputLink:
    case NodeType::AudioInputLink:
        nodes::link::render(inst, frames);
        break;
    case NodeType::LinkedInput:
    case NodeType::LinkedOutput:
        nodes::linkedio::render(inst, frames);
        break;
    case NodeType::AudioOut:
    case NodeType::UsbOut:
    case NodeType::Es8311Out:
    case NodeType::Wm8960Out:
        nodes::output::render(inst, frames);
        break;
    case NodeType::WavePlayer:
        nodes::waveplayer::render(inst, frames);
        break;
    case NodeType::SoundFontPlayer:
    {
        using namespace noderender;
        auto &s = inst.soundFontPlayer;
        const bool bankConnected = (inst.controlConnectedPortMask & 1u) != 0 || (inst.audioConnectedPortMask & 1u) != 0;
        const bool programConnected = (inst.controlConnectedPortMask & 2u) != 0 || (inst.audioConnectedPortMask & 2u) != 0;
        const int requestedBank = bankConnected ? static_cast<int>(portSample(inst, 0, 0) + 0.5f) : s.bank;
        const int requestedProgram = programConnected ? static_cast<int>(portSample(inst, 1, 0) + 0.5f) : s.program;
        if (requestedBank != s.bank || requestedProgram != s.program)
        {
            soundfont::setBankProgram(s, requestedBank, requestedProgram);
        }
        if ((inst.controlConnectedPortMask | inst.audioConnectedPortMask) & (1u << 2))
            s.enableCc = portSample(inst, 2, 0) >= 0.5f;
        if ((inst.controlConnectedPortMask | inst.audioConnectedPortMask) & (1u << 3))
            s.enableModWheel = portSample(inst, 3, 0) >= 0.5f;
        if ((inst.controlConnectedPortMask | inst.audioConnectedPortMask) & (1u << 4))
            s.enablePitchWheel = portSample(inst, 4, 0) >= 0.5f;
        // "gain" (Port-Index 5) - same pattern as Oscillator's "gain": when
        // wired the CV already arrives natively per sample, when unwired
        // the manual gainCv scalar is smoothed PER SAMPLE (see
        // SoundFontPlayerState/Oscillator case above for the reasoning).
        {
            const bool gainFed = (inst.audioConnectedPortMask & static_cast<uint8_t>(1u << 5)) != 0;
            const float gainTarget = gainFed ? 0.0f : cv::vcaGain(s.gainCv, false);
            // Same fast path/reasoning as Oscillator's gain==0 case: the knob
            // itself (not a live envelope) is parked at 0 and the smoother has
            // already ramped down to inaudible, so this block's tsf_render_float
            // call - by far the most expensive part of this node, a full
            // polyphonic SoundFont render - would just get multiplied down to
            // ~0 anyway. Skip it and write silence directly instead. Gated on
            // the already-converged smoothed value, not just gainCv, so a knob
            // turned down THIS block still fades out through the normal path
            // below exactly as before - no click. Active notes' envelopes
            // inside tsf freeze while skipped, same tradeoff Graph::renderBlock
            // already accepts project-wide for an idle-ADSR voice.
            constexpr float kSilentGain = 1e-4f;
            if (!gainFed && gainTarget <= 0.0f && s.gainSmooth.current() <= kSilentGain)
            {
                memset(inst.outL, 0, frames * sizeof(float));
                memset(inst.outR, 0, frames * sizeof(float));
                break;
            }
            const float *planar = soundfont::renderPlanar(s, frames);
            if (planar == nullptr)
            {
                memset(inst.outL, 0, frames * sizeof(float));
                memset(inst.outR, 0, frames * sizeof(float));
                break;
            }
            const float *planarL = planar;
            const float *planarR = planar + frames;
            if (gainFed)
            {
                for (size_t i = 0; i < frames; ++i)
                {
                    const float gainMul = cv::vcaGain(portSample(inst, 5, i), false) * cv::kAudioPeakVolts;
                    inst.outL[i] = planarL[i] * gainMul;
                    inst.outR[i] = planarR[i] * gainMul;
                }
            }
            else
            {
                for (size_t i = 0; i < frames; ++i)
                {
                    const float gainMul = s.gainSmooth.process(gainTarget) * cv::kAudioPeakVolts;
                    inst.outL[i] = planarL[i] * gainMul;
                    inst.outR[i] = planarR[i] * gainMul;
                }
            }
        }
        break;
    }
    case NodeType::VuMeter:
    case NodeType::Oscilloscope:
        break;
    case NodeType::MidiIn:
    case NodeType::MidiSplit:
    case NodeType::Unknown:
    default:
        break;
    }
}
