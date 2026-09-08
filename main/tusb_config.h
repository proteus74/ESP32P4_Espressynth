#pragma once

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_TUSB_OS OPT_OS_FREERTOS
#define CFG_TUSB_DEBUG 1

#if CONFIG_APP_ENABLE_USB_DEVICE
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#else
#define CFG_TUSB_RHPORT0_MODE 0
#endif
#define CFG_TUSB_RHPORT1_MODE (OPT_MODE_HOST | OPT_MODE_HIGH_SPEED)
#define CFG_TUH_MAX_SPEED OPT_MODE_HIGH_SPEED

#define CFG_TUH_DWC2_SLAVE_ENABLE 1
#define CFG_TUH_DWC2_DMA_ENABLE 1
#if CONFIG_CACHE_L1_CACHE_LINE_SIZE
#define CFG_TUH_MEM_DCACHE_ENABLE 1
#define CFG_TUH_MEM_DCACHE_LINE_SIZE CONFIG_CACHE_L1_CACHE_LINE_SIZE
#define CFG_TUSB_MEM_SECTION __attribute__((aligned(CONFIG_CACHE_L1_CACHE_LINE_SIZE)))
#endif

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif

#define CFG_TUH_HUB 1
#define CFG_TUH_DEVICE_MAX (3 * CFG_TUH_HUB + 1)

#if CONFIG_USB_HOST_CLASS_MIDI
#define CFG_TUH_MIDI CFG_TUH_DEVICE_MAX
#else
#define CFG_TUH_MIDI 0
#endif
#define CFG_TUH_MIDI_RX_BUFSIZE 64
#define CFG_TUH_MIDI_TX_BUFSIZE 64
#define CFG_TUH_MIDI_EP_BUFSIZE 64

#if CONFIG_USB_HOST_CLASS_HID
#define CFG_TUH_HID CFG_TUH_DEVICE_MAX
#else
#define CFG_TUH_HID 0
#endif
#define CFG_TUH_HID_EPIN_BUFSIZE 64
#define CFG_TUH_HID_EPOUT_BUFSIZE 64
#define CFG_TUH_MSC 0
#define CFG_TUH_CDC 0
#define CFG_TUH_VENDOR 0

//--------------------------------------------------------------------+
// Device (USB1 / Serial-JTAG port as Custom Composite Device)
//--------------------------------------------------------------------+

#if CONFIG_APP_ENABLE_USB_DEVICE
#define CFG_TUD_ENABLED 1
#else
#define CFG_TUD_ENABLED 0
#endif

#if CONFIG_USB_DEV_ENABLE_CDC
#define CFG_TUD_CDC 1
#define CFG_TUD_CDC_RX_BUFSIZE 256
#define CFG_TUD_CDC_TX_BUFSIZE 256
#else
#define CFG_TUD_CDC 0
#endif

#if CONFIG_USB_DEV_ENABLE_MIDI
#define CFG_TUD_MIDI 1
#define CFG_TUD_MIDI_RX_BUFSIZE 256
#define CFG_TUD_MIDI_TX_BUFSIZE 256
#define CFG_TUD_MIDI_EP_BUFSIZE 64
#else
#define CFG_TUD_MIDI 0
#endif

#if CONFIG_USB_DEV_ENABLE_UAC2
// Fixed sample rate for both directions (project-wide, see Audio Subsystem Kconfig menu)
#define USB_DEV_UAC2_SAMPLE_RATE 44100
#define USB_DEV_UAC2_BYTES_PER_SAMPLE 2
#define USB_DEV_UAC2_BITS_PER_SAMPLE 16
#define USB_DEV_UAC2_CHANNELS 2
// Max ISO packet size for one 1ms Full-Speed frame (see TinyUSB's TUD_AUDIO_EP_SIZE macro;
// duplicated here since usbd.h isn't includable yet at this point in the build).
#define USB_DEV_UAC2_EP_SIZE (((USB_DEV_UAC2_SAMPLE_RATE + 999) / 1000 + 1) * \
                               USB_DEV_UAC2_BYTES_PER_SAMPLE * USB_DEV_UAC2_CHANNELS)

#define CFG_TUD_AUDIO 1
#define CFG_TUD_AUDIO_ENABLE_EP_IN 1   // Microphone (device -> host)
#define CFG_TUD_AUDIO_ENABLE_EP_OUT 1  // Speaker (host -> device)
#define CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ 64
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX USB_DEV_UAC2_EP_SIZE
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX USB_DEV_UAC2_EP_SIZE
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ (USB_DEV_UAC2_EP_SIZE * 32)
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ (USB_DEV_UAC2_EP_SIZE * 32)

// UAC2 descriptor length building blocks (fixed by USB Audio Class 2.0 spec; cannot use
// TinyUSB's TUD_AUDIO_DESC_*_LEN macros here since usbd.h isn't includable yet at this point).
// Layout: IAD + AC(std+header+clock) + 2x(input+feature+output terminal) + 2x AS interface
// (std alt0/alt1 + CS + format + iso EP + CS iso EP), one AS interface per direction.
#define USB_DEV_UAC2_LEN_IAD              8
#define USB_DEV_UAC2_LEN_STD_AC           9
#define USB_DEV_UAC2_LEN_CS_AC            9
#define USB_DEV_UAC2_LEN_CLK_SRC          8
#define USB_DEV_UAC2_LEN_INPUT_TERM       17
#define USB_DEV_UAC2_LEN_OUTPUT_TERM      12
#define USB_DEV_UAC2_LEN_FEATURE_UNIT_2CH (6 + (2 + 1) * 4)
#define USB_DEV_UAC2_LEN_STD_AS_INT       9
#define USB_DEV_UAC2_LEN_CS_AS_INT        16
#define USB_DEV_UAC2_LEN_TYPE_I_FORMAT    6
#define USB_DEV_UAC2_LEN_STD_AS_ISO_EP    7
#define USB_DEV_UAC2_LEN_CS_AS_ISO_EP     8
#define USB_DEV_UAC2_LEN_TERMINAL_CHAIN (USB_DEV_UAC2_LEN_INPUT_TERM + USB_DEV_UAC2_LEN_OUTPUT_TERM + \
                                          USB_DEV_UAC2_LEN_FEATURE_UNIT_2CH)
#define USB_DEV_UAC2_LEN_AS_ITF (USB_DEV_UAC2_LEN_STD_AS_INT * 2 + USB_DEV_UAC2_LEN_CS_AS_INT + \
                                  USB_DEV_UAC2_LEN_TYPE_I_FORMAT + USB_DEV_UAC2_LEN_STD_AS_ISO_EP + \
                                  USB_DEV_UAC2_LEN_CS_AS_ISO_EP)

#define CFG_TUD_AUDIO_FUNC_1_DESC_LEN (USB_DEV_UAC2_LEN_IAD + USB_DEV_UAC2_LEN_STD_AC + \
                                        USB_DEV_UAC2_LEN_CS_AC + USB_DEV_UAC2_LEN_CLK_SRC + \
                                        2 * USB_DEV_UAC2_LEN_TERMINAL_CHAIN + 2 * USB_DEV_UAC2_LEN_AS_ITF)
#else
#define CFG_TUD_AUDIO 0
#endif

#ifdef __cplusplus
}
#endif
