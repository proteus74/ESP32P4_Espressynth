#pragma once

#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace memorydiag
{
enum class ReloadPhase : uint8_t
{
    Idle,
    Started,
    FileBufferReady,
    PresetDocReady,
    Parsed,
    FileBufferFreed,
    OldGraphDestroyed,
    GraphBuilt,
    PresetDocFreed,
    Complete,
    Failed,
    Count,
};

struct HeapSnapshot
{
    uint32_t internalFree = 0;
    uint32_t internalLargest = 0;
    uint32_t internalMinimum = 0;
    uint32_t psramFree = 0;
    uint32_t psramLargest = 0;
    uint32_t psramMinimum = 0;
    uint32_t synthStackHighWaterBytes = 0;
};

struct GraphStats
{
    uint32_t instanceCount = 0;
    uint32_t nodeInstanceBytes = 0;
    uint32_t blockBufferCount = 0;
    uint32_t blockBufferBytes = 0;
    bool instancesInternal = false;
};

struct State
{
    uint32_t generation = 0;
    ReloadPhase phase = ReloadPhase::Idle;
    bool success = false;
    bool inProgress = false;
    uint32_t validPhaseMask = 0;
    HeapSnapshot phases[static_cast<size_t>(ReloadPhase::Count)] = {};
    GraphStats graph = {};
};

void registerSynthTask(TaskHandle_t task);
HeapSnapshot snapshot();
uint32_t beginReload();
void captureReloadPhase(uint32_t generation, ReloadPhase phase);
void finishReload(uint32_t generation, bool success);
void setGraphStats(uint32_t instanceCount, uint32_t nodeInstanceBytes, uint32_t blockBufferCount,
                   uint32_t blockBufferBytes, bool instancesInternal);
void read(State &out);
const char *phaseName(ReloadPhase phase);
} // namespace memorydiag