#include "SafeStart.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace safestart
{
namespace
{
constexpr const char *kTag = "SafeStart";
constexpr const char *kStateDir = "/littlefs/system";
constexpr const char *kStatePath = "/littlefs/system/boot.state";
constexpr const char *kScratchPath = "/littlefs/presets/.safe.json";
constexpr const char *kScratchJournalPath = "/littlefs/presets/.safe.json.live";
constexpr int kMaxFailedBoots = 2; // the boot after this many unhealthy ones is a safe start

struct State
{
    int attempts = 0;      // boots since the last healthy one (this boot included once begin() ran)
    bool netBroken = false; // the network failed even in a safe start: not the preset's fault
    bool netFailed = false; // the previous boot restarted because the network failed
};

State gState;
std::atomic<bool> gActive{false};
std::atomic<bool> gPresetLoaded{false};
std::atomic<bool> gNetworkDone{false};
std::atomic<bool> gHealthyWritten{false};
const char *gReason = "";
char gSkipped[64] = "";

void readState()
{
    FILE *f = fopen(kStatePath, "rb");
    if (f == nullptr) return;
    int attempts = 0, netBroken = 0, netFailed = 0;
    if (fscanf(f, "%d %d %d", &attempts, &netBroken, &netFailed) == 3 && attempts >= 0)
    {
        gState.attempts = attempts;
        gState.netBroken = netBroken != 0;
        gState.netFailed = netFailed != 0;
    }
    fclose(f);
}

void writeState()
{
    mkdir(kStateDir, 0775); // exists on every device that ran the firmware once
    FILE *f = fopen(kStatePath, "wb");
    if (f == nullptr) return;
    fprintf(f, "%d %d %d\n", gState.attempts, gState.netBroken ? 1 : 0, gState.netFailed ? 1 : 0);
    fclose(f);
}

void becomeHealthyIfDone()
{
    if (!gPresetLoaded.load() || !gNetworkDone.load() || gHealthyWritten.exchange(true)) return;
    gState.attempts = 0;
    gState.netFailed = false;
    writeState();
}
} // namespace

void begin(bool touchHeld, const char *activeName)
{
    readState();
    if (touchHeld)
    {
        gReason = "touch";
    }
    else if (gState.netFailed)
    {
        gReason = "network";
    }
    else if (gState.attempts >= kMaxFailedBoots)
    {
        gReason = "boot attempts";
    }
    const bool safe = gReason[0] != '\0';
    gActive.store(safe);
    if (safe)
    {
        snprintf(gSkipped, sizeof(gSkipped), "%s", activeName != nullptr ? activeName : "");
        ESP_LOGW(kTag, "Safe start (reason: %s): preset '%s' was not loaded. Select a preset to continue.", gReason,
                 gSkipped[0] != '\0' ? gSkipped : "(unsaved)");
    }
    // What an earlier safe start left behind is not kept: the scratch slot starts empty every time.
    unlink(kScratchPath);
    unlink(kScratchJournalPath);

    ++gState.attempts;
    gState.netFailed = false;
    writeState();

#if !defined(CONFIG_APP_ENABLE_WIFI)
    gNetworkDone.store(true); // nothing to wait for on a board without network
#endif
}

bool active()
{
    return gActive.load();
}

const char *reason()
{
    return active() ? gReason : "";
}

const char *skippedName()
{
    return gSkipped;
}

const char *scratchPath()
{
    return kScratchPath;
}

void leave()
{
    gActive.store(false);
}

void markPresetLoaded()
{
    gPresetLoaded.store(true);
    becomeHealthyIfDone();
}

void markNetwork(bool ok)
{
    if (gNetworkDone.load()) return;
    if (ok)
    {
        gState.netBroken = false;
    }
    else if (gReason[0] != '\0')
    {
        // Still no network without the preset: it is not the preset. Do not restart for it again.
        ESP_LOGW(kTag, "Network unavailable in a safe start too - not caused by the preset");
        gState.netBroken = true;
    }
    else if (!gState.netBroken)
    {
        ESP_LOGW(kTag, "Network unavailable with the boot preset loaded - restarting without it");
        gState.netFailed = true;
        writeState();
        vTaskDelay(pdMS_TO_TICKS(200)); // let the log line out
        esp_restart();
    }
    gNetworkDone.store(true);
    becomeHealthyIfDone();
}
} // namespace safestart
