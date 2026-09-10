#pragma once

// LVGL's LV_MEM_POOL_ALLOC hook. This header is included directly by LVGL's C allocator unit,
// so the implementation must be available here rather than in the application component.
// The hook name is passed as an object-like compile definition because CMake drops
// function-style definitions containing parentheses.

#include <stddef.h>
#include "esp_heap_caps.h"

static inline void* espressynth_lv_mem_pool_alloc(size_t size)
{
    void* pool = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(pool == NULL) {
        pool = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return pool;
}
