#include "app_events.hpp"
#include "app_startup.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" void app_main(void) {
    App::register_callbacks();
    App::init();
    App::start_background_tasks();

#if (defined(CONFIG_APP_ENABLE_USB_DEVICE) && defined(CONFIG_USB_DEV_ENABLE_UAC2)) || \
    defined(CONFIG_APP_ENABLE_SYNTH)
    uint32_t seconds = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ++seconds;
#if defined(CONFIG_APP_ENABLE_USB_DEVICE) && defined(CONFIG_USB_DEV_ENABLE_UAC2)
        if (seconds % 5 == 0) {
            App::log_usb_audio_stats_tick();
        }Thank you. 
#endif
#if defined(CONFIG_APP_ENABLE_SYNTH) && defined(CONFIG_APP_ENABLE_WEBSERVER)
        Feature::Synth::broadcast_status_tick();
#endif
#if defined(CONFIG_APP_ENABLE_SYNTH)
        // Real DMA-ring underrun log + reset-on-reload/Panic - kept off the web-server gate
        // above so it still shows up on the serial log with no browser/UI involved.
        Feature::Synth::dma_underrun_tick();
        // Native P0 baseline diagnostic (documentation/native-synth-
        // optimization-plan-2026-09-09.md) - off the render task on
        // purpose, see log_p0_baseline_tick()'s comment (synth_manager.hpp).
        // if (seconds % 2 == 0) {
        //     Feature::Synth::log_p0_baseline_tick();
        // }
#endif
    }
#endif
}
