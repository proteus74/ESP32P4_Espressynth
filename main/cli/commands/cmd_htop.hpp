#pragma once

// "htop" command: like "top" (cmd_top.hpp), but redraws every second in place (via
// clear_screen(), cmd_clear.hpp) instead of printing a one-shot snapshot, and adds a
// small scrolling load-history sparkline per core, similar to Linux htop's CPU meters.
// Same prerequisites as "top": CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS and
// CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID (both set in sdkconfig.defaults).
//
// Quits on 'q'/Q/Ctrl+C/Esc, checked via select() on stdin so the same call doubles as
// the ~1s redraw wait. That needs select() support from the active console driver --
// true for UART and USB-Serial-JTAG (this project's default). If the console backend in
// use doesn't support it, select() returning an error just falls back to a plain delay,
// so the display still refreshes -- only the keypress-quit stops working (reset to exit).

#include "cli/commands/cmd_clear.hpp"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>
#include <strings.h>
#include <sys/select.h>

namespace Feature::CLI::Commands {

namespace {

// Prefixed to avoid colliding with cmd_top.hpp's identically-purposed helpers: an
// unnamed namespace is per-*translation-unit*, not per-file, and since this project's
// commands are header-only, cmd_top.hpp's and this file's "namespace {}" are the very
// same namespace once both get included into main.cpp -- same-named symbols in both
// would be a redefinition error, not two independent file-local ones.
constexpr int HTOP_MAX_LISTED_TASKS = 32;
constexpr int HISTORY_LENGTH = 40;
// 8 shading levels, 0-100% in ~12.5% steps, ASCII-only so it renders on any terminal.
constexpr char SPARKLINE_RAMP[] = " .:-=+*#";

inline const char* htop_task_state_name(eTaskState state) {
    switch (state) {
        case eRunning:   return "running";
        case eReady:     return "ready";
        case eBlocked:   return "blocked";
        case eSuspended: return "suspended";
        case eDeleted:   return "deleted";
        default:         return "invalid";
    }
}

// See cmd_top.hpp for the same helper -- duplicated (under a different name, see above)
// rather than shared, matching this project's one-file-per-command convention.
inline const char* htop_task_core_name(BaseType_t core_id, char* buffer, size_t buffer_size) {
    if (core_id == tskNO_AFFINITY) {
        return "any";
    }
    snprintf(buffer, buffer_size, "%d", static_cast<int>(core_id));
    return buffer;
}

inline char htop_sparkline_char(unsigned percent) {
    if (percent > 100) percent = 100;
    constexpr int levels = static_cast<int>(sizeof(SPARKLINE_RAMP)) - 1; // exclude '\0'
    int index = static_cast<int>((percent * levels) / 100);
    if (index >= levels) index = levels - 1;
    return SPARKLINE_RAMP[index];
}

// Sorts in place by CPU time (descending) or by name (ascending). Plain insertion sort:
// count is capped at HTOP_MAX_LISTED_TASKS, so this is cheap even once per second.
inline void htop_sort_tasks(TaskStatus_t* tasks, UBaseType_t count, bool by_cpu) {
    for (UBaseType_t i = 1; i < count; ++i) {
        TaskStatus_t key = tasks[i];
        UBaseType_t j = i;
        while (j > 0) {
            const bool key_goes_first = by_cpu
                ? tasks[j - 1].ulRunTimeCounter < key.ulRunTimeCounter
                : strcasecmp(tasks[j - 1].pcTaskName, key.pcTaskName) > 0;
            if (!key_goes_first) break;
            tasks[j] = tasks[j - 1];
            --j;
        }
        tasks[j] = key;
    }
}

// Blocks up to 1 second waiting for a keypress on stdin. Returns true if 'q'/Q/Ctrl+C/Esc
// was pressed (caller should stop). 's'/'S' toggles *sort_by_cpu instead of quitting; any
// other key is consumed and ignored.
inline bool htop_poll_key(bool* sort_by_cpu) {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(fileno(stdin), &readfds);
    struct timeval tv = {1, 0};

    const int ready = select(fileno(stdin) + 1, &readfds, nullptr, nullptr, &tv);
    if (ready <= 0) {
        if (ready < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000)); // select() unsupported here -- plain fallback delay
        }
        return false;
    }
    const int ch = fgetc(stdin);
    if (ch == 's' || ch == 'S') {
        *sort_by_cpu = !*sort_by_cpu;
        return false;
    }
    return ch == 'q' || ch == 'Q' || ch == 0x03 || ch == 0x1b;
}

inline int cli_cmd_htop(int argc, char** argv) {
    (void)argc;
    (void)argv;

    static TaskStatus_t status[HTOP_MAX_LISTED_TASKS];
    uint8_t history[configNUMBER_OF_CORES][HISTORY_LENGTH]{};
    int history_count = 0;
    int history_head = 0; // next write position (ring buffer)
    bool sort_by_cpu = false; // default: sorted by name; 's' toggles to CPU% and back

    while (true) {
        const UBaseType_t count = uxTaskGetSystemState(status, HTOP_MAX_LISTED_TASKS, nullptr);
        const configRUN_TIME_COUNTER_TYPE total_runtime_pct = portGET_RUN_TIME_COUNTER_VALUE() / 100;
        htop_sort_tasks(status, count, sort_by_cpu);

        unsigned per_core_load[configNUMBER_OF_CORES];
        unsigned load_sum = 0;
        for (int core = 0; core < configNUMBER_OF_CORES; ++core) {
            unsigned idle_percent = static_cast<unsigned>(ulTaskGetIdleRunTimePercentForCore(core));
            if (idle_percent > 100) idle_percent = 100; // rounding, e.g. right after boot
            per_core_load[core] = 100 - idle_percent;
            load_sum += per_core_load[core];
            history[core][history_head] = static_cast<uint8_t>(per_core_load[core]);
        }
        history_head = (history_head + 1) % HISTORY_LENGTH;
        if (history_count < HISTORY_LENGTH) ++history_count;

        clear_screen();
        printf("htop - refreshes every 1s, sorted by %s ('s' to toggle, 'q' to quit)\n",
               sort_by_cpu ? "CPU%" : "name");
        printf("CPU load: %u%%\n\n", load_sum / configNUMBER_OF_CORES);

        for (int core = 0; core < configNUMBER_OF_CORES; ++core) {
            printf("Core %d [", core);
            // Oldest sample first (left) to newest (right), like a scrolling strip chart.
            for (int i = 0; i < history_count; ++i) {
                const int idx = (history_head - history_count + i + HISTORY_LENGTH) % HISTORY_LENGTH;
                putchar(htop_sparkline_char(history[core][idx]));
            }
            for (int i = history_count; i < HISTORY_LENGTH; ++i) putchar(' ');
            printf("] %3u%%\n", per_core_load[core]);
        }
        printf("\n");

        printf("%-16s %-10s %-4s %-8s %5s  %s\n", "Name", "State", "Core", "Priority", "CPU%", "Stack free (words)");
        for (UBaseType_t i = 0; i < count; ++i) {
            const unsigned cpu_percent =
                total_runtime_pct > 0 ? static_cast<unsigned>(status[i].ulRunTimeCounter / total_runtime_pct) : 0;
            char core_buffer[12];
            printf("%-16s %-10s %-4s %-8u %4u%%  %u\n", status[i].pcTaskName,
                   htop_task_state_name(status[i].eCurrentState),
                   htop_task_core_name(status[i].xCoreID, core_buffer, sizeof(core_buffer)),
                   static_cast<unsigned>(status[i].uxCurrentPriority), cpu_percent,
                   static_cast<unsigned>(status[i].usStackHighWaterMark));
        }
        if (uxTaskGetNumberOfTasks() > HTOP_MAX_LISTED_TASKS) {
            printf("(%u more task(s) not shown)\n",
                   static_cast<unsigned>(uxTaskGetNumberOfTasks() - HTOP_MAX_LISTED_TASKS));
        }
        fflush(stdout);

        if (htop_poll_key(&sort_by_cpu)) break;
    }

    printf("htop stopped.\n");
    return 0;
}

} // namespace

inline void register_htop() {
    const esp_console_cmd_t cmd = {
        .command = "htop",
        .help = "Live task/CPU monitor with a per-core load history, refreshes every 1s "
                "('s' to sort by CPU%, 'q' to quit)",
        .hint = nullptr,
        .func = &cli_cmd_htop,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    register_command(cmd);
}

} // namespace Feature::CLI::Commands
