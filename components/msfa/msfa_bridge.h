#pragma once

// Narrow C++ interface to the msfa DX7 engine (Google "music-synthesizer-for-android", Apache-2.0,
// via the MicroDexed/Dexed forks - see NOTICE.md). The msfa headers define global templates
// (min/max) and macros, so they stay out of the application: everything else only sees this file.
//
// Patch format = the "unpacked" DX7 voice (what Dexed keeps in memory): per operator
// 6..1 in blocks of 21 bytes (R1-4, L1-4, BP, LD, RD, LC, RC, RS, AMS, KVS, OL, M, FC, FF, DET),
// then PR1-4, PL1-4, ALG, FB, OKS, LFS, LFD, LPMD, LAMD, LKS, LFW, LPMS, TRNSP, 10 name bytes.
#include <cstddef>
#include <cstdint>

namespace msfa_bridge
{
constexpr int kBlock = 64;        // msfa renders in blocks of 64 samples
constexpr int kPatchBytes = 156;  // 155 voice bytes + 1 spare (operator switches use bit 0-5 of byte 155)

// Idempotent; must be called before the first voice renders.
void init(float sampleRate);

struct Voice;
Voice *create();
void destroy(Voice *voice);

// New note on this voice (velocity 1..127). Restarts envelopes and, if LFO key sync is on, the LFO.
void noteOn(Voice *voice, const uint8_t patch[kPatchBytes], int midiNote, int velocity);
void noteOff(Voice *voice);
// Re-applies the patch while a note is held (live editing); keeps phases and envelope positions.
void update(Voice *voice, const uint8_t patch[kPatchBytes], int midiNote, int velocity);

// Renders `blocks` x 64 samples into out[] (float, +-1.0; overwritten). pitchBend14 = 0x2000 centre.
// Returns the peak magnitude of the rendered audio.
float render(Voice *voice, float *out, int blocks, int pitchBend14, int pitchRangeSemis);
} // namespace msfa_bridge
