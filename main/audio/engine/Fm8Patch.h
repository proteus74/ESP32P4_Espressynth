#pragma once

#include <cstdint>
#include <cstring>

// Patch of the FM8 widget: one DX7 voice in the "unpacked" layout the msfa engine (and Dexed)
// uses, 155 bytes + 1 byte operator on/off mask. Preset/live keys are "d0".."d154" and "ops".
//   op 6..1, 21 bytes each: R1-4, L1-4, BP, LD, RD, LC, RC, RS, AMS, KVS, OL, M, FC, FF, DET
//   126..133: PR1-4, PL1-4   134 ALG(0-31)  135 FB  136 OKS  137..142 LFS LFD LPMD LAMD LKS LFW
//   143 LPMS  144 TRNSP(24 = 0)  145..154 name   155 operator switches (bit n = operator n+1)
//   156 cutoff (0..99, 99 = filter off)  157 resonance (0..99); the msfa engine only reads 0..155
namespace fm8
{
constexpr int kPatchBytes = 158;

// MIDI learn: a patch byte bound to a CC on a channel. Bindings are part of the patch, so they
// travel with presets ("cc_dN" / "ch_dN" keys) and the graph applies them natively.
struct CcBinding
{
    uint8_t param;
    int8_t cc;      // -1 = not bound
    int8_t channel; // 0..15, -1 = not bound
};

// Largest value of a learnable patch byte (its keyboard/UI range starts at 0), -1 if not learnable.
// Mirrors the ranges in data/static/js/fm8.js.
inline int maxValueOf(int index)
{
    if (index >= 0 && index < 126)
    {
        switch (index % 21)
        {
        case 11: case 12: case 14: return 3;  // LC RC AMS
        case 13: case 15: return 7;           // RS KVS
        case 17: return 1;                    // mode
        case 18: return 31;                   // coarse
        case 20: return 14;                   // detune
        default: return 99;
        }
    }
    switch (index)
    {
    case 126: case 127: case 128: case 129: case 130: case 131: case 132: case 133: return 99; // PR, PL
    case 134: return 31;  // algorithm
    case 135: return 7;   // feedback
    case 136: case 141: return 1; // osc key sync, LFO key sync
    case 137: case 138: case 139: case 140: return 99; // LFO speed/delay/PMD/AMD
    case 142: return 5;   // LFO wave
    case 143: return 7;   // LFO pitch mod sens
    case 144: return 48;  // transpose
    case 156: case 157: return 99; // cutoff, resonance
    default: return -1;
    }
}

struct Patch
{
    static constexpr int kMaxBindings = 24;

    uint8_t d[kPatchBytes];
    uint32_t version = 1; // bumped on every change; voices re-apply it while sounding
    CcBinding bindings[kMaxBindings];
    uint8_t bindingCount = 0;

    Patch() { initVoice(); }

    CcBinding *findBinding(int param)
    {
        for (uint8_t i = 0; i < bindingCount; ++i)
            if (bindings[i].param == param) return &bindings[i];
        return nullptr;
    }

    void removeBinding(CcBinding *b)
    {
        *b = bindings[--bindingCount];
    }

    // Native CC route: sets every bound byte whose CC and channel match. Scaling 0..127 -> 0..max.
    void applyMidiCc(uint8_t cc, uint8_t channel, uint8_t value)
    {
        for (uint8_t i = 0; i < bindingCount; ++i)
        {
            const CcBinding &b = bindings[i];
            if (b.cc < 0 || b.channel < 0 || static_cast<uint8_t>(b.cc) != cc ||
                static_cast<uint8_t>(b.channel) != channel)
                continue;
            const int max = maxValueOf(b.param);
            if (max < 0) continue;
            d[b.param] = static_cast<uint8_t>((value * max + 63) / 127);
            ++version;
        }
    }

    // Dexed's "INIT VOICE": a single sine carrier (operator 1), everything else off.
    void initVoice()
    {
        std::memset(d, 0, sizeof(d));
        bindingCount = 0;
        for (int b = 0; b < 6; ++b) // blocks run operator 6 .. 1
        {
            uint8_t *o = d + b * 21;
            o[0] = o[1] = o[2] = o[3] = 99; // R1-4
            o[4] = o[5] = o[6] = 99;        // L1-3
            o[7] = 0;                       // L4
            o[8] = 39;                      // breakpoint C3
            o[16] = (b == 5) ? 99 : 0;      // output level: only operator 1
            o[18] = 1;                      // coarse
            o[20] = 7;                      // detune centre
        }
        d[126] = d[127] = d[128] = d[129] = 99;
        d[130] = d[131] = d[132] = d[133] = 50;
        d[136] = 1;
        d[137] = 35;
        d[141] = 1;
        d[143] = 3;
        d[144] = 24;
        std::memcpy(d + 145, "INIT VOICE", 10);
        d[155] = 0x3f;
        d[156] = 99; // filter open
    }
};
} // namespace fm8
