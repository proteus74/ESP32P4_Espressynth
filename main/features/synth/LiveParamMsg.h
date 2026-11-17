#pragma once

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// A live parameter change ("paramUpdate"/"panic" frame from the browser or the touch UI) on
// its way to SynthEngine's render task. The sender parses the JSON and hands over the tree;
// the render task only walks it and applies it to the live graph in place - no rebuild, no
// disk write. Parsing and freeing use the heap (and its lock, shared with core 0), so both
// stay off the render task: applied trees come back through liveparam::gRetiredQueue and are
// freed by whichever non-audio task calls liveparam::freeRetired() next.
struct LiveParamMsg
{
    cJSON *root; // owned by the queue entry
};

namespace liveparam
{
constexpr size_t kMaxJsonLen = 256; // longer frames are not live-param messages
constexpr UBaseType_t kQueueDepth = 32;
inline QueueHandle_t gRetiredQueue = nullptr; // created next to the live-param queue

// Frees the trees the render task is done with. Any task except the render task.
inline void freeRetired()
{
    cJSON *root = nullptr;
    while (gRetiredQueue != nullptr && xQueueReceive(gRetiredQueue, &root, 0) == pdTRUE)
    {
        cJSON_Delete(root);
    }
}

// Queues `root` for the render task and takes ownership of it (also when it returns false).
// Any task except the render task.
inline bool post(QueueHandle_t queue, cJSON *root)
{
    freeRetired();
    if (root == nullptr)
    {
        return false;
    }
    LiveParamMsg msg{root};
    if (queue != nullptr && xQueueSend(queue, &msg, 0) == pdTRUE)
    {
        return true;
    }
    cJSON_Delete(root);
    return false;
}

// Render task only: hands an applied (or dropped) tree back for freeing. The retired queue
// holds twice the live queue, and every post() empties it first, so the fallback free on the
// render task itself only happens if that invariant breaks - never leak.
inline void retire(cJSON *root)
{
    if (gRetiredQueue == nullptr || xQueueSend(gRetiredQueue, &root, 0) != pdTRUE)
    {
        cJSON_Delete(root);
    }
}
} // namespace liveparam
