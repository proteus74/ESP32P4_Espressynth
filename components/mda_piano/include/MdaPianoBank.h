#pragma once

#include <cstdint>

#include "mdaEPiano.h"

namespace mdapiano
{
struct Instrument
{
    char name[8];
    uint32_t sampleRate;
    uint32_t noteonStep;
    const int16_t *pcm;
    uint32_t pcmSamples;
    const KGRP *keygroups;
    uint32_t keygroupCount;
};

bool loadBank(const char *path = "/littlefs/samples/mda-piano.pack");
const Instrument *instrument(int index);
int instrumentCount();
} // namespace mdapiano