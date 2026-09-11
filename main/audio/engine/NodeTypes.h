#pragma once

#include <cstdint>
#include <cstring>

// Mirrors the node types built by data/static/js/audio-engine.js's
// _buildNode() switch. MidiIn is not a DSP-rendered node (it has no audio
// ports, see PortTables) - it only exists as a graph participant so the
// voiced/global BFS has a starting point and ControlRoutes can reference it.
enum class NodeType : uint8_t
{
    Oscillator = 0,
    Lfo,
    Detune,
    Adsr,
    Filter,
    EspEq,
    EspAlc,
    EspChannelConvert,
    EspMixer,
    EspSonic,
    EspDrc,
    EspMbc,
    Gain,
    Mixer,
    Multiplier,
    Adder,
    Divide,
    FrequencyDivider,
    Abs,
    ValueMapper,
    Chorus,
    Delay,
    Reverb,
    AudioOutputLink,
    AudioInputLink,
    AudioOut,
    UsbOut,
    Es8311Out,
    Wm8960Out,
    VuMeter,
    Oscilloscope,
    Spectrum,
    MidiIn,
    Const,
    Knob,
    Button,
    UpDownButton,
    Switch,
    WavePlayer,
    SoundFontPlayer,
    MidiSplit,
    Clock,
    ReverbLight,
    Hammond,
    Multiplexer,
    CurtisFilter,
    LinkedInput,
    LinkedOutput,
    FmOscillator,
    DxEnvelope,
    Unknown,
};

enum class WaveType : uint8_t
{
    Sine,
    Square,
    Triangle,
    Sawtooth,
    Noise,
    PinkNoise,
};

enum class FilterKind : uint8_t
{
    LowPass,
    HighPass,
    BandPass,
};

enum class CurtisFilterMode : uint8_t
{
    LowPass24,
    LowPass12,
    BandPass12,
    HighPass12,
};

// Mirrors esp_ae_eq_filter_type_t (managed_components/espressif__esp_audio_effects/
// include/esp_ae_eq.h) without leaking that header into NodeState.h - EspEqEffect.cpp
// is the only translation unit that maps this onto the hardware enum.
enum class EqFilterKind : uint8_t
{
    HighPass,
    LowPass,
    Peak,
    HighShelf,
    LowShelf,
};

enum class OverflowMode : uint8_t
{
    StealOldest,
    Ignore,
};

// Maps a preset widget's "type" string (e.g. "oscillator", "delay-stereo")
// onto a NodeType. Returns NodeType::Unknown for anything not recognized
// (matches _buildNode()'s default case returning null).
inline NodeType nodeTypeFromString(const char *typeStr)
{
    if (typeStr == nullptr)
    {
        return NodeType::Unknown;
    }
    if (strcmp(typeStr, "oscillator") == 0)
        return NodeType::Oscillator;
    if (strcmp(typeStr, "fm-oscillator") == 0)
        return NodeType::FmOscillator;
    if (strcmp(typeStr, "lfo") == 0)
        return NodeType::Lfo;
    if (strcmp(typeStr, "detune") == 0)
        return NodeType::Detune;
    if (strcmp(typeStr, "adsr") == 0)
        return NodeType::Adsr;
    if (strcmp(typeStr, "dx-envelope") == 0)
        return NodeType::DxEnvelope;
    if (strcmp(typeStr, "filter") == 0)
        return NodeType::Filter;
    if (strcmp(typeStr, "esp-eq") == 0)
        return NodeType::EspEq;
    if (strcmp(typeStr, "esp-alc") == 0)
        return NodeType::EspAlc;
    if (strcmp(typeStr, "esp-channel-convert") == 0)
        return NodeType::EspChannelConvert;
    if (strcmp(typeStr, "esp-mixer") == 0)
        return NodeType::EspMixer;
    if (strcmp(typeStr, "esp-sonic") == 0)
        return NodeType::EspSonic;
    if (strcmp(typeStr, "esp-drc") == 0)
        return NodeType::EspDrc;
    if (strcmp(typeStr, "esp-mbc") == 0)
        return NodeType::EspMbc;
    if (strcmp(typeStr, "gain") == 0)
        return NodeType::Gain;
    if (strcmp(typeStr, "mixer") == 0)
        return NodeType::Mixer;
    if (strcmp(typeStr, "multiplier") == 0)
        return NodeType::Multiplier;
    if (strcmp(typeStr, "adder") == 0)
        return NodeType::Adder;
    if (strcmp(typeStr, "divide") == 0)
        return NodeType::Divide;
    if (strcmp(typeStr, "frequency-divider") == 0)
        return NodeType::FrequencyDivider;
    if (strcmp(typeStr, "abs") == 0)
        return NodeType::Abs;
    if (strcmp(typeStr, "value-mapper") == 0)
        return NodeType::ValueMapper;
    if (strcmp(typeStr, "chorus") == 0)
        return NodeType::Chorus;
    if (strcmp(typeStr, "delay") == 0)
        return NodeType::Delay;
    if (strcmp(typeStr, "reverb") == 0)
        return NodeType::Reverb;
    if (strcmp(typeStr, "reverb-light") == 0)
        return NodeType::ReverbLight;
    if (strcmp(typeStr, "audio-output-link") == 0)
        return NodeType::AudioOutputLink;
    if (strcmp(typeStr, "audio-input-link") == 0)
        return NodeType::AudioInputLink;
    // linked-input/linked-output: freely-placeable linkId-keyed passthrough
    // links - a separate, unrelated mechanism from the fixed 8-bus
    // audio-output-link/audio-input-link pair above.
    if (strcmp(typeStr, "linked-input") == 0)
        return NodeType::LinkedInput;
    if (strcmp(typeStr, "linked-output") == 0)
        return NodeType::LinkedOutput;
    if (strcmp(typeStr, "audio-out") == 0)
        return NodeType::AudioOut;
    if (strcmp(typeStr, "usb-out") == 0)
        return NodeType::UsbOut;
    if (strcmp(typeStr, "es8311-out") == 0)
        return NodeType::Es8311Out;
    if (strcmp(typeStr, "wm8960-out") == 0)
        return NodeType::Wm8960Out;
    if (strcmp(typeStr, "vu-meter") == 0 || strcmp(typeStr, "vu-meter-horizontal") == 0)
        return NodeType::VuMeter;
    if (strcmp(typeStr, "oscilloscope") == 0)
        return NodeType::Oscilloscope;
    if (strcmp(typeStr, "spectrum") == 0)
        return NodeType::Spectrum;
    if (strcmp(typeStr, "midi-in") == 0)
        return NodeType::MidiIn;
    if (strcmp(typeStr, "midi-split") == 0)
        return NodeType::MidiSplit;
    if (strcmp(typeStr, "const") == 0)
        return NodeType::Const;
    if (strcmp(typeStr, "knob") == 0)
        return NodeType::Knob;
    if (strcmp(typeStr, "button") == 0)
        return NodeType::Button;
    if (strcmp(typeStr, "up-down-button") == 0)
        return NodeType::UpDownButton;
    if (strcmp(typeStr, "switch") == 0)
        return NodeType::Switch;
    if (strcmp(typeStr, "wave-player") == 0)
        return NodeType::WavePlayer;
    if (strcmp(typeStr, "soundfont-player") == 0)
        return NodeType::SoundFontPlayer;
    if (strcmp(typeStr, "clock") == 0)
        return NodeType::Clock;
    if (strcmp(typeStr, "hammond") == 0)
        return NodeType::Hammond;
    if (strcmp(typeStr, "multiplexer") == 0)
        return NodeType::Multiplexer;
    if (strcmp(typeStr, "curtis-filter") == 0)
        return NodeType::CurtisFilter;
    return NodeType::Unknown;
}

inline WaveType waveTypeFromString(const char *s)
{
    if (s == nullptr)
        return WaveType::Sine;
    if (strcmp(s, "square") == 0)
        return WaveType::Square;
    if (strcmp(s, "triangle") == 0)
        return WaveType::Triangle;
    if (strcmp(s, "sawtooth") == 0)
        return WaveType::Sawtooth;
    if (strcmp(s, "noise") == 0)
        return WaveType::Noise;
    if (strcmp(s, "pink-noise") == 0)
        return WaveType::PinkNoise;
    return WaveType::Sine;
}

inline FilterKind filterKindFromString(const char *s)
{
    if (s == nullptr)
        return FilterKind::LowPass;
    if (strcmp(s, "highpass") == 0)
        return FilterKind::HighPass;
    if (strcmp(s, "bandpass") == 0)
        return FilterKind::BandPass;
    return FilterKind::LowPass;
}

inline CurtisFilterMode curtisFilterModeFromString(const char *s)
{
    if (s == nullptr)
        return CurtisFilterMode::LowPass24;
    if (strcmp(s, "lp12") == 0)
        return CurtisFilterMode::LowPass12;
    if (strcmp(s, "bp12") == 0 || strcmp(s, "bandpass") == 0)
        return CurtisFilterMode::BandPass12;
    if (strcmp(s, "hp12") == 0 || strcmp(s, "highpass") == 0)
        return CurtisFilterMode::HighPass12;
    return CurtisFilterMode::LowPass24;
}

inline EqFilterKind eqFilterKindFromString(const char *s)
{
    if (s == nullptr)
        return EqFilterKind::Peak;
    if (strcmp(s, "highpass") == 0)
        return EqFilterKind::HighPass;
    if (strcmp(s, "lowpass") == 0)
        return EqFilterKind::LowPass;
    if (strcmp(s, "highshelf") == 0)
        return EqFilterKind::HighShelf;
    if (strcmp(s, "lowshelf") == 0)
        return EqFilterKind::LowShelf;
    return EqFilterKind::Peak;
}

inline OverflowMode overflowModeFromString(const char *s)
{
    if (s != nullptr && strcmp(s, "ignore") == 0)
    {
        return OverflowMode::Ignore;
    }
    return OverflowMode::StealOldest;
}
