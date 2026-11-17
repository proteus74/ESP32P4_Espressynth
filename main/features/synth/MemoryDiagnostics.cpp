#include "MemoryDiagnostics.h"
#include "SafeStart.h"

#include "esp_heap_caps.h"

namespace
{
portMUX_TYPE gMux = portMUX_INITIALIZER_UNLOCKED;
memorydiag::State gState;
TaskHandle_t gSynthTask = nullptr;
uint32_t gLvglTotal = 0, gLvglUsed = 0, gLvglMaxUsed = 0;
uint8_t gLvglFragPct = 0;
}

namespace memorydiag
{
void registerSynthTask(TaskHandle_t task)
{
    portENTER_CRITICAL(&gMux);
    gSynthTask = task;
    portEXIT_CRITICAL(&gMux);
}

void setLvglStats(uint32_t total, uint32_t used, uint32_t maxUsed, uint8_t fragPct)
{
    portENTER_CRITICAL(&gMux);
    gLvglTotal = total;
    gLvglUsed = used;
    gLvglMaxUsed = maxUsed;
    gLvglFragPct = fragPct;
    portEXIT_CRITICAL(&gMux);
}

HeapSnapshot snapshot()
{
    HeapSnapshot result;
    constexpr uint32_t internalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    constexpr uint32_t psramCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    result.internalFree = static_cast<uint32_t>(heap_caps_get_free_size(internalCaps));
    result.internalLargest = static_cast<uint32_t>(heap_caps_get_largest_free_block(internalCaps));
    result.internalMinimum = static_cast<uint32_t>(heap_caps_get_minimum_free_size(internalCaps));
    result.psramFree = static_cast<uint32_t>(heap_caps_get_free_size(psramCaps));
    result.psramLargest = static_cast<uint32_t>(heap_caps_get_largest_free_block(psramCaps));
    result.psramMinimum = static_cast<uint32_t>(heap_caps_get_minimum_free_size(psramCaps));

    TaskHandle_t synthTask;
    portENTER_CRITICAL(&gMux);
    synthTask = gSynthTask;
    result.lvglTotal = gLvglTotal;
    result.lvglUsed = gLvglUsed;
    result.lvglMaxUsed = gLvglMaxUsed;
    result.lvglFragPct = gLvglFragPct;
    portEXIT_CRITICAL(&gMux);
    if (synthTask != nullptr)
    {
        result.synthStackHighWaterBytes = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(synthTask));
    }
    return result;
}

uint32_t beginReload()
{
    const HeapSnapshot current = snapshot();
    portENTER_CRITICAL(&gMux);
    const uint32_t generation = ++gState.generation;
    gState.phase = ReloadPhase::Started;
    gState.success = false;
    gState.inProgress = true;
    gState.validPhaseMask = 1U << static_cast<uint8_t>(ReloadPhase::Started);
    for (HeapSnapshot &phase : gState.phases)
    {
        phase = {};
    }
    gState.phases[static_cast<size_t>(ReloadPhase::Started)] = current;
    portEXIT_CRITICAL(&gMux);
    return generation;
}

void captureReloadPhase(uint32_t generation, ReloadPhase phase)
{
    if (phase >= ReloadPhase::Count)
    {
        return;
    }
    const HeapSnapshot current = snapshot();
    portENTER_CRITICAL(&gMux);
    if (generation == gState.generation)
    {
        const uint8_t index = static_cast<uint8_t>(phase);
        gState.phase = phase;
        gState.validPhaseMask |= 1U << index;
        gState.phases[index] = current;
    }
    portEXIT_CRITICAL(&gMux);
}

void finishReload(uint32_t generation, bool success)
{
    safestart::markPresetLoaded(); // the firmware survived a preset load (only the first one counts)
    const ReloadPhase phase = success ? ReloadPhase::Complete : ReloadPhase::Failed;
    captureReloadPhase(generation, phase);
    portENTER_CRITICAL(&gMux);
    if (generation == gState.generation)
    {
        gState.success = success;
        gState.inProgress = false;
    }
    portEXIT_CRITICAL(&gMux);
}

void setGraphStats(uint32_t instanceCount, uint32_t nodeInstanceBytes, uint32_t blockBufferCount,
                   uint32_t blockBufferBytes, bool instancesInternal, uint32_t outputPsramFallbacks)
{
    portENTER_CRITICAL(&gMux);
    gState.graph.instanceCount = instanceCount;
    gState.graph.nodeInstanceBytes = nodeInstanceBytes;
    gState.graph.blockBufferCount = blockBufferCount;
    gState.graph.blockBufferBytes = blockBufferBytes;
    gState.graph.instancesInternal = instancesInternal;
    gState.graph.outputPsramFallbacks = outputPsramFallbacks;
    portEXIT_CRITICAL(&gMux);
}

void read(State &out)
{
    portENTER_CRITICAL(&gMux);
    out = gState;
    portEXIT_CRITICAL(&gMux);
}

const char *phaseName(ReloadPhase phase)
{
    switch (phase)
    {
    case ReloadPhase::Idle: return "idle";
    case ReloadPhase::Started: return "started";
    case ReloadPhase::FileBufferReady: return "fileBufferReady";
    case ReloadPhase::PresetDocReady: return "presetDocReady";
    case ReloadPhase::Parsed: return "parsed";
    case ReloadPhase::FileBufferFreed: return "fileBufferFreed";
    case ReloadPhase::OldGraphDestroyed: return "oldGraphDestroyed";
    case ReloadPhase::GraphBuilt: return "graphBuilt";
    case ReloadPhase::PresetDocFreed: return "presetDocFreed";
    case ReloadPhase::Complete: return "complete";
    case ReloadPhase::Failed: return "failed";
    default: return "unknown";
    }
}
} // namespace memorydiag