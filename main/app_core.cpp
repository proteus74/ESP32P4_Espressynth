#include "app_core.hpp"
#include "freertos/task.h"

// xTaskCreate() and the heap-capability helpers ultimately use these APIs.
// Preserve explicit affinity (including IDF's per-core IPC and idle tasks).
extern "C" {

BaseType_t __real_xTaskCreatePinnedToCore(TaskFunction_t task, const char *name,
                                       uint32_t stackDepth, void *parameters,
                                       UBaseType_t priority, TaskHandle_t *handle,
                                       BaseType_t core);

BaseType_t __wrap_xTaskCreatePinnedToCore(TaskFunction_t task, const char *name,
                                       uint32_t stackDepth, void *parameters,
                                       UBaseType_t priority, TaskHandle_t *handle,
                                       BaseType_t core)
{
    return __real_xTaskCreatePinnedToCore(task, name, stackDepth, parameters, priority,
                                        handle, core == tskNO_AFFINITY ? App::MAIN_CORE : core);
}

TaskHandle_t __real_xTaskCreateStaticPinnedToCore(TaskFunction_t task, const char *name,
                                               uint32_t stackDepth, void *parameters,
                                               UBaseType_t priority, StackType_t *stack,
                                               StaticTask_t *taskBuffer, BaseType_t core);

TaskHandle_t __wrap_xTaskCreateStaticPinnedToCore(TaskFunction_t task, const char *name,
                                               uint32_t stackDepth, void *parameters,
                                               UBaseType_t priority, StackType_t *stack,
                                               StaticTask_t *taskBuffer, BaseType_t core)
{
    return __real_xTaskCreateStaticPinnedToCore(task, name, stackDepth, parameters, priority,
                                              stack, taskBuffer,
                                              core == tskNO_AFFINITY ? App::MAIN_CORE : core);
}

} // extern "C"
