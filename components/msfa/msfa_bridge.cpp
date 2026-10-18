// Bridge between the application and the msfa DX7 core. See msfa_bridge.h.
#include "msfa_bridge.h"

#include <cmath>
#include <cstring>
#include <new>

#include "controllers.h"
#include "dx7note.h"
#include "env.h"
#include "exp2.h"
#include "fm_core.h"
#include "freqlut.h"
#include "lfo.h"
#include "pitchenv.h"
#include "sin.h"

namespace msfa_bridge
{
namespace
{
bool s_initialized = false;
FmCore s_core; // stateless apart from scratch buffers used within one render() call
} // namespace

struct Voice
{
    Dx7Note note;
    Lfo lfo;
    Controllers controllers;
    bool keyDown = false;
};

void init(float sampleRate)
{
    if (s_initialized) return;
    Exp2::init();
    Sin::init();
    Freqlut::init(sampleRate);
    Lfo::init(sampleRate);
    PitchEnv::init(sampleRate);
    Env::init_sr(sampleRate);
    s_initialized = true;
}

Voice *create()
{
    void *raw = ::operator new(sizeof(Voice), std::nothrow);
    if (raw == nullptr) return nullptr;
    Voice *v = new (raw) Voice();
    v->controllers.core = &s_core;
    v->controllers.opSwitch = 0x3f;
    v->controllers.values_[kControllerPitch] = 0x2000;
    v->controllers.values_[kControllerPitchRange] = 2;
    v->controllers.values_[kControllerPitchStep] = 0;
    v->controllers.masterTune = 0;
    v->controllers.amp_mod = 0;
    v->controllers.pitch_mod = 0;
    v->controllers.eg_mod = 127;
    return v;
}

void destroy(Voice *voice)
{
    if (voice == nullptr) return;
    voice->~Voice();
    ::operator delete(voice);
}

void noteOn(Voice *voice, const uint8_t patch[kPatchBytes], int midiNote, int velocity)
{
    if (voice == nullptr) return;
    voice->note.init(patch, midiNote, velocity < 1 ? 1 : (velocity > 127 ? 127 : velocity));
    voice->lfo.reset(patch + 137);
    voice->lfo.keydown();
    voice->controllers.opSwitch = patch[155] != 0 ? (patch[155] & 0x3f) : 0x3f;
    voice->keyDown = true;
}

void noteOff(Voice *voice)
{
    if (voice == nullptr || !voice->keyDown) return;
    voice->note.keyup();
    voice->keyDown = false;
}

void update(Voice *voice, const uint8_t patch[kPatchBytes], int midiNote, int velocity)
{
    if (voice == nullptr) return;
    voice->note.update(patch, midiNote, velocity < 1 ? 1 : (velocity > 127 ? 127 : velocity));
    voice->lfo.reset(patch + 137);
    voice->controllers.opSwitch = patch[155] != 0 ? (patch[155] & 0x3f) : 0x3f;
}

float render(Voice *voice, float *out, int blocks, int pitchBend14, int pitchRangeSemis, float gain)
{
    if (voice == nullptr) return 0.0f;
    voice->controllers.values_[kControllerPitch] = pitchBend14;
    voice->controllers.values_[kControllerPitchRange] = pitchRangeSemis;
    float peak = 0.0f;
    alignas(16) int32_t buf[kBlock];
    const float scale = gain * (1.0f / 32768.0f);
    for (int b = 0; b < blocks; ++b)
    {
        std::memset(buf, 0, sizeof(buf));
        const int32_t lfoValue = voice->lfo.getsample();
        const int32_t lfoDelay = voice->lfo.getdelay();
        voice->note.compute(buf, lfoValue, lfoDelay, &voice->controllers);
        float *dst = out + b * kBlock;
        // Peak on the integer domain (one compare per sample, no fabsf), scaled once at the end.
        int32_t peakInt = 0;
        for (int i = 0; i < kBlock; ++i)
        {
            // msfa output is ~24 bit; Dexed scales it to 16 bit with >> 13 and saturates.
            int32_t v = buf[i] >> 13;
            v = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
            const int32_t a = v < 0 ? -v : v;
            if (a > peakInt) peakInt = a;
            dst[i] = static_cast<float>(v) * scale;
        }
        const float blockPeak = static_cast<float>(peakInt) * (1.0f / 32768.0f);
        if (blockPeak > peak) peak = blockPeak;
    }
    return peak;
}
} // namespace msfa_bridge
