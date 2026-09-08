#include "app_events.hpp"
#include "app_startup.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" void app_main(void) {
    App::register_callbacks();
    App::init();
    App::start_background_tasks();

#if defined(CONFIG_APP_ENABLE_USB_DEVICE) && defined(CONFIG_USB_DEV_ENABLE_UAC2)
    uint32_t seconds = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (++seconds % 5 == 0) {
            App::log_usb_audio_stats_tick();
        }
    }
#endif
}
