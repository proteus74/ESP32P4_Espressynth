#pragma once

#include <cstdint>
#include <cstring>

// Patch of the FM8 widget: one DX7 voice in the "unpacked" layout the msfa engine (and Dexed)
// uses, 155 bytes + 1 byte operator on/off mask. Preset/live keys are "d0".."d154" and "ops".
//   op 6..1, 21 bytes each: R1-4, L1-4, BP, LD, RD, LC, RC, RS, AMS, KVS, OL, M, FC, FF, DET
//   126..133: PR1-4, PL1-4   134 ALG(0-31)  135 FB  136 OKS  137..142 LFS LFD LPMD LAMD LKS LFW
//   143 LPMS  144 TRNSP(24 = 0)  145..154 name   155 operator switches (bit n = operator n+1)
namespace fm8
{
constexpr int kPatchBytes = 156;

struct Patch
{
    uint8_t d[kPatchBytes];
    uint32_t version = 1; // bumped on every change; voices re-apply it while sounding

    Patch() { initVoice(); }

    // Dexed's "INIT VOICE": a single sine carrier (operator 1), everything else off.
    void initVoice()
    {
        std::memset(d, 0, sizeof(d));
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
    }
};
} // namespace fm8
