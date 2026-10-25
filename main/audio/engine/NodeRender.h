#pragma once

#include <cstddef>
#include <cstring>

#include "esp_attr.h"

#include "CvConventions.h"
#include "EspEqEffect.h"
#include "GraphicEqEffect.h"
#include "NodeInstance.h"
#include "SoundFontPlayer.h"
#include "SynthConfig.h"

#include "nodes/AdsrNode.h"
#include "nodes/ArithmeticNodes.h"
#include "nodes/ButtonNode.h"
#include "nodes/BitcrusherNode.h"
#include "nodes/WaveFolderNode.h"
#include "nodes/OverdriveNode.h"
#include "nodes/BurstGeneratorNode.h"
#include "nodes/EnvelopeFollowerNode.h"
#include "nodes/PanNode.h"
#include "nodes/ChorusNode.h"
#include "nodes/ClockNode.h"
#include "nodes/CvInNode.h"
#include "nodes/MicInNode.h"
#include "nodes/Wm8960InNode.h"
#include "nodes/CvOutNode.h"
#include "nodes/DigitalInNode.h"
#include "nodes/DigitalOutNode.h"
#include "nodes/MidiInNode.h"
#include "nodes/RgbLedNode.h"
#include "nodes/SampleHoldNode.h"
#include "nodes/CounterNode.h"
#include "nodes/MonoflopNode.h"
#include "nodes/CurtisFilterNode.h"
#include "nodes/CombFilterNode.h"
#include "nodes/ConstNode.h"
#include "nodes/DelayNode.h"
#include "nodes/DetuneNode.h"
#include "nodes/DxEnvelopeNode.h"
#include "nodes/FilterNode.h"
#include "nodes/FmOscillatorNode.h"
#include "nodes/Fm8Node.h"
#include "nodes/GainNode.h"
#include "nodes/HammondNode.h"
#include "nodes/KnobNode.h"
#include "nodes/LfoNode.h"
#include "nodes/WavetableNode.h"
#include "nodes/LinkNodes.h"
#include "nodes/MixerNode.h"
#include "nodes/MdaPianoNode.h"
#include "nodes/MultiplexerNode.h"
#include "nodes/OscillatorNode.h"
#include "nodes/OutputMultiplexerNode.h"
#include "nodes/OutputNodes.h"
#include "nodes/OutputSwitchNode.h"
#include "nodes/ReverbNode.h"
#include "nodes/StepArpeggiatorNode.h"
#include "nodes/DrumSequencerNode.h"
#include "nodes/SwitchNode.h"
#include "nodes/ValueMapperNode.h"
#include "nodes/LinkedIONodes.h"
#include "nodes/WavePlayerNode.h"
#include "nodes/SamplerNode.h"

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
    case NodeType::Fm8:
        nodes::fm8::render(inst, frames);
        break;
    case NodeType::Hammond:
        nodes::hammond::render(inst, frames);
        break;
    case NodeType::Lfo:
        nodes::lfo::render(inst, frames);
        break;
    case NodeType::Wavetable:
        nodes::wavetable::render(inst, frames);
        break;
    case NodeType::Clock:
        nodes::clock::render(inst, frames);
        break;
    case NodeType::CvIn:
        nodes::cvin::render(inst, frames);
        break;
    case NodeType::MicIn:
        nodes::micin::render(inst, frames);
        break;
    case NodeType::Wm8960In:
        nodes::wm8960in::render(inst, frames);
        break;
    case NodeType::CvOut:
        nodes::cvout::render(inst, frames);
        break;
    case NodeType::DigitalIn:
        nodes::digitalin::render(inst, frames);
        break;
    case NodeType::DigitalOut:
        nodes::digitalout::render(inst, frames);
        break;
    case NodeType::RgbLed:
        nodes::rgbled::render(inst, frames);
        break;
    case NodeType::SampleHold:
        nodes::samplehold::render(inst, frames);
        break;
    case NodeType::Counter:
        nodes::counter::render(inst, frames);
        break;
    case NodeType::Monoflop:
        nodes::monoflop::render(inst, frames);
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
    case NodeType::OutputSwitch:
        nodes::outputswitch::render(inst, frames);
        break;
    case NodeType::Adsr:
        nodes::adsr::render(inst, frames);
        break;
    case NodeType::DxEnvelope:
        nodes::dxenvelope::render(inst, frames);
        break;
    case NodeType::StepArpeggiator:
        nodes::steparp::render(inst, frames);
        break;
    case NodeType::DrumSequencer:
        nodes::drumsequencer::render(inst, frames);
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
    case NodeType::OutputMultiplexer:
        nodes::outputmultiplexer::render(inst, frames);
        break;
    case NodeType::EspEq:
    {
        using namespace noderender;
        // Wired control ports override the widget's settings once per block, same raw units
        // as the browser preview (Hz / Q / dB), clamped to the widget's slider ranges. The
        // filter coefficients are only recomputed when a value actually changes.
        auto &eq = *inst.espEq;
        if (portIsFed(inst, 2)) eq.bypass = portSample(inst, 2, 0) >= cv::kGateThresholdVolts;
        float frequency = eq.frequencyHz, q = eq.q, gainDb = eq.gainDb;
        if (portIsFed(inst, 3)) frequency = clampToRange(portSample(inst, 3, 0), 20.0f, 22049.0f);
        if (portIsFed(inst, 4)) q = clampToRange(portSample(inst, 4, 0), 0.1f, 20.0f);
        if (portIsFed(inst, 5)) gainDb = clampToRange(portSample(inst, 5, 0), -15.0f, 15.0f);
        if (frequency != eq.frequencyHz || q != eq.q || gainDb != eq.gainDb)
            updateEspEq(eq, eq.filterType, frequency, q, gainDb);
        processEspEq(eq, inst.inPorts[0], inst.inPorts[1], inst.outL, inst.outR, frames);
        break;
    }
    case NodeType::GraphicEq:
    {
        using namespace noderender;
        // Same override pattern as EspEq above: "bandN" maps 0-10V through that band's
        // min/max/curve (like the browser's EFFECT_CV_PARAMS), "gain" is raw 0-10V.
        auto &geq = *inst.graphicEq;
        if (portIsFed(inst, 2)) geq.bypass = portSample(inst, 2, 0) >= cv::kGateThresholdVolts;
        for (int band = 0; band < GraphicEqState::kBandCount; ++band)
        {
            if (!portIsFed(inst, 3 + band)) continue;
            const float volts = clampToRange(portSample(inst, 3 + band, 0), 0.0f, cv::kMaxVolts);
            const float gainDb = cv::cvToRange(volts, geq.bandMin[band], geq.bandMax[band], geq.bandLog[band]);
            if (gainDb != geq.gainDb[band]) updateGraphicEqBandGain(geq, band, gainDb);
        }
        if (portIsFed(inst, 11)) geq.outputGainCv = clampToRange(portSample(inst, 11, 0), 0.0f, cv::kMaxVolts);
        processGraphicEq(geq, inst.inPorts[0], inst.inPorts[1], inst.outL, inst.outR, frames);
        break;
    }
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
    case NodeType::LogicAnd:
        nodes::arithmetic::renderLogicAnd(inst, frames);
        break;
    case NodeType::LogicOr:
        nodes::arithmetic::renderLogicOr(inst, frames);
        break;
    case NodeType::LogicXor:
        nodes::arithmetic::renderLogicXor(inst, frames);
        break;
    case NodeType::LogicNot:
        nodes::arithmetic::renderLogicNot(inst, frames);
        break;
    case NodeType::ValueMapper:
        nodes::valuemapper::render(inst, frames);
        break;
    case NodeType::Bitcrusher:
        nodes::bitcrusher::render(inst, frames);
        break;
    case NodeType::WaveFolder:
        nodes::wavefolder::render(inst, frames);
        break;
    case NodeType::Overdrive:
        nodes::overdrive::render(inst, frames);
        break;
    case NodeType::EnvelopeFollower:
        nodes::envelopefollower::render(inst, frames);
        break;
    case NodeType::BurstGenerator:
        nodes::burstgenerator::render(inst, frames);
        break;
    case NodeType::Pan:
        nodes::pan::render(inst, frames);
        break;
    case NodeType::Chorus:
        nodes::chorus::render(inst, frames);
        break;
    case NodeType::CombFilter:
        nodes::combfilter::render(inst, frames);
        break;
    case NodeType::Delay:
        nodes::delay::render(inst, frames);
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
    case NodeType::Ns4168Out:
        nodes::output::render(inst, frames);
        break;
    case NodeType::WavePlayer:
        nodes::waveplayer::render(inst, frames);
        break;
    case NodeType::Sampler:
        nodes::sampler::render(inst, frames);
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
            // x2: tsf's center pan (-3 dB) and velocity curve leave a GM single note
            // around 0.5 of full scale (Creative 4M GM median at C4/vel 100); x2 brings
            // it near a full-scale Oscillator. Must match audio-engine.js's
            // _sf2ChannelGainNode().
            constexpr float kOutputScale = cv::kAudioPeakVolts * 2.0f;
            if (gainFed)
            {
                for (size_t i = 0; i < frames; ++i)
                {
                    const float gainMul = cv::vcaGain(portSample(inst, 5, i), false) * kOutputScale;
                    inst.outL[i] = planarL[i] * gainMul;
                    inst.outR[i] = planarR[i] * gainMul;
                }
            }
            else
            {
                for (size_t i = 0; i < frames; ++i)
                {
                    const float gainMul = s.gainSmooth.process(gainTarget) * kOutputScale;
                    inst.outL[i] = planarL[i] * gainMul;
                    inst.outR[i] = planarR[i] * gainMul;
                }
            }
        }
        break;
    }
    case NodeType::MdaPiano:
        nodes::mdapiano::render(inst, frames);
        break;
    case NodeType::VuMeter:
    case NodeType::Oscilloscope:
        break;
    case NodeType::MidiIn:
        nodes::midiin::render(inst, frames);
        break;
    case NodeType::MidiFilter:
    case NodeType::MidiSplit:
    case NodeType::Unknown:
    default:
        break;
    }
}
