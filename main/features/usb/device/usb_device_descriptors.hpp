#pragma once

#include "usb_device_audio.hpp"
#include "usb_device_interfaces.hpp"
#include "tusb.h"
#include "device/usbd.h"
#include "class/audio/audio.h"
#include <cstring>

//--------------------------------------------------------------------+
// Endpoint numbering (fixed slot per class regardless of which are enabled)
//--------------------------------------------------------------------+

enum {
    EPNUM_CDC_NOTIF = 0x81,
    EPNUM_CDC_OUT   = 0x02,
    EPNUM_CDC_IN    = 0x82,
    EPNUM_MIDI_OUT  = 0x03,
    EPNUM_MIDI_IN   = 0x83,
    EPNUM_AUDIO_OUT = 0x04,
    EPNUM_AUDIO_IN  = 0x84,
};

constexpr uint16_t CONFIG_TOTAL_LEN = TUD_CONFIG_DESC_LEN
#if CONFIG_USB_DEV_ENABLE_CDC
    + TUD_CDC_DESC_LEN
#endif
#if CONFIG_USB_DEV_ENABLE_MIDI
    + TUD_MIDI_DESC_LEN
#endif
#if CONFIG_USB_DEV_ENABLE_UAC2
    + CFG_TUD_AUDIO_FUNC_1_DESC_LEN
#endif
    ;

//--------------------------------------------------------------------+
// Device Descriptor
//--------------------------------------------------------------------+

inline const tusb_desc_device_t device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    // Composite device: class/subclass/protocol at the device level signal "look at each
    // Interface Association Descriptor" rather than a single fixed class.
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = CONFIG_USB_DEV_VID,
    .idProduct = CONFIG_USB_DEV_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+

inline const uint8_t config_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, UsbDeviceItf::TOTAL, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_SELF_POWERED, 100),

#if CONFIG_USB_DEV_ENABLE_CDC
    TUD_CDC_DESCRIPTOR(UsbDeviceItf::CDC_CTRL, 0, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
#endif

#if CONFIG_USB_DEV_ENABLE_MIDI
    TUD_MIDI_DESCRIPTOR(UsbDeviceItf::MIDI_CTRL, 0, EPNUM_MIDI_OUT, EPNUM_MIDI_IN, 64),
#endif

#if CONFIG_USB_DEV_ENABLE_UAC2
    // Interface Association: AC + AS(speaker) + AS(mic)
    TUD_AUDIO_DESC_IAD(UsbDeviceItf::AUDIO_CTRL, 0x03, 0),
    // Standard + Class-Specific AC Interface
    TUD_AUDIO_DESC_STD_AC(UsbDeviceItf::AUDIO_CTRL, 0x00, 0),
    TUD_AUDIO_DESC_CS_AC(0x0200, AUDIO_FUNC_DESKTOP_SPEAKER,
                         USB_DEV_UAC2_LEN_CLK_SRC + 2 * USB_DEV_UAC2_LEN_TERMINAL_CHAIN, AUDIO_CTRL_NONE),
    // Shared clock source, fixed sample rate
    TUD_AUDIO_DESC_CLK_SRC(Feature::USBDevice::Audio::ENTITY_CLOCK, AUDIO_CLOCK_SOURCE_ATT_INT_FIX_CLK,
                           (AUDIO_CTRL_R << AUDIO_CLOCK_SOURCE_CTRL_CLK_FRQ_POS), 0x00, 0x00),
    // Speaker path: USB Streaming -> Feature Unit -> Desktop Speaker
    TUD_AUDIO_DESC_INPUT_TERM(Feature::USBDevice::Audio::ENTITY_SPK_INPUT_TERM, AUDIO_TERM_TYPE_USB_STREAMING,
                              0x00, Feature::USBDevice::Audio::ENTITY_CLOCK, 0x02,
                              AUDIO_CHANNEL_CONFIG_NON_PREDEFINED, 0x00, AUDIO_CTRL_NONE, 0x00),
    TUD_AUDIO_DESC_FEATURE_UNIT_TWO_CHANNEL(
        Feature::USBDevice::Audio::ENTITY_SPK_FEATURE, Feature::USBDevice::Audio::ENTITY_SPK_INPUT_TERM,
        (AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_MUTE_POS) | (AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_VOLUME_POS),
        (AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_MUTE_POS) | (AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_VOLUME_POS),
        (AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_MUTE_POS) | (AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_VOLUME_POS),
        0x00),
    TUD_AUDIO_DESC_OUTPUT_TERM(Feature::USBDevice::Audio::ENTITY_SPK_OUTPUT_TERM, AUDIO_TERM_TYPE_OUT_DESKTOP_SPEAKER,
                               0x00, Feature::USBDevice::Audio::ENTITY_SPK_FEATURE,
                               Feature::USBDevice::Audio::ENTITY_CLOCK, AUDIO_CTRL_NONE, 0x00),
    // Mic path: Generic Mic -> Feature Unit -> USB Streaming
    TUD_AUDIO_DESC_INPUT_TERM(Feature::USBDevice::Audio::ENTITY_MIC_INPUT_TERM, AUDIO_TERM_TYPE_IN_GENERIC_MIC,
                              0x00, Feature::USBDevice::Audio::ENTITY_CLOCK, 0x02,
                              AUDIO_CHANNEL_CONFIG_NON_PREDEFINED, 0x00, AUDIO_CTRL_NONE, 0x00),
    TUD_AUDIO_DESC_FEATURE_UNIT_TWO_CHANNEL(
        Feature::USBDevice::Audio::ENTITY_MIC_FEATURE, Feature::USBDevice::Audio::ENTITY_MIC_INPUT_TERM,
        (AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_MUTE_POS) | (AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_VOLUME_POS),
        (AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_MUTE_POS) | (AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_VOLUME_POS),
        (AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_MUTE_POS) | (AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_VOLUME_POS),
        0x00),
    TUD_AUDIO_DESC_OUTPUT_TERM(Feature::USBDevice::Audio::ENTITY_MIC_OUTPUT_TERM, AUDIO_TERM_TYPE_USB_STREAMING,
                               0x00, Feature::USBDevice::Audio::ENTITY_MIC_FEATURE,
                               Feature::USBDevice::Audio::ENTITY_CLOCK, AUDIO_CTRL_NONE, 0x00),
    // AS Interface: Speaker (host -> device, OUT)
    TUD_AUDIO_DESC_STD_AS_INT((uint8_t)(UsbDeviceItf::AUDIO_STREAM_SPK), 0x00, 0x00, 0x00),
    TUD_AUDIO_DESC_STD_AS_INT((uint8_t)(UsbDeviceItf::AUDIO_STREAM_SPK), 0x01, 0x01, 0x00),
    TUD_AUDIO_DESC_CS_AS_INT(Feature::USBDevice::Audio::ENTITY_SPK_INPUT_TERM, AUDIO_CTRL_NONE, AUDIO_FORMAT_TYPE_I,
                             AUDIO_DATA_FORMAT_TYPE_I_PCM, 0x02, AUDIO_CHANNEL_CONFIG_NON_PREDEFINED, 0x00),
    TUD_AUDIO_DESC_TYPE_I_FORMAT(USB_DEV_UAC2_BYTES_PER_SAMPLE, USB_DEV_UAC2_BITS_PER_SAMPLE),
    TUD_AUDIO_DESC_STD_AS_ISO_EP(EPNUM_AUDIO_OUT,
                                 (uint8_t)((uint8_t)TUSB_XFER_ISOCHRONOUS | (uint8_t)TUSB_ISO_EP_ATT_ADAPTIVE |
                                           (uint8_t)TUSB_ISO_EP_ATT_DATA),
                                 USB_DEV_UAC2_EP_SIZE, 0x01),
    TUD_AUDIO_DESC_CS_AS_ISO_EP(AUDIO_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, AUDIO_CTRL_NONE,
                                AUDIO_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0x0000),
    // AS Interface: Microphone (device -> host, IN)
    TUD_AUDIO_DESC_STD_AS_INT((uint8_t)(UsbDeviceItf::AUDIO_STREAM_MIC), 0x00, 0x00, 0x00),
    TUD_AUDIO_DESC_STD_AS_INT((uint8_t)(UsbDeviceItf::AUDIO_STREAM_MIC), 0x01, 0x01, 0x00),
    TUD_AUDIO_DESC_CS_AS_INT(Feature::USBDevice::Audio::ENTITY_MIC_OUTPUT_TERM, AUDIO_CTRL_NONE, AUDIO_FORMAT_TYPE_I,
                             AUDIO_DATA_FORMAT_TYPE_I_PCM, 0x02, AUDIO_CHANNEL_CONFIG_NON_PREDEFINED, 0x00),
    TUD_AUDIO_DESC_TYPE_I_FORMAT(USB_DEV_UAC2_BYTES_PER_SAMPLE, USB_DEV_UAC2_BITS_PER_SAMPLE),
    TUD_AUDIO_DESC_STD_AS_ISO_EP(EPNUM_AUDIO_IN,
                                 (uint8_t)((uint8_t)TUSB_XFER_ISOCHRONOUS | (uint8_t)TUSB_ISO_EP_ATT_ASYNCHRONOUS |
                                           (uint8_t)TUSB_ISO_EP_ATT_DATA),
                                 USB_DEV_UAC2_EP_SIZE, 0x01),
    TUD_AUDIO_DESC_CS_AS_ISO_EP(AUDIO_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, AUDIO_CTRL_NONE,
                                AUDIO_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0x0000),
#endif
};

static_assert(sizeof(config_descriptor) == CONFIG_TOTAL_LEN,
              "config_descriptor size does not match its declared wTotalLength");

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+

// Index 0 (language ID) is handled specially in tud_descriptor_string_cb; this table starts at 1.
inline const char* const string_descriptor_table[] = {
    nullptr,
    CONFIG_USB_DEV_MANUFACTURER,         // 1: Manufacturer
    CONFIG_USB_DEV_PRODUCT_NAME,         // 2: Product
    CONFIG_USB_DEV_SERIAL_NUMBER,        // 3: Serial
};

inline uint16_t string_descriptor_buf[32];

extern "C" __attribute__((used)) uint8_t const* tud_descriptor_device_cb(void) {
    return reinterpret_cast<const uint8_t*>(&device_descriptor);
}

extern "C" __attribute__((used)) uint8_t const* tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return config_descriptor;
}

extern "C" __attribute__((used)) uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;

    size_t char_count;
    if (index == 0) {
        string_descriptor_buf[1] = 0x0409;  // English (US)
        char_count = 1;
    } else {
        if (index >= sizeof(string_descriptor_table) / sizeof(string_descriptor_table[0])) {
            return nullptr;
        }
        const char* str = string_descriptor_table[index];
        const size_t max_chars = (sizeof(string_descriptor_buf) / sizeof(string_descriptor_buf[0])) - 1;
        char_count = std::strlen(str);
        if (char_count > max_chars) char_count = max_chars;
        for (size_t i = 0; i < char_count; ++i) {
            string_descriptor_buf[1 + i] = static_cast<uint16_t>(str[i]);
        }
    }
    string_descriptor_buf[0] = static_cast<uint16_t>((TUSB_DESC_STRING << 8) | (2 * (char_count + 1)));
    return string_descriptor_buf;
}
