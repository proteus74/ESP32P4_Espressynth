#pragma once
#include <cstddef>
#include <cstdint>

namespace Feature::IO {

// Two buses, each with four ADS1115 and eight GP8413 addresses.
constexpr size_t MAX_DETECTED_I2C_DEVICES = 24;

struct DetectedI2CDevice {
    char name[16] = {};   // "ADS1115" / "GP8413"
    uint8_t bus_port = 0; // 0 or 1, matches Board::get_i2c_bus_handle()
    uint8_t address = 0;  // 7-bit I2C address
};

namespace I2CDeviceRegistry {
void clear();
void add(const char* name, uint8_t bus_port, uint8_t address);
size_t count();
// Returns false (leaves out untouched) if index >= count().
bool get(size_t index, DetectedI2CDevice& out);
} // namespace I2CDeviceRegistry

} // namespace Feature::IO
