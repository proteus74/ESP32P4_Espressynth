#include "i2c_device_registry.h"
#include "freertos/FreeRTOS.h"
#include <cstring>

namespace {
portMUX_TYPE gMux = portMUX_INITIALIZER_UNLOCKED;
Feature::IO::DetectedI2CDevice gDevices[Feature::IO::MAX_DETECTED_I2C_DEVICES];
size_t gCount = 0;
} // namespace

namespace Feature::IO::I2CDeviceRegistry {

void clear() {
    portENTER_CRITICAL(&gMux);
    gCount = 0;
    portEXIT_CRITICAL(&gMux);
}

void add(const char* name, uint8_t bus_port, uint8_t address) {
    portENTER_CRITICAL(&gMux);
    if (gCount < MAX_DETECTED_I2C_DEVICES) {
        DetectedI2CDevice& d = gDevices[gCount++];
        std::strncpy(d.name, name, sizeof(d.name) - 1);
        d.bus_port = bus_port;
        d.address = address;
    }
    portEXIT_CRITICAL(&gMux);
}

size_t count() {
    portENTER_CRITICAL(&gMux);
    const size_t c = gCount;
    portEXIT_CRITICAL(&gMux);
    return c;
}

bool get(size_t index, DetectedI2CDevice& out) {
    portENTER_CRITICAL(&gMux);
    bool ok = index < gCount;
    if (ok) out = gDevices[index];
    portEXIT_CRITICAL(&gMux);
    return ok;
}

} // namespace Feature::IO::I2CDeviceRegistry
