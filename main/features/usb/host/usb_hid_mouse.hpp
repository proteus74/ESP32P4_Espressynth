#pragma once
#include <cstdint>

namespace Feature::Mouse {

struct Event {
    uint8_t buttons;    // bit0=left, bit1=right, bit2=middle
    int8_t dx;
    int8_t dy;
    int8_t wheel;
};

using EventCallback = void (*)(const Event& event);
using ConnectionCallback = void (*)(bool connected);

/**
 * @brief Register a callback invoked from the mouse task for every mouse report.
 *        Pass nullptr to unregister. Only one callback is stored at a time.
 */
void set_callback(EventCallback callback);

/**
 * @brief Register a callback invoked when a USB mouse is plugged in or unplugged.
 *        Pass nullptr to unregister. Only one callback is stored at a time.
 */
void set_connection_callback(ConnectionCallback callback);

/**
 * @brief True if at least one USB mouse is currently connected.
 */
bool is_connected();

} // namespace Feature::Mouse
