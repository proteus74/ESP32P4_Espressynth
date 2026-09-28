#include "MdaPianoBank.h"

#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace mdapiano
{
namespace
{
constexpr char kTag[] = "MdaPianoBank";
constexpr char kMagic[8] = {'M', 'D', 'A', 'P', 'A', 'C', 'K', '\0'};
constexpr uint32_t kVersion = 1;
constexpr uint32_t kInstrumentCount = 6;

#pragma pack(push, 1)
struct Header
{
    char magic[8];
    uint32_t version;
    uint32_t instrumentCount;
    uint32_t totalSize;
    uint32_t payloadCrc32;
};

struct Entry
{
    char name[8];
    uint32_t sampleRate;
    uint32_t noteonStep;
    uint32_t pcmOffset;
    uint32_t pcmSamples;
    uint32_t keygroupOffset;
    uint32_t keygroupCount;
    uint32_t pcmCrc32;
};
#pragma pack(pop)

void *gData = nullptr;
Instrument gInstruments[kInstrumentCount]{};
bool gLoadAttempted = false;

// Loading runs on the low-priority preset loader task (core 0). The pack is
// several MB, so both the read and the CRC work in chunks that yield between
// them - one uninterrupted pass starved core 0's idle task long enough to trip
// the task watchdog.
constexpr size_t kChunkBytes = 64 * 1024;

// Same IEEE CRC-32 as before, via the ROM's table-driven routine instead of a
// bit-by-bit loop.
uint32_t crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0;
    for (size_t done = 0; done < length; done += kChunkBytes)
    {
        const size_t chunk = length - done < kChunkBytes ? length - done : kChunkBytes;
        crc = esp_rom_crc32_le(crc, data + done, static_cast<uint32_t>(chunk));
        vTaskDelay(1);
    }
    return crc;
}

bool readChunked(FILE *file, uint8_t *data, size_t length)
{
    for (size_t done = 0; done < length; done += kChunkBytes)
    {
        const size_t chunk = length - done < kChunkBytes ? length - done : kChunkBytes;
        if (fread(data + done, 1, chunk, file) != chunk) return false;
        vTaskDelay(1);
    }
    return true;
}

bool rangeValid(uint32_t offset, uint64_t bytes, uint32_t totalSize)
{
    return offset >= sizeof(Header) + sizeof(Entry) * kInstrumentCount &&
           static_cast<uint64_t>(offset) + bytes <= totalSize;
}
} // namespace

bool loadBank(const char *path)
{
    if (gData != nullptr) return true;
    if (gLoadAttempted) return false;
    gLoadAttempted = true;

    FILE *file = fopen(path, "rb");
    if (file == nullptr)
    {
        ESP_LOGE(kTag, "Cannot open %s", path);
        return false;
    }
    Header header{};
    if (fread(&header, 1, sizeof(header), file) != sizeof(header) ||
        memcmp(header.magic, kMagic, sizeof(kMagic)) != 0 || header.version != kVersion ||
        header.instrumentCount != kInstrumentCount || header.totalSize < sizeof(Header) + sizeof(Entry) * kInstrumentCount)
    {
        ESP_LOGE(kTag, "Invalid pack header");
        fclose(file);
        return false;
    }

    void *data = heap_caps_malloc(header.totalSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (data == nullptr)
    {
        ESP_LOGE(kTag, "Cannot allocate %lu bytes in PSRAM", static_cast<unsigned long>(header.totalSize));
        fclose(file);
        return false;
    }
    const TickType_t startTicks = xTaskGetTickCount();
    rewind(file);
    const bool readOk = readChunked(file, static_cast<uint8_t *>(data), header.totalSize);
    fclose(file);
    if (!readOk)
    {
        ESP_LOGE(kTag, "Truncated pack");
        heap_caps_free(data);
        return false;
    }

    const auto *loadedHeader = static_cast<const Header *>(data);
    const auto *entries = reinterpret_cast<const Entry *>(loadedHeader + 1);
    const uint32_t payloadOffset = sizeof(Header) + sizeof(Entry) * kInstrumentCount;
    const auto *bytes = static_cast<const uint8_t *>(data);
    if (crc32(bytes + payloadOffset, header.totalSize - payloadOffset) != header.payloadCrc32)
    {
        ESP_LOGE(kTag, "Pack CRC mismatch");
        heap_caps_free(data);
        return false;
    }

    for (uint32_t i = 0; i < kInstrumentCount; ++i)
    {
        const Entry &entry = entries[i];
        const uint64_t pcmBytes = static_cast<uint64_t>(entry.pcmSamples) * sizeof(int16_t);
        const uint64_t keygroupBytes = static_cast<uint64_t>(entry.keygroupCount) * sizeof(KGRP);
        if (entry.noteonStep == 0 || entry.keygroupCount == 0 || entry.keygroupCount > 64 ||
            !rangeValid(entry.pcmOffset, pcmBytes, header.totalSize) ||
            !rangeValid(entry.keygroupOffset, keygroupBytes, header.totalSize) ||
            crc32(bytes + entry.pcmOffset, pcmBytes) != entry.pcmCrc32)
        {
            ESP_LOGE(kTag, "Invalid instrument entry %lu", static_cast<unsigned long>(i));
            heap_caps_free(data);
            return false;
        }
        Instrument &instrument = gInstruments[i];
        memcpy(instrument.name, entry.name, sizeof(instrument.name));
        instrument.sampleRate = entry.sampleRate;
        instrument.noteonStep = entry.noteonStep;
        instrument.pcm = reinterpret_cast<const int16_t *>(bytes + entry.pcmOffset);
        instrument.pcmSamples = entry.pcmSamples;
        instrument.keygroups = reinterpret_cast<const KGRP *>(bytes + entry.keygroupOffset);
        instrument.keygroupCount = entry.keygroupCount;
    }
    gData = data;
    ESP_LOGI(kTag, "Loaded %lu-byte six-instrument pack in %lu ms", static_cast<unsigned long>(header.totalSize),
             static_cast<unsigned long>(pdTICKS_TO_MS(xTaskGetTickCount() - startTicks)));
    return true;
}

const Instrument *instrument(int index)
{
    return gData != nullptr && index >= 0 && index < static_cast<int>(kInstrumentCount) ? &gInstruments[index] : nullptr;
}

int instrumentCount()
{
    return gData != nullptr ? static_cast<int>(kInstrumentCount) : 0;
}
} // namespace mdapiano