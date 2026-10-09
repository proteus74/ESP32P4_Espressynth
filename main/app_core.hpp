#pragma once

#include "freertos/FreeRTOS.h"

// Central core-affinity policy: project tasks use explicit affinity; app_core.cpp
// defaults unbound dependency tasks (including ESP-Hosted) to MAIN_CORE.
// IDF's per-core IPC and idle tasks retain their required affinity.
//
// AUDIO_CORE carries everything that moves or generates audio sample data: the onboard
// codec <-> USB Audio (UAC2) bridge tasks and the USB MIDI relay. MAIN_CORE carries
// everything else (Wi-Fi/BLE co-processor, storage, HID input, CLI, ...).
//
// One unavoidable exception: TinyUSB runs ONE shared task per direction (a single device
// task services every enabled USB Device class together -- CDC, MIDI, UAC2, ...; a single
// host task services every enabled USB Host class together -- HID, MIDI, ...). There is no
// per-class task to split further, so each of those two tasks is pinned as a whole:
//   - The USB Device task is pinned to AUDIO_CORE, since UAC2 speaker/mic sample I/O runs
//     directly inside its callbacks and audio dropouts are the more noticeable failure mode
//     than a little extra CDC/MIDI-out jitter.
//   - The USB Host task stays on MAIN_CORE, since HID keyboard/mouse polling and MIDI-in
//     byte parsing both happen there and keyboard/mouse responsiveness wins that trade-off;
//     MIDI input tolerates jitter far better than audio output does.
namespace App {

constexpr BaseType_t MAIN_CORE = 0;
constexpr BaseType_t AUDIO_CORE = 1;

} // namespace App
