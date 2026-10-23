#pragma once

// Deferred preset writes. A preset save from the browser used to go straight to LittleFS:
// every flash erase/write disables the cache and stalls the render task's flash-resident
// code, audible as dropouts while playing. Saves are parked in PSRAM instead and written by
// a low-priority task once the audio output has been silent for a moment - never while audio
// plays (a former 60 s safety-net write did exactly that and dropped out). The tradeoff: a
// power loss during an uninterrupted performance loses the pending save. Readers that must see the latest state (the engine's
// preset loader, GET /api/preset) take the pending copy; a preset change, backup export or
// shutdown-like path calls flushAll() first so nothing is lost or served stale.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace presetcache
{
constexpr size_t kMaxEntries = 4;
constexpr size_t kMaxPathLen = 128;
constexpr uint32_t kSilentMs = 700; // output quiet this long -> write now
constexpr const char *kTag = "preset_cache";

using SilentForFn = bool (*)(uint32_t ms);

struct Entry
{
    char path[kMaxPathLen];
    char *data;   // PSRAM, owned
    size_t size;
    int64_t stagedUs;
};

namespace detail
{
inline Entry gEntries[kMaxEntries] = {};
inline SemaphoreHandle_t gMutex = nullptr;
inline SilentForFn gSilentFor = nullptr;
inline TaskHandle_t gTask = nullptr;

inline void lock() { xSemaphoreTakeRecursive(gMutex, portMAX_DELAY); }
inline void unlock() { xSemaphoreGiveRecursive(gMutex); }

inline Entry *find(const char *path)
{
    for (auto &e : gEntries)
        if (e.data != nullptr && strcmp(e.path, path) == 0) return &e;
    return nullptr;
}

// Atomic replace: temp file + rename, same as the former inline handler code.
inline bool writeFile(const char *path, const char *data, size_t size)
{
    char tmp[kMaxPathLen + 5];
    const size_t n = strnlen(path, kMaxPathLen - 1); // stage() already bounds the path
    memcpy(tmp, path, n);
    memcpy(tmp + n, ".tmp", 5);
    FILE *f = fopen(tmp, "wb");
    if (f == nullptr)
    {
        ESP_LOGE(kTag, "%s could not be opened for writing", path);
        return false;
    }
    const bool ok = fwrite(data, 1, size, f) == size;
    const bool closed = fclose(f) == 0;
    if (!ok || !closed || rename(tmp, path) != 0)
    {
        unlink(tmp);
        ESP_LOGE(kTag, "%s could not be written", path);
        return false;
    }
    return true;
}

// Caller holds the lock. Writes and releases one entry (the write happens under the lock so a
// concurrent stage() for the same path cannot interleave; staging waits a few ms at most).
inline void flushEntry(Entry &e)
{
    writeFile(e.path, e.data, e.size);
    heap_caps_free(e.data);
    e.data = nullptr;
    e.size = 0;
}

inline void writerTask(void *)
{
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
        lock();
        bool pending = false;
        for (const auto &e : gEntries)
            if (e.data != nullptr) pending = true;
        if (pending && gSilentFor != nullptr && gSilentFor(kSilentMs))
        {
            for (auto &e : gEntries)
                if (e.data != nullptr) flushEntry(e);
            ESP_LOGI(kTag, "deferred preset write done (silence)");
        }
        unlock();
    }
}
} // namespace detail

// Call once (Feature::Synth::init). silentFor(ms) must answer "has the audio output been
// quiet for at least ms" from any task.
inline void init(SilentForFn silentFor)
{
    if (detail::gMutex == nullptr) detail::gMutex = xSemaphoreCreateRecursiveMutex();
    detail::gSilentFor = silentFor;
    if (detail::gTask == nullptr)
        xTaskCreatePinnedToCore(&detail::writerTask, "preset_writer", 4096, nullptr, 2, &detail::gTask, 0);
}

inline bool ready() { return detail::gMutex != nullptr; }

// Takes ownership of `data` (heap_caps allocation). Replaces an older pending copy of the
// same path; with every slot in use the oldest one is written out first.
inline bool stage(const char *path, char *data, size_t size)
{
    if (!ready() || strlen(path) >= kMaxPathLen) return false;
    detail::lock();
    Entry *slot = detail::find(path);
    if (slot != nullptr)
    {
        heap_caps_free(slot->data);
    }
    else
    {
        Entry *oldest = nullptr;
        for (auto &e : detail::gEntries)
        {
            if (e.data == nullptr) { slot = &e; break; }
            if (oldest == nullptr || e.stagedUs < oldest->stagedUs) oldest = &e;
        }
        if (slot == nullptr)
        {
            detail::flushEntry(*oldest);
            slot = oldest;
        }
        snprintf(slot->path, sizeof(slot->path), "%s", path);
    }
    slot->data = data;
    slot->size = size;
    slot->stagedUs = esp_timer_get_time();
    detail::unlock();
    return true;
}

// Returns a heap_caps copy (PSRAM, caller frees) of the pending data for `path`, or nullptr.
inline char *copyPending(const char *path, size_t &size)
{
    if (!ready()) return nullptr;
    detail::lock();
    char *copy = nullptr;
    size = 0;
    if (Entry *e = detail::find(path))
    {
        copy = static_cast<char *>(heap_caps_malloc(e->size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (copy != nullptr)
        {
            memcpy(copy, e->data, e->size);
            size = e->size;
        }
    }
    detail::unlock();
    return copy;
}

inline bool hasPending(const char *path)
{
    if (!ready()) return false;
    detail::lock();
    const bool pending = detail::find(path) != nullptr;
    detail::unlock();
    return pending;
}

// Writes everything pending now (preset change, backup export). Blocks the caller for the
// flash write - only from tasks that may stall (httpd, LVGL), never the render task.
inline void flushAll()
{
    if (!ready()) return;
    detail::lock();
    for (auto &e : detail::gEntries)
        if (e.data != nullptr) detail::flushEntry(e);
    detail::unlock();
}
} // namespace presetcache
