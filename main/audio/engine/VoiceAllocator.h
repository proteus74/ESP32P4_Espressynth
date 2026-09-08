#pragma once

#include <cstdint>

#include "NodeTypes.h"
#include "SynthConfig.h"

// Port of static/js/main.js's allocateVoice()/findVoiceForNote() note-to-
// voice-slot state machine, extended with sustain-pedal (CC64) support -
// see Slot::held below. One instance per midi-in widget in the loaded
// preset. Voice count is always SYNTH_VOICE_COUNT here (fixed, unlike the
// JS reference) - a mono/monophonic midi-in (preset voices==1) still routes
// through slot 0 of the same machinery for uniformity.
class VoiceAllocator
{
public:
    void configure(uint8_t voiceCount, OverflowMode mode)
    {
        voiceCount_ = (voiceCount < 1) ? 1 : (voiceCount > SYNTH_VOICE_COUNT ? SYNTH_VOICE_COUNT : voiceCount);
        overflowMode_ = mode;
        for (auto &slot : slots_)
        {
            slot.note = -1;
            slot.heldSince = 0;
            slot.held = false;
        }
        for (auto &heldSince : monoHeldSince_)
        {
            heldSince = 0;
        }
        holdCounter_ = 0;
    }

    // Returns the voice slot index (0..SYNTH_VOICE_COUNT-1) this note-on was
    // assigned to, or -1 if dropped (overflowMode==Ignore, no free/oldest
    // slot available). Steals a held (sustain-only) slot in preference to an
    // actively-held one when the voice pool is full - see findOldestSlot().
    //
    // stillSounding (optional, indexed by slot): a slot with note<0 (freed
    // on note-off, see noteOff) can still be audibly ringing out its ADSR
    // release - `note<0` alone doesn't mean "safe to reuse without an
    // audible cutoff". When provided, a note<0-but-stillSounding[i] slot is
    // only picked once every note<0-and-truly-idle slot has been tried
    // (tier 2 below), instead of being treated as equally free (which used
    // to make noteOn() grab the just-released low-index slot ahead of
    // genuinely idle ones - audibly chopping off a long release the moment
    // any other note played).
    int8_t noteOn(uint8_t note, const bool *stillSounding = nullptr)
    {
        ++holdCounter_;

        if (voiceCount_ == 1)
        {
            monoHeldSince_[note] = holdCounter_;
            slots_[0].note = note;
            slots_[0].heldSince = holdCounter_;
            slots_[0].held = false;
            return 0;
        }

        // Same note already sounding: reuse its slot (fast retrigger).
        for (uint8_t i = 0; i < voiceCount_; ++i)
        {
            if (slots_[i].note == static_cast<int16_t>(note))
            {
                slots_[i].heldSince = holdCounter_;
                slots_[i].held = false; // re-striking un-sustains it
                return static_cast<int8_t>(i);
            }
        }

        // First truly idle slot (freed and its release has actually decayed
        // to silence, or never used).
        for (uint8_t i = 0; i < voiceCount_; ++i)
        {
            if (slots_[i].note < 0 && (stillSounding == nullptr || !stillSounding[i]))
            {
                slots_[i].note = note;
                slots_[i].heldSince = holdCounter_;
                slots_[i].held = false;
                return static_cast<int8_t>(i);
            }
        }

        // No fully-idle slot: reuse the oldest slot that's freed but still
        // ringing out its release, rather than falling straight to
        // overflow-stealing an actively-held/sustained voice.
        if (stillSounding != nullptr)
        {
            int8_t oldestReleasing = -1;
            for (uint8_t i = 0; i < voiceCount_; ++i)
            {
                if (slots_[i].note < 0 &&
                    (oldestReleasing < 0 || slots_[i].heldSince < slots_[oldestReleasing].heldSince))
                {
                    oldestReleasing = static_cast<int8_t>(i);
                }
            }
            if (oldestReleasing >= 0)
            {
                slots_[oldestReleasing].note = note;
                slots_[oldestReleasing].heldSince = holdCounter_;
                slots_[oldestReleasing].held = false;
                return oldestReleasing;
            }
        }

        if (overflowMode_ == OverflowMode::Ignore)
        {
            return -1;
        }

        // Steal a held (sustain-only, key already released) slot in preference
        // to an actively-held one; within either pool, steal the oldest
        // (smallest heldSince, round-robin, deterministic).
        int8_t stealIdx = findOldestSlot(/*onlyHeld=*/true);
        if (stealIdx < 0)
        {
            stealIdx = findOldestSlot(/*onlyHeld=*/false);
        }
        slots_[stealIdx].note = note;
        slots_[stealIdx].heldSince = holdCounter_;
        slots_[stealIdx].held = false;
        return stealIdx;
    }

    // sustainDown: current CC64 state for this midi-in. If true, the slot is
    // marked held (kept sounding, not freed) and -1 is returned - matches
    // the existing "no delivery" convention the caller already relies on via
    // `if (voiceIdx < 0) continue;`. If false (or the note wasn't found),
    // behaves exactly as before: frees the slot and returns its index, or -1
    // if the note was already stolen (correct no-op per JS semantics).
    int8_t noteOff(uint8_t note, bool sustainDown)
    {
        if (voiceCount_ == 1)
        {
            monoHeldSince_[note] = 0;
            if (slots_[0].note != static_cast<int16_t>(note))
            {
                return -1;
            }
            if (sustainDown)
            {
                slots_[0].held = true;
                return -1;
            }
            const int16_t fallback = newestHeldMonoNote();
            slots_[0].note = fallback;
            slots_[0].held = false;
            return 0;
        }
        for (uint8_t i = 0; i < voiceCount_; ++i)
        {
            if (slots_[i].note == static_cast<int16_t>(note))
            {
                if (sustainDown)
                {
                    slots_[i].held = true; // defer - keep sounding, don't free
                    return -1;
                }
                slots_[i].note = -1;
                slots_[i].held = false;
                return static_cast<int8_t>(i);
            }
        }
        return -1; // already stolen - correct no-op
    }

    // Frees every currently held (sustain-only) slot - call once on pedal-up.
    // Writes freed slot indices into outVoiceIndices and their note numbers
    // into outNotes (same index, both capacity maxOut, pass SYNTH_VOICE_COUNT)
    // and returns how many were written. outNotes exists so a caller can
    // still resolve e.g. a midi-split's side/transpose for the deferred
    // release - the slot's note would otherwise be lost the moment it's
    // cleared below.
    uint8_t releaseSustained(int8_t *outVoiceIndices, uint8_t *outNotes, uint8_t maxOut)
    {
        uint8_t count = 0;
        for (uint8_t i = 0; i < voiceCount_ && count < maxOut; ++i)
        {
            if (slots_[i].held)
            {
                outNotes[count] = static_cast<uint8_t>(slots_[i].note);
                slots_[i].note = -1;
                slots_[i].held = false;
                outVoiceIndices[count++] = static_cast<int8_t>(i);
            }
        }
        return count;
    }

    int16_t currentNote(uint8_t voiceIndex) const
    {
        return voiceIndex < voiceCount_ ? slots_[voiceIndex].note : -1;
    }

private:
    struct Slot
    {
        int16_t note = -1;
        uint32_t heldSince = 0;
        bool held = false; // true = released while pedal was down, still sounding (sustain-held)
    };

    // Index of the occupied slot with the smallest heldSince, optionally
    // restricted to held==true slots; -1 if the search set is empty.
    int8_t findOldestSlot(bool onlyHeld) const
    {
        int8_t best = -1;
        for (uint8_t i = 0; i < voiceCount_; ++i)
        {
            if (slots_[i].note < 0)
            {
                continue;
            }
            if (onlyHeld && !slots_[i].held)
            {
                continue;
            }
            if (best < 0 || slots_[i].heldSince < slots_[best].heldSince)
            {
                best = static_cast<int8_t>(i);
            }
        }
        return best;
    }

    int16_t newestHeldMonoNote() const
    {
        int16_t newest = -1;
        uint32_t newestSince = 0;
        for (uint16_t note = 0; note < 128; ++note)
        {
            if (monoHeldSince_[note] > newestSince)
            {
                newest = static_cast<int16_t>(note);
                newestSince = monoHeldSince_[note];
            }
        }
        return newest;
    }

    Slot slots_[SYNTH_VOICE_COUNT];
    uint32_t monoHeldSince_[128] = {};
    uint32_t holdCounter_ = 0;
    uint8_t voiceCount_ = 1;
    OverflowMode overflowMode_ = OverflowMode::StealOldest;
};
