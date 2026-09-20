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
    Gain,
    Mixer,
    Multiplier,
    Adder,
    Divide,
    FrequencyDivider,
    Abs,
    ValueMapper,
    Bitcrusher,
    WaveFolder,
    Pan,
    Chorus,
    Delay,
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
    SampleHold,
    ReverbLight,
    Hammond,
    Multiplexer,
    CurtisFilter,
    LinkedInput,
    LinkedOutput,
    FmOscillator,
    DxEnvelope,
    StepArpeggiator,
    DrumSequencer,
    OutputSwitch,
    OutputMultiplexer,
    CvIn,
    CvOut,
    DigitalIn,
    DigitalOut,
    MdaPiano,
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
    SuperSaw,
};

// Order matters: the Filter widget's "filterType" CV port maps 0-10V onto this
// list by index (filterKindFromCv in FilterNode.h), and main.js's
// FILTER_TYPE_BUTTONS must stay in the same order.
enum class FilterKind : uint8_t
{
    None,
    LowPass,
    HighPass,
    BandPass,
    Notch,
    LowShelf,
    HighShelf,
    AllPass,
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
    if (strcmp(typeStr, "bitcrusher") == 0)
        return NodeType::Bitcrusher;
    if (strcmp(typeStr, "wave-folder") == 0)
        return NodeType::WaveFolder;
    if (strcmp(typeStr, "pan") == 0)
        return NodeType::Pan;
    if (strcmp(typeStr, "chorus") == 0)
        return NodeType::Chorus;
    if (strcmp(typeStr, "delay") == 0)
        return NodeType::Delay;
    if (strcmp(typeStr, "reverb-light") == 0)
        return NodeType::ReverbLight;
    if (strcmp(typeStr, "audio-output-link") == 0)
        return NodeType::AudioOutputLink;
    if (strcmp(typeStr, "audio-input-link") == 0)
        return NodeType::AudioInputLink;
    // linked-input/linked-output: freely-placeable linkId-keyed passthrough
    // links - a separate, unrelated mechanism from the fixed 8-bus
    // audio-output-link/audio-input-link pair above.
    if (strcmp(typeStr, "linked-input") == 0 || strcmp(typeStr, "linked-input-4") == 0 ||
        strcmp(typeStr, "linked-input-8") == 0)
        return NodeType::LinkedInput;
    if (strcmp(typeStr, "linked-output") == 0 || strcmp(typeStr, "linked-output-4") == 0 ||
        strcmp(typeStr, "linked-output-8") == 0)
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
    if (strcmp(typeStr, "mda-piano") == 0)
        return NodeType::MdaPiano;
    if (strcmp(typeStr, "clock") == 0)
        return NodeType::Clock;
    if (strcmp(typeStr, "sample-hold") == 0)
        return NodeType::SampleHold;
    if (strcmp(typeStr, "hammond") == 0)
        return NodeType::Hammond;
    if (strcmp(typeStr, "multiplexer") == 0)
        return NodeType::Multiplexer;
    if (strcmp(typeStr, "curtis-filter") == 0)
        return NodeType::CurtisFilter;
    if (strcmp(typeStr, "step-arpeggiator") == 0)
        return NodeType::StepArpeggiator;
    if (strcmp(typeStr, "drum-sequencer") == 0)
        return NodeType::DrumSequencer;
    if (strcmp(typeStr, "output-switch") == 0)
        return NodeType::OutputSwitch;
    if (strcmp(typeStr, "output-multiplexer") == 0)
        return NodeType::OutputMultiplexer;
    if (strcmp(typeStr, "cv-in") == 0)
        return NodeType::CvIn;
    if (strcmp(typeStr, "cv-out") == 0)
        return NodeType::CvOut;
    if (strcmp(typeStr, "digital-in") == 0)
        return NodeType::DigitalIn;
    if (strcmp(typeStr, "digital-out") == 0)
        return NodeType::DigitalOut;
    return NodeType::Unknown;
}

inline WaveType waveTypeFromString(const char *s, bool allowSuperSaw = false)
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
    if (allowSuperSaw && strcmp(s, "supersaw") == 0)
        return WaveType::SuperSaw;
    return WaveType::Sine;
}

inline FilterKind filterKindFromString(const char *s)
{
    if (s == nullptr)
        return FilterKind::LowPass;
    if (strcmp(s, "none") == 0)
        return FilterKind::None;
    if (strcmp(s, "highpass") == 0)
        return FilterKind::HighPass;
    if (strcmp(s, "bandpass") == 0)
        return FilterKind::BandPass;
    if (strcmp(s, "notch") == 0)
        return FilterKind::Notch;
    if (strcmp(s, "lowshelf") == 0)
        return FilterKind::LowShelf;
    if (strcmp(s, "highshelf") == 0)
        return FilterKind::HighShelf;
    if (strcmp(s, "allpass") == 0)
        return FilterKind::AllPass;
    return FilterKind::LowPass;
}

// Shelving filters have no Q - the filter widgets' "resonance" control supplies
// their gain instead (0-10V -> -24..+24 dB, mirroring main.js's
// shelfGainDbFromCv). Shared by both filter node types.
inline bool isShelfKind(FilterKind kind)
{
    return kind == FilterKind::LowShelf || kind == FilterKind::HighShelf;
}

inline float shelfGainDbFromCv(float volts) { return (volts - 5.0f) * 4.8f; }

// The CEM3320 filter used to store its type and its slope in one string
// ("lp24", "lp12", "bp12", "hp12"); it now shares FilterKind plus a separate
// slope with the plain filter. These two unpack such a legacy value, and pass
// anything else through to the current representation.
inline FilterKind curtisLegacyFilterKind(const char *s)
{
    if (s == nullptr)
        return FilterKind::LowPass;
    if (strcmp(s, "lp24") == 0 || strcmp(s, "lp12") == 0)
        return FilterKind::LowPass;
    if (strcmp(s, "bp12") == 0)
        return FilterKind::BandPass;
    if (strcmp(s, "hp12") == 0)
        return FilterKind::HighPass;
    return filterKindFromString(s);
}

inline uint8_t curtisLegacySlopeDb(const char *s, int storedSlopeDb)
{
    if (s != nullptr)
    {
        if (strcmp(s, "lp24") == 0)
            return 24;
        if (strcmp(s, "lp12") == 0 || strcmp(s, "bp12") == 0 || strcmp(s, "hp12") == 0)
            return 12;
    }
    const int poles = storedSlopeDb / 6;
    return static_cast<uint8_t>((poles < 1 ? 1 : (poles > 6 ? 6 : poles)) * 6);
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
