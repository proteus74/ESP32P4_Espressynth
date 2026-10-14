// TinySoundFont upstream commit 853a0a171759f1ddba0de1442133a75912bbeffa.
// Allocation is routed to ESP-IDF's capability heap so large decoded SF2 sample
// banks use PSRAM. The local header adds a cooperative conversion-yield hook.

#include <cstdlib>
#include <cstring>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace
{
struct AllocationHeader
{
    size_t size;
};

void *tsfPsramMalloc(size_t size)
{
    auto *raw = static_cast<AllocationHeader *>(
        heap_caps_malloc(sizeof(AllocationHeader) + size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (raw == nullptr)
    {
        raw = static_cast<AllocationHeader *>(
            heap_caps_malloc(sizeof(AllocationHeader) + size, MALLOC_CAP_8BIT));
    }
    if (raw == nullptr) return nullptr;
    raw->size = size;
    return raw + 1;
}

void tsfPsramFree(void *ptr)
{
    if (ptr != nullptr) heap_caps_free(static_cast<AllocationHeader *>(ptr) - 1);
}

void *tsfPsramRealloc(void *ptr, size_t size)
{
    if (ptr == nullptr) return tsfPsramMalloc(size);
    if (size == 0)
    {
        tsfPsramFree(ptr);
        return nullptr;
    }
    auto *oldHeader = static_cast<AllocationHeader *>(ptr) - 1;
    void *replacement = tsfPsramMalloc(size);
    if (replacement == nullptr) return nullptr;
    memcpy(replacement, ptr, oldHeader->size < size ? oldHeader->size : size);
    tsfPsramFree(ptr);
    return replacement;
}
} // namespace

#define TSF_MALLOC tsfPsramMalloc
#define TSF_REALLOC tsfPsramRealloc
#define TSF_FREE tsfPsramFree
#define TSF_RENDER_EFFECTSAMPLEBLOCK 128
#define TSF_LOAD_YIELD_INTERVAL 8192
#define TSF_LOAD_YIELD() vTaskDelay(pdMS_TO_TICKS(1))
#include "esp_log.h"
#include "esp_timer.h"
#define TSF_LOAD_PROFILE_NOW_US() static_cast<long long>(esp_timer_get_time())
#define TSF_LOAD_PROFILE_LOG(fmt, ...) ESP_LOGI("tsf", fmt, ##__VA_ARGS__)
#define TSF_IMPLEMENTATION
#include "tsf.h"
