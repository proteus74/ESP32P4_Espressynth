#pragma once

#include <cstddef>
#include <cstring>
#include <new>

#include "esp_heap_caps.h"
#include "MdaPianoBank.h"
#include "mdaEPiano.h"
#include "reface_cp_chain.h"

#include "NodeInstance.h"
#include "PresetParser.h"
#include "UsbMidiEvent.h"

namespace nodes::mdapiano
{
struct Runtime
{
    Runtime(uint8_t maxVoices, int initialInstrument) : piano(maxVoices)
    {
        piano.setSampleRate(static_cast<float>(synth::kSampleRate));
        effects.init(static_cast<float>(synth::kSampleRate));
        ready = ::mdapiano::loadBank();
        if (ready) selectInstrument(initialInstrument);
    }

    bool selectInstrument(int index)
    {
        const ::mdapiano::Instrument *instrument = ::mdapiano::instrument(index);
        if (instrument == nullptr) return false;
        piano.loadInstrument(instrument->pcm, instrument->keygroups,
                             static_cast<int32_t>(instrument->keygroupCount),
                             static_cast<int32_t>(instrument->noteonStep));
        currentInstrument = index;
        return true;
    }

    mdaEPiano piano;
    RefaceCpChain effects;
    int16_t scratchL[synth::kBlockSize]{};
    int16_t scratchR[synth::kBlockSize]{};
    int currentInstrument = -1;
    bool ready = false;
};

inline Runtime *runtime(MdaPianoState &state)
{
    return static_cast<Runtime *>(state.engine);
}

inline float clampCv(float value)
{
    return value < 0.0f ? 0.0f : (value > 10.0f ? 10.0f : value);
}

inline int voiceTypeFromCv(float value)
{
    const int voice = static_cast<int>(clampCv(value) * 0.6f);
    return voice > 5 ? 5 : voice;
}

inline int effectModeFromCv(float value)
{
    const int mode = static_cast<int>(clampCv(value) * 0.3f);
    return mode > 2 ? 2 : mode;
}

inline void allSoundOff(MdaPianoState &state)
{
    if (Runtime *rt = runtime(state)) rt->piano.resetVoices();
    state.pendingNoteOn = false;
    state.gatePrev = false;
}

inline void noteOff(MdaPianoState &state, int note)
{
    if (Runtime *rt = runtime(state)) rt->piano.noteOff(note);
    state.pendingNoteOn = false;
}

inline void build(NodeInstance &inst, const WidgetParams &params)
{
    auto &state = inst.mdaPiano;
    state.enableModWheel = params.mdaPianoEnableModWheel;
    state.enablePitchWheel = params.mdaPianoEnablePitchWheel;
    state.voiceTypeCv = clampCv(params.mdaPianoVoiceType);
    state.driveCv = clampCv(params.mdaPianoDrive);
    state.tremWahModeCv = clampCv(params.mdaPianoTremWahMode);
    state.tremWahDepthCv = clampCv(params.mdaPianoTremWahDepth);
    state.tremWahRateCv = clampCv(params.mdaPianoTremWahRate);
    state.choPhaModeCv = clampCv(params.mdaPianoChoPhaMode);
    state.choPhaDepthCv = clampCv(params.mdaPianoChoPhaDepth);
    state.choPhaSpeedCv = clampCv(params.mdaPianoChoPhaSpeed);
    state.preGainCv = clampCv(params.mdaPianoPreGain);
    state.gainCv = clampCv(params.gain);
    state.gainSmooth.setCutoff(300.0f, static_cast<float>(synth::kSampleRate));

    void *storage = heap_caps_malloc(sizeof(Runtime), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (storage == nullptr)
        storage = heap_caps_malloc(sizeof(Runtime), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (storage != nullptr)
        state.engine = new (storage) Runtime(static_cast<uint8_t>(params.mdaPianoMaxVoices),
                             voiceTypeFromCv(state.voiceTypeCv));
}

inline void destroy(NodeInstance &inst)
{
    auto &state = inst.mdaPiano;
    if (Runtime *rt = runtime(state))
    {
        rt->~Runtime();
        heap_caps_free(rt);
        state.engine = nullptr;
    }
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    auto &state = inst.mdaPiano;
    const float cv = clampCv(value);
    if (strcmp(name, "enableModWheel") == 0) state.enableModWheel = value >= 0.5f;
    else if (strcmp(name, "enablePitchWheel") == 0) state.enablePitchWheel = value >= 0.5f;
    else if (strcmp(name, "voiceType") == 0)
    {
        if (voiceTypeFromCv(cv) != voiceTypeFromCv(state.voiceTypeCv)) allSoundOff(state);
        state.voiceTypeCv = cv;
    }
    else if (strcmp(name, "drive") == 0) state.driveCv = cv;
    else if (strcmp(name, "tremWahMode") == 0) state.tremWahModeCv = cv;
    else if (strcmp(name, "tremWahDepth") == 0) state.tremWahDepthCv = cv;
    else if (strcmp(name, "tremWahRate") == 0) state.tremWahRateCv = cv;
    else if (strcmp(name, "choPhaMode") == 0) state.choPhaModeCv = cv;
    else if (strcmp(name, "choPhaDepth") == 0) state.choPhaDepthCv = cv;
    else if (strcmp(name, "choPhaSpeed") == 0) state.choPhaSpeedCv = cv;
    else if (strcmp(name, "preGain") == 0) state.preGainCv = cv;
    else if (strcmp(name, "gain") == 0) state.gainCv = cv;
    else return false;
    return true;
}

inline void handleMidi(MdaPianoState &state, const UsbMidiEvent &event)
{
    Runtime *rt = runtime(state);
    switch (event.codeIndex)
    {
    case 0x8:
        if (rt != nullptr) rt->piano.noteOff(event.data1);
        if (event.data1 == state.currentNote) state.gatePrev = false;
        break;
    case 0x9:
        state.currentNote = event.data1;
        state.pendingVelocity = event.data2 / 127.0f;
        if (rt != nullptr && rt->ready)
        {
            if (event.data2 == 0) rt->piano.noteOff(event.data1);
            else rt->piano.noteOn(event.data1, event.data2);
        }
        state.pendingNoteOn = false;
        state.gatePrev = event.data2 != 0;
        break;
    case 0xB:
        if (state.enableModWheel && (event.status & 0x0F) == 0 && event.data1 == 1)
            state.modWheel = event.data2 / 127.0f;
        // Sustain (64), sostenuto (66 - mdaEPiano treats it as sustain),
        // All Sound Off (120), All Notes Off (123): handled natively by the
        // engine. Other CCs stay filtered - 7/126/127 would change volume or
        // polyphony behind the node's back.
        else if (rt != nullptr && (event.data1 == 64 || event.data1 == 66 ||
                                   event.data1 == 120 || event.data1 == 123))
            rt->piano.processMidiController(event.data1, event.data2);
        break;
    case 0xE:
        if (state.enablePitchWheel)
        {
            state.pitchBend14 = (event.data2 << 7) | event.data1;
            if (rt != nullptr) rt->piano.setPitchBend(state.pitchBend14);
        }
        break;
    default:
        break;
    }
}

inline void render(NodeInstance &inst, size_t frames)
{
    auto &state = inst.mdaPiano;
    Runtime *rt = runtime(state);
    if (rt == nullptr || !rt->ready || frames > synth::kBlockSize)
    {
        memset(inst.outL, 0, frames * sizeof(float));
        memset(inst.outR, 0, frames * sizeof(float));
        return;
    }
    float *values[] = {
        nullptr, nullptr, &state.voiceTypeCv, &state.driveCv, &state.tremWahModeCv,
        &state.tremWahDepthCv, &state.tremWahRateCv, &state.choPhaModeCv,
        &state.choPhaDepthCv, &state.choPhaSpeedCv, &state.preGainCv, &state.gainCv,
    };
    const synth::PortMask connected = inst.controlConnectedPortMask | inst.audioConnectedPortMask;
    if (connected & synth::portBit(0)) state.enableModWheel = noderender::portSample(inst, 0, 0) >= 0.5f;
    if (connected & synth::portBit(1)) state.enablePitchWheel = noderender::portSample(inst, 1, 0) >= 0.5f;
    for (int port = 2; port < 12; ++port)
    {
        if ((connected & synth::portBit(port)) == 0) continue;
        const float cv = clampCv(noderender::portSample(inst, port, 0));
        if (port == 2 && voiceTypeFromCv(cv) != voiceTypeFromCv(state.voiceTypeCv)) allSoundOff(state);
        *values[port] = cv;
    }

    const int selectedInstrument = voiceTypeFromCv(state.voiceTypeCv);
    if (selectedInstrument != rt->currentInstrument)
    {
        allSoundOff(state);
        if (!rt->selectInstrument(selectedInstrument))
        {
            memset(inst.outL, 0, frames * sizeof(float));
            memset(inst.outR, 0, frames * sizeof(float));
            return;
        }
    }

    if (state.pendingNoteOn)
    {
        rt->piano.noteOn(state.currentNote, static_cast<int>(state.pendingVelocity * 127.0f));
        state.pendingNoteOn = false;
    }

    rt->effects.setVoiceType(selectedInstrument);
    rt->effects.setDrive(state.driveCv * 0.1f);
    rt->effects.setTremWahMode(effectModeFromCv(state.tremWahModeCv));
    rt->effects.setTremWahDepth(state.tremWahDepthCv * 0.1f * state.modWheel);
    rt->effects.setTremWahRate(state.tremWahRateCv * 0.1f);
    rt->effects.setChoPhaMode(effectModeFromCv(state.choPhaModeCv));
    rt->effects.setChoPhaDepth(state.choPhaDepthCv * 0.1f);
    rt->effects.setChoPhaSpeed(state.choPhaSpeedCv * 0.1f);
    rt->effects.setPreGain(state.preGainCv * 0.1f);

    rt->piano.process(rt->scratchR, rt->scratchL, static_cast<int32_t>(frames));
    constexpr float kInt16ToFloat = 1.0f / 32768.0f;
    for (size_t i = 0; i < frames; ++i)
    {
        inst.outL[i] = static_cast<float>(rt->scratchL[i]) * kInt16ToFloat;
        inst.outR[i] = static_cast<float>(rt->scratchR[i]) * kInt16ToFloat;
    }
    rt->effects.process(inst.outL, inst.outR, static_cast<int>(frames));

    // mda-ePiano peaks at ~0.2 for a single velocity-127 note (internal volume 0.2,
    // final *0.5); x5 brings that to cv::kAudioPeakVolts like a full-scale Oscillator.
    // Must match audio-engine.js's mda-piano processor.
    constexpr float kOutputScale = cv::kAudioPeakVolts * 5.0f;
    const bool gainFed = (connected & synth::portBit(11)) != 0;
    const float gainTarget = cv::vcaGain(state.gainCv, false);
    for (size_t i = 0; i < frames; ++i)
    {
        const float gain = gainFed
            ? cv::vcaGain(clampCv(noderender::portSample(inst, 11, i)), false) * kOutputScale
            : state.gainSmooth.process(gainTarget) * kOutputScale;
        inst.outL[i] *= gain;
        inst.outR[i] *= gain;
    }
}

inline void parsePreset(cJSON *params, WidgetParams &out)
{
    out.mdaPianoMaxVoices = presetparser::getInt(params, "maxVoices", 32);
    if (out.mdaPianoMaxVoices < 4) out.mdaPianoMaxVoices = 4;
    if (out.mdaPianoMaxVoices > 128) out.mdaPianoMaxVoices = 128;
    out.mdaPianoEnableModWheel = presetparser::getBool(params, "enableModWheel", false);
    out.mdaPianoEnablePitchWheel = presetparser::getBool(params, "enablePitchWheel", false);
    out.mdaPianoVoiceType = clampCv(presetparser::getFloat(params, "voiceType", 0.0f));
    out.mdaPianoDrive = clampCv(presetparser::getFloat(params, "drive", 0.0f));
    out.mdaPianoTremWahMode = clampCv(presetparser::getFloat(params, "tremWahMode", 0.0f));
    out.mdaPianoTremWahDepth = clampCv(presetparser::getFloat(params, "tremWahDepth", 0.0f));
    out.mdaPianoTremWahRate = clampCv(presetparser::getFloat(params, "tremWahRate", 5.0f));
    out.mdaPianoChoPhaMode = clampCv(presetparser::getFloat(params, "choPhaMode", 0.0f));
    out.mdaPianoChoPhaDepth = clampCv(presetparser::getFloat(params, "choPhaDepth", 0.0f));
    out.mdaPianoChoPhaSpeed = clampCv(presetparser::getFloat(params, "choPhaSpeed", 5.0f));
    out.mdaPianoPreGain = clampCv(presetparser::getFloat(params, "preGain", 10.0f));
    out.gain = clampCv(presetparser::getFloat(params, "gain", 10.0f));
}
} // namespace nodes::mdapiano