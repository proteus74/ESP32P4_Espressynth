#pragma once

#include <cstring>
#include "audio/engine/NodeTypes.h"

namespace portlabels {
struct Entry { const char *id; const char *label; };

inline const char *display(NodeType type, const char *id)
{
    if (id == nullptr || id[0] == '\0') return "--";
    if (strcmp(id, "midi") == 0 && type == NodeType::MidiFilter) return "MIDI-In";
    if ((strcmp(id, "in1") == 0 || strcmp(id, "in2") == 0) &&
        (type == NodeType::AudioOutputLink || type == NodeType::LinkedInput)) return id + 2;
    if (strcmp(id, "gain") == 0) {
        if (type == NodeType::AudioOut || type == NodeType::UsbOut || type == NodeType::Es8311Out ||
            type == NodeType::Wm8960Out) return "Volume";
        if (type == NodeType::CombFilter) return "Output Gain";
    }
    if (strcmp(id, "feedback") == 0) {
        if (type == NodeType::Oscillator) return "Feedback (Self FB)";
        if (type == NodeType::FmOscillator) return "Feedback (Self)";
    }
    if (strcmp(id, "enableModWheel") == 0 && type == NodeType::SoundFontPlayer)
        return "Enable Mod Wheel (Ch 1, CC 1)";
    if (strcmp(id, "preGain") == 0 && type == NodeType::EnvelopeFollower) return "PreGain";
    if (strcmp(id, "amount") == 0 && type == NodeType::Gain) return "Output Gain";
    if (strcmp(id, "time") == 0 && type == NodeType::CombFilter) return "Delay";
    if (strcmp(id, "rate") == 0 &&
        (type == NodeType::StepArpeggiator || type == NodeType::DrumSequencer)) return "BPM";
    // Port IDs and default labels mirror WIDGET_TYPES inputs in data/static/js/main.js.
    static constexpr Entry labels[] = {
        {"amount", "Amount"},
        {"amplitude", "Amplitude"},
        {"att", "Attack"},
        {"attack", "Attack"},
        {"band1", "63 Hz"},
        {"band2", "125 Hz"},
        {"band3", "250 Hz"},
        {"band4", "500 Hz"},
        {"band5", "1 kHz"},
        {"band6", "2 kHz"},
        {"band7", "5 kHz"},
        {"band8", "14 kHz"},
        {"bank", "Bank"},
        {"bypass", "Bypass"},
        {"choPhaDepth", "Cho/Pha Depth"},
        {"choPhaMode", "Cho/Pha Mode"},
        {"choPhaSpeed", "Cho/Pha Speed"},
        {"clock", "Trigger"},
        {"count", "Count"},
        {"crush", "Crush"},
        {"curve", "Curve"},
        {"cutoff", "Cutoff"},
        {"cv", "In"},
        {"damping", "Damping"},
        {"dec", "Decay"},
        {"decay", "Decay"},
        {"depth", "Depth"},
        {"direction", "Direction"},
        {"divider", "Divider"},
        {"drawbar1", "16′"},
        {"drawbar2", "5⅓′"},
        {"drawbar3", "8′"},
        {"drawbar4", "4′"},
        {"drawbar5", "2⅔′"},
        {"drawbar6", "2′"},
        {"drawbar7", "1⅗′"},
        {"drawbar8", "1⅓′"},
        {"drawbar9", "1′"},
        {"drive", "Drive"},
        {"dryLevel", "Dry Level"},
        {"dutyCycle", "Duty Cycle"},
        {"enableCc", "Enable CC Messages"},
        {"enableModWheel", "Enable Mod Wheel (Ch 1, CC 1)"},
        {"enablePitchWheel", "Enable Pitch Wheel"},
        {"feedback", "Feedback"},
        {"filterType", "Filter Type"},
        {"fmIn", "FM In"},
        {"fmIndex", "FM Ratio"},
        {"frequency", "Frequency"},
        {"gain", "Gain"},
        {"gain1", "1"},
        {"gain2", "2"},
        {"gain3", "3"},
        {"gain4", "4"},
        {"gain5", "5"},
        {"gain6", "6"},
        {"gain7", "7"},
        {"gain8", "8"},
        {"gainDb", "Gain dB"},
        {"gate", "Gate"},
        {"glide", "Glide"},
        {"glideTime", "Glide Time"},
        {"global", "Global"},
        {"in", "In"},
        {"in1", "In 1"},
        {"in2", "In 2"},
        {"in3", "3"},
        {"in4", "4"},
        {"in5", "5"},
        {"in6", "6"},
        {"in7", "7"},
        {"in8", "8"},
        {"inL", "In L"},
        {"inR", "In R"},
        {"layer", "Layer"},
        {"left", "In L"},
        {"level1", "Level 1"},
        {"level2", "Level 2"},
        {"level3", "Level 3"},
        {"level4", "Level 4"},
        {"midi", "MIDI"},
        {"mix", "Mix"},
        {"mode", "Mode"},
        {"morph", "Morph"},
        {"muted", "Mute (Gate)"},
        {"note", "Note (0–127)"},
        {"offset", "Offset"},
        {"oneShot", "One Shot"},
        {"pan", "Pan"},
        {"pattern", "Pattern"},
        {"phase", "Phase"},
        {"pitch", "Pitch"},
        {"pitchCv", "Pitch"},
        {"polarity", "Feedback ±"},
        {"preGain", "Pre Gain"},
        {"program", "Program"},
        {"pulseTime", "Gate-Out-Time"},
        {"q", "Q"},
        {"rate", "Rate"},
        {"rel", "Release"},
        {"release", "Release"},
        {"reset", "Reset"},
        {"resonance", "Resonance"},
        {"right", "In R"},
        {"roomSize", "Room Size"},
        {"select", "Select"},
        {"slope", "Slope"},
        {"step1", "Step 1"},
        {"step10", "Step 10"},
        {"step11", "Step 11"},
        {"step12", "Step 12"},
        {"step13", "Step 13"},
        {"step14", "Step 14"},
        {"step15", "Step 15"},
        {"step16", "Step 16"},
        {"step2", "Step 2"},
        {"step3", "Step 3"},
        {"step4", "Step 4"},
        {"step5", "Step 5"},
        {"step6", "Step 6"},
        {"step7", "Step 7"},
        {"step8", "Step 8"},
        {"step9", "Step 9"},
        {"superSawDetune", "SuperSaw Detune"},
        {"superSawMix", "SuperSaw Mix"},
        {"sus", "Sustain"},
        {"switchAtEnd", "Switch at End"},
        {"symmetry", "Symmetry"},
        {"threshold", "Threshold"},
        {"time", "Time"},
        {"time1", "Time 1"},
        {"time2", "Time 2"},
        {"time3", "Time 3"},
        {"time4", "Time 4"},
        {"tremWahDepth", "Trem/Wah Depth"},
        {"tremWahMode", "Trem/Wah Mode"},
        {"tremWahRate", "Trem/Wah Rate"},
        {"trigger", "Trigger"},
        {"tune", "Tune"},
        {"velocity", "Velocity"},
        {"voices", "Voices"},
        {"voiceType", "Voice Type"},
        {"waveform", "Waveform"},
        {"wetLevel", "Wet Level"},
        {"zeroPoint", "Offset"},
    };
    for (const Entry &entry : labels)
        if (strcmp(entry.id, id) == 0) return entry.label;
    return id;
}
} // namespace portlabels
