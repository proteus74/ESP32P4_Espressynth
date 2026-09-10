#pragma once

#include <cmath>
#include <cstddef>
#include <cstring>

#include "esp_ae_alc.h"
#include "esp_ae_ch_cvt.h"
#include "esp_ae_drc.h"
#include "esp_ae_mbc.h"
#include "esp_ae_mixer.h"
#include "esp_ae_sonic.h"
#include "esp_heap_caps.h"

#include "CvConventions.h"
#include "EspAudioEffectsAdapter.h"
#include "NodeState.h"
#include "PresetParser.h"
#include "SynthConfig.h"

enum class EspEffectKind : unsigned char
{
    Alc,
    ChannelConvert,
    Mixer,
    Sonic,
    Drc,
    Mbc,
};

namespace
{
float espEffectClampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
uint16_t espEffectU16(float v, int hi) { return static_cast<uint16_t>(espEffectClampf(v, 0.0f, static_cast<float>(hi))); }
void espEffectCopyOrZero(const float *src, float *dst, size_t n)
{
    if (src) memcpy(dst, src, n * sizeof(float)); else memset(dst, 0, n * sizeof(float));
}
void espEffectBypass(float *const *in, size_t count, float *l, float *r, size_t n)
{
    espEffectCopyOrZero(count > 0 ? in[0] : nullptr, l, n);
    espEffectCopyOrZero(count > 1 ? in[1] : (count > 0 ? in[0] : nullptr), r, n);
}
bool espEffectAllocExtra(EspEffectState &s, int count)
{
    // PSRAM-preferred, internal fallback: touched at most once per block (not per-sample) by
    // the ESP_AUDIO_EFFECTS calls in applyEspEffect() below, so PSRAM latency doesn't threaten
    // the render budget the way a per-sample-accessed buffer would - same reasoning already
    // applied to every other allocation in this engine (see GraphBuilder.h's allocateNodeState/
    // allocateBlockBuffer), this one had just been left internal-only by oversight.
    const size_t bytes = synth::kBlockSize * 2 * sizeof(int32_t);
    for (int i = 0; i < count; ++i)
    {
        s.extra[i] = static_cast<int32_t *>(heap_caps_aligned_calloc(16, 1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (s.extra[i] == nullptr)
        {
            s.extra[i] = static_cast<int32_t *>(heap_caps_aligned_calloc(16, 1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
        if (!s.extra[i]) return false;
    }
    return true;
}
}

inline void destroyEspEffect(EspEffectState &s);

inline bool initEspEffect(EspEffectState &s, EspEffectKind kind, const float *p, size_t count)
{
    destroyEspEffect(s);
    s.kind = static_cast<uint8_t>(kind);
    for (size_t i = 0; i < count && i < 16; ++i) s.params[i] = p[i];
    if (!espae::allocateScratch(s.scratch, 2, synth::kBlockSize)) return false;
    esp_ae_err_t err = ESP_AE_ERR_FAIL;
    switch (kind)
    {
    case EspEffectKind::Alc:
    {
        esp_ae_alc_cfg_t cfg = {synth::kSampleRate, 2, ESP_AE_BIT32};
        esp_ae_alc_handle_t h = nullptr; err = esp_ae_alc_open(&cfg, &h); s.handle = h;
        if (err == ESP_AE_ERR_OK) { esp_ae_alc_set_gain(h, 0, static_cast<int8_t>(espEffectClampf(s.params[0], -64, 63))); esp_ae_alc_set_gain(h, 1, static_cast<int8_t>(espEffectClampf(s.params[1], -64, 63))); }
        break;
    }
    case EspEffectKind::ChannelConvert:
    {
        float weights[2] = {s.params[0], s.params[1]};
        esp_ae_ch_cvt_cfg_t cfg = {synth::kSampleRate, ESP_AE_BIT32, 2, 1, weights, 2};
        esp_ae_ch_cvt_handle_t h = nullptr; err = esp_ae_ch_cvt_open(&cfg, &h); s.handle = h;
        break;
    }
    case EspEffectKind::Mixer:
    {
        s.inputCount = 2;
        esp_ae_mixer_info_t info[2] = {{s.params[0], s.params[1], static_cast<uint32_t>(espEffectClampf(s.params[2], 1, 10000))}, {s.params[1], s.params[0], static_cast<uint32_t>(espEffectClampf(s.params[2], 1, 10000))}};
        esp_ae_mixer_cfg_t cfg = {synth::kSampleRate, 2, ESP_AE_BIT32, 2, info};
        esp_ae_mixer_handle_t h = nullptr; err = esp_ae_mixer_open(&cfg, &h); s.handle = h;
        if (err == ESP_AE_ERR_OK && s.params[3] >= cv::kGateThresholdVolts) { esp_ae_mixer_set_mode(h, 0, ESP_AE_MIXER_MODE_FADE_UPWARD); esp_ae_mixer_set_mode(h, 1, ESP_AE_MIXER_MODE_FADE_UPWARD); }
        if (!espEffectAllocExtra(s, 4)) err = ESP_AE_ERR_MEM_LACK;
        break;
    }
    case EspEffectKind::Sonic:
    {
        esp_ae_sonic_cfg_t cfg = {synth::kSampleRate, 2, ESP_AE_BIT32};
        esp_ae_sonic_handle_t h = nullptr; err = esp_ae_sonic_open(&cfg, &h); s.handle = h;
        if (err == ESP_AE_ERR_OK) { esp_ae_sonic_set_speed(h, espEffectClampf(s.params[0], .5f, 2)); esp_ae_sonic_set_pitch(h, espEffectClampf(s.params[1], .5f, 2)); }
        if (!espEffectAllocExtra(s, 2)) err = ESP_AE_ERR_MEM_LACK;
        break;
    }
    case EspEffectKind::Drc:
    {
        const float t = espEffectClampf(s.params[0], -99, -.1f), ratio = espEffectClampf(s.params[1], 1, 100);
        esp_ae_drc_curve_point pts[3] = {{0, t + (0 - t) / ratio}, {t, t}, {-100, -100}};
        esp_ae_drc_cfg_t cfg = {}; cfg.sample_rate = synth::kSampleRate; cfg.channel = 2; cfg.bits_per_sample = ESP_AE_BIT32;
        cfg.drc_para = {pts, 3, espEffectClampf(s.params[4], -10, 10), espEffectClampf(s.params[5], 0, 10), espEffectU16(s.params[2], 500), espEffectU16(s.params[3], 500), 0};
        esp_ae_drc_handle_t h = nullptr; err = esp_ae_drc_open(&cfg, &h); s.handle = h;
        break;
    }
    case EspEffectKind::Mbc:
    {
        esp_ae_mbc_config_t cfg = {}; cfg.sample_rate = synth::kSampleRate; cfg.channel = 2; cfg.bits_per_sample = ESP_AE_BIT32;
        cfg.fc[0] = 120; cfg.fc[1] = 2000; cfg.fc[2] = 10000;
        for (auto &b : cfg.mbc_para) b = {espEffectClampf(s.params[0], -99, -.1f), espEffectClampf(s.params[1], 1, 100), espEffectClampf(s.params[2], -10, 10), 5, 100, 0, 2};
        esp_ae_mbc_handle_t h = nullptr; err = esp_ae_mbc_open(&cfg, &h); s.handle = h;
        break;
    }
    }
    s.ready = err == ESP_AE_ERR_OK && s.handle != nullptr;
    if (!s.ready) destroyEspEffect(s);
    return s.ready;
}

inline void processEspEffect(EspEffectState &s, float *const *in, size_t count, float *l, float *r, size_t n)
{
    if (s.bypass || !s.ready || n > synth::kBlockSize) { espEffectBypass(in, count, l, r, n); return; }
    auto ch = s.scratch.channels;
    if (static_cast<EspEffectKind>(s.kind) == EspEffectKind::Mixer)
    {
        for (int src = 0; src < 2; ++src) for (int c = 0; c < 2; ++c) espae::floatToS32(count > static_cast<size_t>(src * 2 + c) ? in[src * 2 + c] : nullptr, s.extra[src * 2 + c], n);
        esp_ae_sample_t a[2] = {s.extra[0], s.extra[1]}, b[2] = {s.extra[2], s.extra[3]}; esp_ae_sample_t *inputs[2] = {a, b}; esp_ae_sample_t out[2] = {ch[0], ch[1]};
        if (esp_ae_mixer_deintlv_process(s.handle, n, inputs, out) != ESP_AE_ERR_OK) { espEffectBypass(in, count, l, r, n); return; }
    }
    else if (static_cast<EspEffectKind>(s.kind) == EspEffectKind::Sonic)
    {
        for (size_t i = 0; i < n; ++i) { float lv = count > 0 && in[0] ? in[0][i] : 0, rv = count > 1 && in[1] ? in[1][i] : lv; espae::floatToS32(&lv, &s.extra[0][2*i], 1); espae::floatToS32(&rv, &s.extra[0][2*i+1], 1); }
        esp_ae_sonic_in_data_t si = {s.extra[0], static_cast<uint32_t>(n), 0}; esp_ae_sonic_out_data_t so = {s.extra[1], static_cast<uint32_t>(n), 0};
        if (esp_ae_sonic_process(s.handle, &si, &so) != ESP_AE_ERR_OK) { espEffectBypass(in, count, l, r, n); return; }
        memset(l, 0, n*sizeof(float)); memset(r, 0, n*sizeof(float));
        for (size_t i = 0; i < so.out_num; ++i) { espae::s32ToFloat(&s.extra[1][2*i], &l[i], 1); espae::s32ToFloat(&s.extra[1][2*i+1], &r[i], 1); }
        return;
    }
    else
    {
        espae::floatToS32(count > 0 ? in[0] : nullptr, ch[0], n); espae::floatToS32(count > 1 ? in[1] : (count ? in[0] : nullptr), ch[1], n); esp_ae_sample_t io[2] = {ch[0], ch[1]}; esp_ae_err_t e = ESP_AE_ERR_FAIL;
        switch (static_cast<EspEffectKind>(s.kind)) {
        case EspEffectKind::Alc: e = esp_ae_alc_deintlv_process(s.handle,n,io,io); break;
        case EspEffectKind::ChannelConvert: { esp_ae_sample_t mono[1] = {ch[0]}; e = esp_ae_ch_cvt_deintlv_process(s.handle,n,io,mono); if(e==ESP_AE_ERR_OK) memcpy(ch[1],ch[0],n*sizeof(int32_t)); break; }
        case EspEffectKind::Drc: e = esp_ae_drc_deintlv_process(s.handle,n,io,io); break;
        case EspEffectKind::Mbc: e = esp_ae_mbc_deintlv_process(s.handle,n,io,io); break;
        default: break; }
        if (e != ESP_AE_ERR_OK) { espEffectBypass(in,count,l,r,n); return; }
    }
    espae::s32ToFloat(ch[0],l,n); espae::s32ToFloat(ch[1],r,n);
}

inline bool updateEspEffect(EspEffectState &s, const char *name, float v)
{
    if (!strcmp(name, "bypass")) { s.bypass = v >= 0.5f; return true; }
    if (!s.ready) return false;
    switch(static_cast<EspEffectKind>(s.kind)) {
    case EspEffectKind::Alc: if(!strcmp(name,"gainL")){s.params[0]=v;return esp_ae_alc_set_gain(s.handle,0,static_cast<int8_t>(espEffectClampf(v,-64,63)))==0;} if(!strcmp(name,"gainR")){s.params[1]=v;return esp_ae_alc_set_gain(s.handle,1,static_cast<int8_t>(espEffectClampf(v,-64,63)))==0;} break;
    case EspEffectKind::Mixer: if(!strcmp(name,"mode")){s.params[3]=v; auto m=v>=cv::kGateThresholdVolts?ESP_AE_MIXER_MODE_FADE_UPWARD:ESP_AE_MIXER_MODE_FADE_DOWNWARD; return esp_ae_mixer_set_mode(s.handle,0,m)==0 && esp_ae_mixer_set_mode(s.handle,1,m)==0;} break;
    case EspEffectKind::Sonic: if(!strcmp(name,"speed")){s.params[0]=v;return esp_ae_sonic_set_speed(s.handle,espEffectClampf(v,.5f,2))==0;} if(!strcmp(name,"pitch")){s.params[1]=v;return esp_ae_sonic_set_pitch(s.handle,espEffectClampf(v,.5f,2))==0;} break;
    case EspEffectKind::Drc: if(!strcmp(name,"attackMs"))return esp_ae_drc_set_attack_time(s.handle,espEffectU16(v,500))==0; if(!strcmp(name,"releaseMs"))return esp_ae_drc_set_release_time(s.handle,espEffectU16(v,500))==0; if(!strcmp(name,"makeupDb"))return esp_ae_drc_set_makeup_gain(s.handle,espEffectClampf(v,-10,10))==0; break;
    default: break; }
    return false;
}

inline void destroyEspEffect(EspEffectState &s)
{
    if (s.handle) { switch(static_cast<EspEffectKind>(s.kind)) { case EspEffectKind::Alc: esp_ae_alc_close(s.handle); break; case EspEffectKind::ChannelConvert: esp_ae_ch_cvt_close(s.handle); break; case EspEffectKind::Mixer: esp_ae_mixer_close(s.handle); break; case EspEffectKind::Sonic: esp_ae_sonic_close(s.handle); break; case EspEffectKind::Drc: esp_ae_drc_close(s.handle); break; case EspEffectKind::Mbc: esp_ae_mbc_close(s.handle); break; } }
    s.handle=nullptr; espae::freeScratch(s.scratch); for(auto &p:s.extra){heap_caps_free(p);p=nullptr;} s.ready=false;
}

inline void parsePresetEspAlc(cJSON *p, WidgetParams &out)
{
    out.espParam[0] = presetparser::getFloat(p, "gainL", 0.0f);
    out.espParam[1] = presetparser::getFloat(p, "gainR", 0.0f);
    out.espBypass = presetparser::getBool(p, "bypass", false);
}

inline void parsePresetEspChannelConvert(cJSON *p, WidgetParams &out)
{
    out.espParam[0] = presetparser::getFloat(p, "weightL", 0.70710678f);
    out.espParam[1] = presetparser::getFloat(p, "weightR", 0.70710678f);
    out.espBypass = presetparser::getBool(p, "bypass", false);
}

inline void parsePresetEspMixer(cJSON *p, WidgetParams &out)
{
    out.espParam[0] = presetparser::getFloat(p, "weight1", 1.0f);
    out.espParam[1] = presetparser::getFloat(p, "weight2", 0.0f);
    out.espParam[2] = presetparser::getFloat(p, "fadeMs", 100.0f);
    out.espParam[3] = presetparser::getFloat(p, "mode", 0.0f);
}

inline void parsePresetEspSonic(cJSON *p, WidgetParams &out)
{
    out.espParam[0] = presetparser::getFloat(p, "speed", 1.0f);
    out.espParam[1] = presetparser::getFloat(p, "pitch", 1.0f);
    out.espBypass = presetparser::getBool(p, "bypass", false);
}

inline void parsePresetEspDrc(cJSON *p, WidgetParams &out)
{
    out.espParam[0] = presetparser::getFloat(p, "threshold", -20.0f);
    out.espParam[1] = presetparser::getFloat(p, "ratio", 4.0f);
    out.espParam[2] = presetparser::getFloat(p, "attackMs", 10.0f);
    out.espParam[3] = presetparser::getFloat(p, "releaseMs", 100.0f);
    out.espParam[4] = presetparser::getFloat(p, "makeupDb", 0.0f);
    out.espParam[5] = presetparser::getFloat(p, "kneeDb", 2.0f);
    out.espBypass = presetparser::getBool(p, "bypass", false);
}

inline void parsePresetEspMbc(cJSON *p, WidgetParams &out)
{
    out.espParam[0] = presetparser::getFloat(p, "threshold", -22.0f);
    out.espParam[1] = presetparser::getFloat(p, "ratio", 2.0f);
    out.espParam[2] = presetparser::getFloat(p, "makeupDb", 0.0f);
    out.espBypass = presetparser::getBool(p, "bypass", false);
}
