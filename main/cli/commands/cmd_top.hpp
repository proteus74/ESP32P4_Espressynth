#pragma once

// Requires CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS (set in sdkconfig.defaults), which enables
// uxTaskGetSystemState()'s per-task run time counters and per-core idle time tracking
// (ulTaskGetIdleRunTimePercentForCore(), from freertos/idf_additions.h -- this project's
// FreeRTOS build is the classic, not the newer "SMP", kernel).

#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include <cstdio>

namespace Feature::CLI::Commands {

namespace {

constexpr int MAX_LISTED_TASKS = 32;

inline const char* task_state_name(eTaskState state) {
    switch (state) {
        case eRunning:   return "running";
        case eReady:     return "ready";
        case eBlocked:   return "blocked";
        case eSuspended: return "suspended";
        case eDeleted:   return "deleted";
        default:         return "invalid";
    }
}

// xCoreID is the task's core AFFINITY (0, 1, or tskNO_AFFINITY for "can run on
// either core"), not a live "currently executing on" snapshot -- but since almost
// every task in this project is pinned via xTaskCreatePinnedToCore, it tells you
// which core a given task actually runs on in practice.
inline const char* task_core_name(BaseType_t core_id, char* buffer, size_t buffer_size) {
    if (core_id == tskNO_AFFINITY) {
        return "any";
    }
    snprintf(buffer, buffer_size, "%d", static_cast<int>(core_id));
    return buffer;
}

inline int cli_cmd_top(int argc, char** argv) {
    (void)argc;
    (void)argv;

    static TaskStatus_t status[MAX_LISTED_TASKS];
    const UBaseType_t count = uxTaskGetSystemState(status, MAX_LISTED_TASKS, nullptr);

    // Percentage of one core's wall-clock time each task has consumed since boot -- a task
    // that's kept one core fully busy reads ~100%, not 50%, on a dual-core board.
    const configRUN_TIME_COUNTER_TYPE total_runtime_pct = portGET_RUN_TIME_COUNTER_VALUE() / 100;

    printf("%-16s %-10s %-4s %-8s %5s  %s\n", "Name", "State", "Core", "Priority", "CPU%", "Stack free (words)");
    for (UBaseType_t i = 0; i < count; ++i) {
        const unsigned cpu_percent =
            total_runtime_pct > 0 ? static_cast<unsigned>(status[i].ulRunTimeCounter / total_runtime_pct) : 0;
        char core_buffer[12]; // fits any int32_t incl. sign, though core_id is really just 0/1
        printf("%-16s %-10s %-4s %-8u %4u%%  %u\n", status[i].pcTaskName, task_state_name(status[i].eCurrentState),
               task_core_name(status[i].xCoreID, core_buffer, sizeof(core_buffer)),
               static_cast<unsigned>(status[i].uxCurrentPriority), cpu_percent,
               static_cast<unsigned>(status[i].usStackHighWaterMark));
    }
    if (uxTaskGetNumberOfTasks() > MAX_LISTED_TASKS) {
        printf("(%u more task(s) not shown)\n", static_cast<unsigned>(uxTaskGetNumberOfTasks() - MAX_LISTED_TASKS));
    }

    unsigned per_core_load[configNUMBER_OF_CORES];
    unsigned load_sum = 0;
    for (int core = 0; core < configNUMBER_OF_CORES; ++core) {
        unsigned idle_percent = static_cast<unsigned>(ulTaskGetIdleRunTimePercentForCore(core));
        if (idle_percent > 100) idle_percent = 100; // rounding, e.g. right after boot
        per_core_load[core] = 100 - idle_percent;
        load_sum += per_core_load[core];
    }

    printf("\nCPU load: %u%%\n", load_sum / configNUMBER_OF_CORES);
    for (int core = 0; core < configNUMBER_OF_CORES; ++core) {
        printf("  Core %d: %u%%\n", core, per_core_load[core]);
    }
    return 0;
}

} // namespace

inline void register_top() {
    const esp_console_cmd_t cmd = {
        .command = "top",
        .help = "List FreeRTOS tasks (state, core, priority, CPU%, free stack) plus total/per-core CPU load",
        .hint = nullptr,
        .func = &cli_cmd_top,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    register_command(cmd);
}

} // namespace Feature::CLI::Commands
