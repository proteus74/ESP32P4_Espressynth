#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "esp_attr.h"
#include "esp_heap_caps.h"

#include "msfa_bridge.h"

#include "../CvConventions.h"
#include "../Fm8Patch.h"
#include "../NodeInstance.h"
#include "../PresetParser.h"
#include "../SynthConfig.h"

// FM8 widget: a complete DX7 voice (6 operators, 32 algorithms, pitch EG, LFO) per instance, run
// by the msfa engine (see components/msfa). One widget replaces ~14 Oscillator/Envelope widgets.
//
// The patch is the unpacked DX7 voice (Fm8Patch.h). It is shared by all voices of a widget
// (owned by voice 0); every other voice only points at it and re-applies it when its version
// changes. Ports (control-rate): 0 pitchCv, 1 gate, 2 velocity, 3 tune, 4 gain.
namespace nodes::fm8
{

using ::fm8::Patch;

struct Fm8State
{
    Patch *patch = nullptr;
    bool ownsPatch = false;
    msfa_bridge::Voice *voice = nullptr;
    uint32_t appliedVersion = 0;

    float tuneVolts = 5.0f;
    float gainCv = 10.0f;

    bool gate = false;
    bool prevGatePort = false;
    bool pendingOn = false;     // set by the MIDI route delivery (an idle voice is never rendered)
    uint8_t pendingNote = 60;
    bool idle = true;
    uint8_t note = 60;          // MIDI note as played (before tune)
    uint8_t velocity = 100;     // 1..127
    uint16_t silentBlocks = 0;
};

namespace detail
{
inline bool s_engineReady = false;

inline void ensureEngine()
{
    if (!s_engineReady)
    {
        msfa_bridge::init(static_cast<float>(synth::kSampleRate));
        s_engineReady = true;
    }
}

// Played note + Tune port (1 V per octave around 5 V), rounded to semitones for msfa's key scaling.
inline int effectiveNote(const Fm8State &s)
{
    const int semis = static_cast<int>(lroundf((s.tuneVolts - cv::kMaxVolts * 0.5f) * 12.0f));
    int n = static_cast<int>(s.note) + semis;
    return n < 0 ? 0 : (n > 127 ? 127 : n);
}
} // namespace detail

inline void gateOn(Fm8State &s, int note, int velocity127)
{
    if (s.voice == nullptr || s.patch == nullptr) return;
    s.note = static_cast<uint8_t>(note < 0 ? 0 : (note > 127 ? 127 : note));
    s.velocity = static_cast<uint8_t>(velocity127 < 1 ? 1 : (velocity127 > 127 ? 127 : velocity127));
    msfa_bridge::noteOn(s.voice, s.patch->d, detail::effectiveNote(s), s.velocity);
    s.appliedVersion = s.patch->version;
    s.gate = true;
    s.idle = false;
    s.silentBlocks = 0;
}

inline void gateOff(Fm8State &s)
{
    s.gate = false;
    if (s.voice != nullptr && !s.idle) msfa_bridge::noteOff(s.voice);
}

IRAM_ATTR inline void render(NodeInstance &inst, size_t frames)
{
    Fm8State &s = *inst.fm8;
    float *out = inst.outL;
    if (out == nullptr) return;
    if (s.voice == nullptr || s.patch == nullptr)
    {
        memset(out, 0, frames * sizeof(float));
        return;
    }

    // Note source: MIDI route (pendingOn) or a patched gate cable (edge detection).
    const bool gatePort = noderender::portSample(inst, 1, 0) >= cv::kGateThresholdVolts;
    const float velVolts = noderender::portSample(inst, 2, 0);
    const int velocity =
        static_cast<int>(lroundf(noderender::clampToRange(velVolts * cv::kInvMaxVolts, 0.0f, 1.0f) * 127.0f));
    if (s.pendingOn)
    {
        s.pendingOn = false;
        gateOn(s, s.pendingNote, velocity > 0 ? velocity : 100);
    }
    else if (gatePort != s.prevGatePort)
    {
        if (gatePort)
        {
            const float pitch = noderender::portSample(inst, 0, 0);
            gateOn(s, static_cast<int>(lroundf((pitch - cv::kMaxVolts * 0.5f) * 12.0f + 60.0f)),
                   velocity > 0 ? velocity : 100);
        }
        else
        {
            gateOff(s);
        }
    }
    s.prevGatePort = gatePort;

    if (s.idle)
    {
        memset(out, 0, frames * sizeof(float));
        return;
    }
    if (s.appliedVersion != s.patch->version)
    {
        msfa_bridge::update(s.voice, s.patch->d, detail::effectiveNote(s), s.velocity);
        s.appliedVersion = s.patch->version;
    }

    const float gain = cv::vcaGain(s.gainCv, false) * cv::kAudioPeakVolts;
    float peak = 0.0f;
    float chunk[msfa_bridge::kBlock];
    size_t done = 0;
    while (done < frames)
    {
        const size_t n = frames - done < static_cast<size_t>(msfa_bridge::kBlock)
                             ? frames - done
                             : static_cast<size_t>(msfa_bridge::kBlock);
        const float p = msfa_bridge::render(s.voice, chunk, 1, 0x2000, 2);
        if (p > peak) peak = p;
        for (size_t i = 0; i < n; ++i) out[done + i] = chunk[i] * gain;
        done += n;
    }

    if (peak < 1e-5f)
    {
        if (s.silentBlocks < 65535) ++s.silentBlocks;
    }
    else
    {
        s.silentBlocks = 0;
    }
    // Released and silent: stop rendering. A held note that has decayed to silence stops after ~1 s.
    if ((!s.gate && s.silentBlocks > 8) || s.silentBlocks > 400) s.idle = true;
}

inline void build(NodeInstance &inst, const WidgetParams &p)
{
    detail::ensureEngine();
    Fm8State &s = *inst.fm8;
    if (inst.primary == &inst)
    {
        void *raw = heap_caps_malloc(sizeof(Patch), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (raw == nullptr) raw = heap_caps_malloc(sizeof(Patch), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (raw != nullptr)
        {
            s.patch = new (raw) Patch(p.fm8Patch);
            s.ownsPatch = true;
        }
    }
    else if (inst.primary != nullptr && inst.primary->fm8 != nullptr)
    {
        s.patch = inst.primary->fm8->patch;
    }
    s.voice = msfa_bridge::create();
    s.tuneVolts = p.fm8Tune;
    s.gainCv = p.fm8Gain;
}

inline void destroy(NodeInstance &inst)
{
    if (inst.fm8 == nullptr) return;
    Fm8State &s = *inst.fm8;
    if (s.voice != nullptr)
    {
        msfa_bridge::destroy(s.voice);
        s.voice = nullptr;
    }
    if (s.ownsPatch && s.patch != nullptr) heap_caps_free(s.patch);
    heap_caps_free(inst.fm8);
    inst.fm8 = nullptr;
}

// "d0".."d154" (voice bytes), "ops" (operator switches). Returns false for other names.
inline bool setPatchKey(Patch &p, const char *name, float value)
{
    int index = -1;
    if (name[0] == 'd' && name[1] >= '0' && name[1] <= '9')
    {
        index = 0;
        for (const char *c = name + 1; *c != '\0'; ++c)
        {
            if (*c < '0' || *c > '9') return false;
            index = index * 10 + (*c - '0');
            if (index > 999) return false;
        }
    }
    else if (strcmp(name, "ops") == 0)
    {
        index = 155;
    }
    if (index < 0 || index >= ::fm8::kPatchBytes) return false;
    int v = static_cast<int>(lroundf(value));
    const int maxValue = index == 155 ? 63 : 127;
    v = v < 0 ? 0 : (v > maxValue ? maxValue : v);
    p.d[index] = static_cast<uint8_t>(v);
    ++p.version;
    return true;
}

inline bool setParam(NodeInstance &inst, const char *name, float value)
{
    if (name == nullptr || inst.fm8 == nullptr) return false;
    Fm8State &s = *inst.fm8;
    if (strcmp(name, "tune") == 0)
    {
        s.tuneVolts = value;
        return true;
    }
    if (strcmp(name, "gain") == 0)
    {
        s.gainCv = value;
        return true;
    }
    if (name[0] == 'd' || strcmp(name, "ops") == 0)
    {
        // All voices share one patch: only voice 0 writes it, the others just acknowledge.
        if (inst.primary == &inst && s.patch != nullptr) return setPatchKey(*s.patch, name, value);
        return true;
    }
    return false;
}

// Preset: flat numeric keys d0..d154 / ops / tune / gain. Absent keys keep the INIT VOICE values.
inline void parsePreset(cJSON *p, WidgetParams &out)
{
    out.fm8Tune = presetparser::getFloat(p, "tune", 5.0f);
    out.fm8Gain = presetparser::getFloat(p, "gain", 10.0f);
    if (p == nullptr) return;
    for (cJSON *item = p->child; item != nullptr; item = item->next)
    {
        if (item->string != nullptr && cJSON_IsNumber(item))
        {
            setPatchKey(out.fm8Patch, item->string, static_cast<float>(item->valuedouble));
        }
    }
}

} // namespace nodes::fm8
