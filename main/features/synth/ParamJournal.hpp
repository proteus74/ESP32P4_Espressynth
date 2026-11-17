#pragma once

// Parameter autosave without a browser ("Autosave" in the web UI, DeviceSettings.h's
// paramAutosave). The browser is the only place that can serialize a whole preset, so with a
// browser open it simply saves on its own (POST /api/preset?reload=0). A Knob or Button moved
// by a MIDI CC or on the touch screen while no browser is open would be lost with the next
// preset change, though - so the render task reports every such change here, and this journal
// keeps the latest value per control in a small sidecar file next to the preset
// ("<preset>.json.live"). The sidecar is written on its own with Autosave on, or on demand by
// the touch UI's Save button (saveNow()). The preset loader re-applies the journal to every freshly built
// graph through the same Graph::applyLiveParams path a live edit takes; the browser applies it
// to its own state after loading a preset (GET /api/preset/live), and its next full save bakes
// the values into the preset and clears the journal.
//
// Threads: the render task only posts ControlChangeMsg to gQueue (non-blocking). Everything
// else (preset_nav_task's tick(), the loader's attach()/forEach(), httpd's clear()/forget()/
// toJson()) runs on core 0 under gMutex.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "PresetWriteCache.hpp"

namespace paramjournal
{
constexpr size_t kMaxEntries = 128;
constexpr size_t kWidgetIdLen = 24;       // sizeof(WidgetSlot::id)
constexpr int64_t kSaveDelayUs = 2000000; // one sidecar write per gesture, not per CC message
constexpr size_t kMaxSidecarBytes = 32 * 1024;
constexpr size_t kEntryJsonMax = 128;
constexpr const char *kTag = "param_journal";

enum class Kind : uint8_t
{
    Knob = 0,   // a = raw (0..127)
    Button = 1, // a = listIndex, b = wasAbove (0/1)
};

// Render task -> journal. `tag` names the graph the change happened in, see attach().
struct ControlChangeMsg
{
    uint16_t tag;
    Kind kind;
    uint8_t index; // control index incl. layer, 0-based
    char widgetId[kWidgetIdLen];
    float a;
    float b;
};

struct Entry
{
    char widgetId[kWidgetIdLen];
    Kind kind;
    uint8_t index;
    float a;
    float b;
};

inline QueueHandle_t gQueue = nullptr;

namespace detail
{
inline Entry gEntries[kMaxEntries] = {};
inline size_t gCount = 0;
inline char gPath[presetcache::kMaxPathLen] = {}; // preset the journal belongs to, "" = none
inline uint16_t gTag = 0;
inline int64_t gDirtyUs = 0; // esp_timer time of the first unsaved change, 0 = clean
inline bool gOnFlash = false; // a sidecar (possibly still parked in PresetWriteCache) exists
inline SemaphoreHandle_t gMutex = nullptr;

inline void lock() { xSemaphoreTake(gMutex, portMAX_DELAY); }
inline void unlock() { xSemaphoreGive(gMutex); }

inline bool sidecarPath(const char *presetPath, char *out, size_t outSize)
{
    const int n = snprintf(out, outSize, "%s.live", presetPath);
    return n > 0 && static_cast<size_t>(n) < outSize;
}

inline int formatEntry(const Entry &e, char *out, size_t outSize)
{
    if (e.kind == Kind::Knob)
    {
        return snprintf(out, outSize, "{\"widgetId\":\"%s\",\"params\":{\"knobIndex\":%u,\"raw\":%g}}", e.widgetId,
                        static_cast<unsigned>(e.index) + 1, static_cast<double>(e.a));
    }
    return snprintf(out, outSize,
                    "{\"widgetId\":\"%s\",\"params\":{\"buttonIndex\":%u,\"listIndex\":%u,\"wasAbove\":%u}}", e.widgetId,
                    static_cast<unsigned>(e.index) + 1, static_cast<unsigned>(e.a), e.b != 0.0f ? 1u : 0u);
}

// The journal as a JSON array in a PSRAM buffer (caller frees), same shape as the web UI's
// "paramUpdate" frames. Never nullptr unless out of memory.
inline char *formatLocked(size_t &len)
{
    const size_t cap = 4 + gCount * kEntryJsonMax;
    char *buf = static_cast<char *>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    len = 0;
    if (buf == nullptr) return nullptr;
    buf[len++] = '[';
    for (size_t i = 0; i < gCount; ++i)
    {
        if (i != 0) buf[len++] = ',';
        const int n = formatEntry(gEntries[i], buf + len, cap - len - 2);
        if (n <= 0 || static_cast<size_t>(n) >= cap - len - 2) break;
        len += static_cast<size_t>(n);
    }
    if (buf[len - 1] == ',') --len;
    buf[len++] = ']';
    buf[len] = '\0';
    return buf;
}

inline void stageSidecar(const char *presetPath, char *data, size_t len)
{
    char sidecar[presetcache::kMaxPathLen];
    if (data == nullptr || !sidecarPath(presetPath, sidecar, sizeof(sidecar)) ||
        !presetcache::stage(sidecar, data, len))
    {
        heap_caps_free(data);
        ESP_LOGW(kTag, "could not stage the journal of %s", presetPath);
    }
}

inline void flushLocked()
{
    if (gDirtyUs == 0 || gPath[0] == '\0') return;
    size_t len = 0;
    char *data = formatLocked(len);
    stageSidecar(gPath, data, len); // written to flash in the next output pause
    gDirtyUs = 0;
    gOnFlash = true;
}

inline Entry *findLocked(const char *widgetId, Kind kind, uint8_t index)
{
    for (size_t i = 0; i < gCount; ++i)
    {
        Entry &e = gEntries[i];
        if (e.kind == kind && e.index == index && strncmp(e.widgetId, widgetId, kWidgetIdLen) == 0) return &e;
    }
    return nullptr;
}

// Returns true when the journal changed.
inline bool upsertLocked(const char *widgetId, Kind kind, uint8_t index, float a, float b)
{
    if (widgetId[0] == '\0' || strpbrk(widgetId, "\"\\") != nullptr) return false; // not JSON-safe
    Entry *e = findLocked(widgetId, kind, index);
    if (e == nullptr)
    {
        if (gCount >= kMaxEntries)
        {
            ESP_LOGW(kTag, "journal full (%u controls), change not saved", static_cast<unsigned>(kMaxEntries));
            return false;
        }
        e = &gEntries[gCount++];
        snprintf(e->widgetId, sizeof(e->widgetId), "%s", widgetId);
        e->kind = kind;
        e->index = index;
    }
    else if (e->a == a && e->b == b)
    {
        return false;
    }
    e->a = a;
    e->b = b;
    return true;
}

inline void loadLocked()
{
    char sidecar[presetcache::kMaxPathLen];
    if (!sidecarPath(gPath, sidecar, sizeof(sidecar))) return;
    size_t size = 0;
    char *data = presetcache::copyPending(sidecar, size); // newer than the file
    if (data == nullptr)
    {
        FILE *f = fopen(sidecar, "rb");
        if (f == nullptr) return;
        data = static_cast<char *>(heap_caps_malloc(kMaxSidecarBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (data != nullptr) size = fread(data, 1, kMaxSidecarBytes, f);
        fclose(f);
    }
    if (data == nullptr) return;
    gOnFlash = true;
    cJSON *root = cJSON_ParseWithLength(data, size);
    heap_caps_free(data);
    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, root)
    {
        cJSON *widgetId = cJSON_GetObjectItemCaseSensitive(item, "widgetId");
        cJSON *params = cJSON_GetObjectItemCaseSensitive(item, "params");
        cJSON *knobIndex = cJSON_GetObjectItemCaseSensitive(params, "knobIndex");
        cJSON *buttonIndex = cJSON_GetObjectItemCaseSensitive(params, "buttonIndex");
        if (!cJSON_IsString(widgetId)) continue;
        if (cJSON_IsNumber(knobIndex) && knobIndex->valueint >= 1 && knobIndex->valueint <= 256)
        {
            cJSON *raw = cJSON_GetObjectItemCaseSensitive(params, "raw");
            if (cJSON_IsNumber(raw))
                upsertLocked(widgetId->valuestring, Kind::Knob, static_cast<uint8_t>(knobIndex->valueint - 1),
                             static_cast<float>(raw->valuedouble), 0.0f);
        }
        else if (cJSON_IsNumber(buttonIndex) && buttonIndex->valueint >= 1 && buttonIndex->valueint <= 256)
        {
            cJSON *listIndex = cJSON_GetObjectItemCaseSensitive(params, "listIndex");
            cJSON *wasAbove = cJSON_GetObjectItemCaseSensitive(params, "wasAbove");
            upsertLocked(widgetId->valuestring, Kind::Button, static_cast<uint8_t>(buttonIndex->valueint - 1),
                         cJSON_IsNumber(listIndex) ? static_cast<float>(listIndex->valuedouble) : 0.0f,
                         cJSON_IsNumber(wasAbove) && wasAbove->valueint != 0 ? 1.0f : 0.0f);
        }
    }
    cJSON_Delete(root);
}
} // namespace detail

inline bool ready() { return detail::gMutex != nullptr; }

// Once, before the engine starts (the first preset build already calls attach()).
inline void init()
{
    detail::gMutex = xSemaphoreCreateMutex();
    gQueue = xQueueCreate(32, sizeof(ControlChangeMsg));
}

// Loader: a graph for `presetPath` has just been built. Switches the journal to that preset
// (reading its sidecar) and returns the tag the new graph must carry - changes still arriving
// from the previous graph are ignored from now on. Unsaved changes of the preset being left
// are written out with `autosave`, otherwise they are dropped like any other unsaved edit.
inline uint16_t attach(const char *presetPath, bool autosave)
{
    if (!ready()) return 0;
    detail::lock();
    if (strcmp(detail::gPath, presetPath) != 0)
    {
        if (autosave) detail::flushLocked();
        detail::gDirtyUs = 0;
        detail::gCount = 0;
        detail::gOnFlash = false;
        detail::gPath[0] = '\0';
        if (strlen(presetPath) + 6 < sizeof(detail::gPath))
        {
            snprintf(detail::gPath, sizeof(detail::gPath), "%s", presetPath);
            detail::loadLocked();
        }
    }
    if (++detail::gTag == 0) ++detail::gTag;
    const uint16_t tag = detail::gTag;
    detail::unlock();
    return tag;
}

// Loader: visits every journaled control of the attached preset.
template <typename Fn> inline void forEach(Fn &&fn)
{
    if (!ready()) return;
    detail::lock();
    for (size_t i = 0; i < detail::gCount; ++i) fn(detail::gEntries[i]);
    detail::unlock();
}

// preset_nav_task, every <= 500 ms: takes over the render task's changes. With `autosave` the
// sidecar is staged once a gesture has rested for kSaveDelayUs; without it the changes stay
// in RAM until saveNow() (the device's Save button) or until the preset is left.
inline void tick(bool autosave)
{
    if (!ready()) return;
    ControlChangeMsg msg;
    detail::lock();
    while (xQueueReceive(gQueue, &msg, 0) == pdTRUE)
    {
        msg.widgetId[kWidgetIdLen - 1] = '\0';
        if (msg.tag != detail::gTag || detail::gPath[0] == '\0') continue; // from a graph that is gone
        if (detail::upsertLocked(msg.widgetId, msg.kind, msg.index, msg.a, msg.b) && detail::gDirtyUs == 0)
        {
            detail::gDirtyUs = esp_timer_get_time();
        }
    }
    if (autosave && detail::gDirtyUs != 0 && esp_timer_get_time() - detail::gDirtyUs >= kSaveDelayUs)
    {
        detail::flushLocked();
    }
    detail::unlock();
}

// The device's Save button: stages the journal now, whatever the Autosave setting. Returns
// false when nothing has changed since the last save.
inline bool saveNow()
{
    if (!ready()) return false;
    tick(false); // take over what the render task has reported so far
    detail::lock();
    const bool dirty = detail::gDirtyUs != 0;
    detail::flushLocked();
    detail::unlock();
    return dirty;
}

// httpd: the browser saved the full preset at `presetPath` - its values are the truth now.
inline void clear(const char *presetPath)
{
    if (!ready()) return;
    detail::lock();
    if (strcmp(detail::gPath, presetPath) == 0)
    {
        if (detail::gCount != 0 || detail::gOnFlash)
        {
            detail::gCount = 0;
            detail::gDirtyUs = 1; // long overdue: the next tick() stages the empty journal
        }
    }
    else
    {
        // Another preset's file was replaced: a sidecar left there would override the new content.
        char sidecar[presetcache::kMaxPathLen];
        struct stat st = {};
        if (detail::sidecarPath(presetPath, sidecar, sizeof(sidecar)) &&
            (presetcache::hasPending(sidecar) || stat(sidecar, &st) == 0))
        {
            char *empty = static_cast<char *>(heap_caps_malloc(3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (empty != nullptr) memcpy(empty, "[]", 3);
            detail::stageSidecar(presetPath, empty, 2);
        }
    }
    detail::unlock();
}

// httpd: the preset file at `presetPath` was deleted.
inline void forget(const char *presetPath)
{
    if (!ready()) return;
    detail::lock();
    if (strcmp(detail::gPath, presetPath) == 0)
    {
        detail::gCount = 0;
        detail::gDirtyUs = 0;
        detail::gOnFlash = false;
    }
    char sidecar[presetcache::kMaxPathLen];
    if (detail::sidecarPath(presetPath, sidecar, sizeof(sidecar)))
    {
        presetcache::discard(sidecar);
        unlink(sidecar);
    }
    detail::unlock();
}

// httpd: {"path":"<attached preset>","entries":[...]} (PSRAM buffer, caller frees), or
// nullptr. The path lets the browser tell whether the journal already belongs to the preset
// it has just loaded - the loader attaches a newly selected preset a moment after the select.
inline char *toJson(size_t &len)
{
    len = 0;
    if (!ready()) return nullptr;
    detail::lock();
    size_t entriesLen = 0;
    char *entries = detail::formatLocked(entriesLen);
    char *json = nullptr;
    if (entries != nullptr)
    {
        const size_t cap = entriesLen + sizeof(detail::gPath) + 32;
        json = static_cast<char *>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (json != nullptr)
        {
            len = static_cast<size_t>(snprintf(json, cap, "{\"path\":\"%s\",\"entries\":%s}", detail::gPath, entries));
        }
        heap_caps_free(entries);
    }
    detail::unlock();
    return json;
}
} // namespace paramjournal
