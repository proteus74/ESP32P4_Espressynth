#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if CONFIG_EH_HOST_PORT_HAS_DMA
extern "C" void *__real_eh_host_port_dma_alloc_aligned(size_t size, size_t alignment);

extern "C" void *__wrap_eh_host_port_dma_alloc_aligned(size_t size, size_t alignment)
{
#if CONFIG_EH_HOST_PORT_DMA_PREFER_SPIRAM && CONFIG_ESP_HOSTED_HOST_TRANSPORT_BUS_SDIO && SOC_SDMMC_PSRAM_DMA_CAPABLE
    // P4 SDMMC supports PSRAM, but its heap does not advertise MALLOC_CAP_DMA.
    // The dependency's SPIRAM | DMA request therefore always falls back to internal RAM.
    if (alignment != 0 && (alignment & (alignment - 1)) == 0) {
        void *buffer = heap_caps_aligned_alloc(alignment, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buffer != nullptr) return buffer;
    }
#endif
    return __real_eh_host_port_dma_alloc_aligned(size, alignment);
}
#endif
