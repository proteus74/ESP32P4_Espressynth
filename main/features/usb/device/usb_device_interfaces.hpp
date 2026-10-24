#pragma once
#include "sdkconfig.h"

// USB interface numbering for the composite device, shared between the descriptor builder
// (usb_device_descriptors.cpp) and any class implementation that needs to recognize its own
// raw USB interface number (currently only the UAC2 audio callbacks). Numbering auto-adjusts
// to whichever classes are enabled in Kconfig, so this must stay the single source of truth.

namespace UsbDeviceItf {

enum {
#if CONFIG_USB_DEV_ENABLE_CDC
    CDC_CTRL,
    CDC_DATA,
#endif
#if CONFIG_USB_DEV_ENABLE_MIDI
    MIDI_CTRL,
    MIDI_STREAM,
#endif
#if CONFIG_USB_DEV_ENABLE_UAC2
    AUDIO_CTRL,
    AUDIO_STREAM_SPK,
    AUDIO_STREAM_MIC,
#endif
    TOTAL
};

} // namespace UsbDeviceItf
