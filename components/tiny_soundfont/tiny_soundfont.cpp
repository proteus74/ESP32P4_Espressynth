// TinySoundFont upstream commit 853a0a171759f1ddba0de1442133a75912bbeffa.
// Upstream header is kept unmodified in include/tsf.h; allocation is routed to
// ESP-IDF's capability heap so large decoded SF2 sample banks use PSRAM.

#include <cstdlib>
#include <cstring>

#include "esp_heap_caps.h"

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
#define TSF_IMPLEMENTATION
#include "tsf.h"
