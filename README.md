# ESP32-P4 Template

This project is a modular, feature-rich ESP32-P4 application template built around the ESP-IDF framework. It is designed as a practical foundation for boards such as the JC8012P4A1, JC1060P470C 7-inch display board, the Waveshare ESP32-P4 Wi-Fi 6 development kit, and custom ESP32-P4 hardware.

The codebase is intentionally organized around feature modules instead of a single monolithic app. It combines Wi-Fi, Ethernet, Bluetooth, USB host/device functionality, audio codecs, LVGL display handling, filesystems, web serving, and a command-line interface into one reusable architecture.

This repository is especially useful when you want to prototype an ESP32-P4 product or a feature-rich embedded UI system without starting from scratch every time.

## At a glance

| Category | Summary |
| --- | --- |
| Platform | ESP32-P4 firmware template built with ESP-IDF |
| Supported boards | JC8012P4A1, JC1060P470C 7-inch, Waveshare ESP32-P4 Wi-Fi 6 Dev Kit, custom ESP32-P4 |
| Networking | Wi-Fi, Ethernet, ESP-Hosted co-processor support |
| USB | USB Host + USB Device via TinyUSB |
| Audio | ES8311, WM8960, USB Audio Class 2.0 |
| UI | LVGL display + touch support |
| Storage | LittleFS, SD card, web assets partition |
| Debug | Console CLI, system diagnostics, task info |
| Config | Modular Kconfig-based feature enable/disable |

### Supported boards

- JC8012P4A1
- JC1060P470C 7-inch board
- Waveshare ESP32-P4 Wi-Fi 6 Dev Kit
- Custom ESP32-P4 board definition

### Supported features

- Wi-Fi station mode and recovery logic
- Ethernet support
- Bluetooth LE via ESP-Hosted + NimBLE
- USB Host and USB Device via TinyUSB
- HID keyboard/mouse support
- MIDI host/device support
- ES8311 and WM8960 audio codec support
- USB audio class 2.0
- LVGL display and touch integration
- Web UI served from LittleFS
- SD card support
- filesystem utilities and CLI debug commands
- modular Kconfig-based feature selection

## Quick start

### 1) Set up ESP-IDF

```powershell
$env:IDF_PATH = 'C:\esp\v6.1-rc1\esp-idf'
. "$env:IDF_PATH\export.ps1"
```

### 2) Configure the project

```powershell
idf.py menuconfig
```

Select the target board, enable the features you need, and adjust audio, USB, Wi-Fi, display, and networking settings.

### 3) Build the firmware

```powershell
idf.py build
```

### 4) Flash the ESP32-P4

```powershell
idf.py -p COM16 flash
```

Replace `COM16` with your board's serial port.

### 5) Optional: flash the co-processor firmware

The Wi-Fi/BLE co-processor is an ESP32-C6. Build and stage it from the `coprocessor_fw` folder:

```powershell
cd coprocessor_fw
idf.py set-target esp32c6
idf.py build
coprocessor_fw\build_and_stage.bat COM16
```

This writes the co-processor image into the P4's `slave_fw` partition and prepares the host firmware to push it to the C6 at boot or via the OTA update flow.

---

## What this project does

At a high level, this firmware can:

- run on multiple ESP32-P4 board variants
- initialize board-specific peripherals and pin mappings
- drive display panels and touch input with LVGL
- support USB host and USB device stacks through TinyUSB
- interface with onboard audio codecs and USB audio devices
- connect to Wi-Fi networks or Ethernet
- use an ESP32-C6 hosted co-processor for Wi-Fi/BLE over SDIO
- expose a built-in CLI over UART, USB Serial/JTAG, or USB CDC
- serve static web content from LittleFS
- manage SD cards, storage, and network-aware app startup flows
- expose a modular structure where features can be enabled or disabled per Kconfig

It is not just a "hello world" project. It is meant to be a real embedded platform template with configurable software blocks and hardware support.

---

## Main capabilities

### 1. Board support and hardware abstraction

The project supports multiple target boards through board definitions and compile-time selection:

- JC8012P4A1
- JC1060P470C 7-inch board
- Waveshare ESP32-P4 Wi-Fi 6 Dev Kit
- Custom ESP32-P4 board definition

Board logic is centralized in the `main/boards` directory and selected through `Kconfig.projbuild`. This keeps board-specific GPIO mappings, display parameters, touch drivers, and peripheral wiring isolated from the application logic.

This is important because ESP32-P4 projects often require different display, touch, audio, and I2C mappings depending on the hardware vendor.

### 2. USB host and USB device support

The firmware uses TinyUSB and supports both directions of USB traffic:

#### USB device mode

Configured for USB device use cases such as:

- CDC serial device
- MIDI device
- USB audio class 2.0 (UAC2)
- HID keyboard, mouse, gamepad
- MSC / DFU / other optional TinyUSB classes

This is useful for turning the ESP32-P4 into a USB peripheral that a host PC or tablet can enumerate as a keyboard, MIDI interface, audio device, or serial terminal.

#### USB host mode

Configured for USB Host use cases such as:

- keyboard input from USB peripherals
- mouse input from USB peripherals
- MIDI input from USB MIDI devices
- hub-aware USB device detection and enumeration

The app keeps host and device functionality modular, which allows the same firmware to be reused for different product scenarios.

### 3. Audio subsystem

Audio is one of the most important areas of this project.

Supported audio paths include:

- onboard ES8311 audio codec
- generic WM8960 audio codec
- USB audio class 2.0 device support
- fixed 44.1 kHz audio configuration across the project

The app contains explicit logic for:

- I2S bus setup
- codec initialization
- microphone gain and output volume configuration
- pin validation to prevent board/resource conflicts
- optional audio button handling for WM8960 boards

#### WM8960 runtime control API

Unlike the ES8311 path (wrapped in `esp_codec_dev`), the WM8960 codec (`main/features/audio/wm8960_manager.hpp`) exposes its own small control API directly under `Feature::Audio::WM8960`, so any other part of the app (CLI commands, app logic, ...) can drive it without touching the vendored `jonlidgard/wm8960` library or its raw `wm8960_t` handle:

- `set_headphone_volume_db()` / `get_headphone_volume_db()` and `set_headphone_enabled()`
- `set_speaker_volume_db()` / `get_speaker_volume_db()` and `set_speaker_enabled()`
- `set_dac_mute()` and `set_dac_volume_db()` / `get_dac_volume_db()` (digital trim ahead of the DAC)
- `set_line_input_volume_db()` / `get_line_input_volume_db()` (PGA gain feeding the ADC) and `set_line_input_mute()`
- `set_mic_boost_gain_db()` / `get_mic_boost_gain_db()` (rounds to the WM8960's fixed 0/13/20/29 dB steps)
- `set_mic_bias_enabled()`
- `set_adc_volume_db()` / `get_adc_volume_db()` (digital trim after the ADC)
- `get_object()` for direct access to the raw handle for anything not covered above (ALC, 3D enhancement, loopback, PLL/clocking, ...) -- left unwrapped since those are tuned to this project's fixed 44.1 kHz setup and easy to misconfigure

The WM8960's control interface is I2C **write-only** (no register readback), so every `get_*()` here returns the last value this API successfully *commanded*, not something read back from the chip -- each is seeded with the same default `init()` applies, so a `get_*()` right after a clean boot already reflects reality.

#### Tuning audio output latency

Total output latency (the time between a sample/note being produced and it reaching the speaker) is the sum of two independent stages, each configurable in `idf.py menuconfig` under **Application Features → Audio Subsystem**:

**1. I2S DMA buffer** -- `AUDIO_I2S_DMA_DESC_NUM` × `AUDIO_I2S_DMA_FRAME_NUM`, shared by both onboard codecs (ES8311/WM8960; the USB UAC2 sound card buffers separately). The I2S peripheral streams audio out of RAM via DMA in a ring of `AUDIO_I2S_DMA_DESC_NUM` fixed-size chunks ("descriptors"), each holding `AUDIO_I2S_DMA_FRAME_NUM` frames (1 frame = 1 sample per channel, both channels at once -- 4 bytes at this project's fixed 16-bit stereo).

```
total_frames = AUDIO_I2S_DMA_DESC_NUM * AUDIO_I2S_DMA_FRAME_NUM
latency_ms   = total_frames / 44100 * 1000
```

Default 6 × 240 = 1440 frames ≈ **32.6 ms**. A bigger ring buys more headroom against a writer stall (e.g. the `tone` CLI command briefly starving the audio writer of CPU) before it audibly clicks/pops; a smaller ring trades that headroom for lower latency. `AUDIO_I2S_DMA_FRAME_NUM`'s upper range (960) stays under the I2S driver's hard 4095-byte per-descriptor ceiling (1023 frames at 4 bytes/frame).

**2. Synth engine render block** -- `AUDIO_SYNTH_BLOCK_SIZE` (`synth::kBlockSize` in `SynthConfig.h`, only relevant with the node-graph synth engine enabled). The synth's render task works in fixed-size blocks, not sample-by-sample: each loop iteration walks the whole node graph once to produce this many samples, then writes the block onward (to the codec mix and/or USB speaker mix).

```
block_time_ms = AUDIO_SYNTH_BLOCK_SIZE / 44100 * 1000
```

Default 128 samples ≈ **2.9 ms**. This is also the render task's real-time CPU budget per block (see `audioCoreLoadPercent()` / the CLI `top` audio-core load -- ≥100% means a block took longer to render than it takes to play, i.e. underrun risk): a MIDI note-on can only take effect at the start of the *next* block, so it waits on average half a block, up to a full block worst case, before it's heard. Smaller blocks lower that latency but raise overhead (node dispatch, control-rate smoothing, envelope/LFO updates all repeat more often per second of audio); larger blocks are the opposite.

**Total** is roughly the sum of both stages -- e.g. defaults give ~32.6 ms (I2S) + ~2.9 ms (synth) ≈ 35.5 ms from a rendered sample to sound.

This makes the firmware suitable for audio prototypes such as:

- sound cards
- USB audio bridges
- local media playback devices
- audio input/output test and debug systems

### 4. Wi-Fi, Ethernet, and ESP-Hosted networking

The project supports network connectivity in several ways:

- Wi-Fi STA mode with reconnection logic
- optional captive portal fallback in AP+STA mode
- automatic retry logic and reconnect recovery
- Ethernet support
- ESP-Hosted architecture for Wi-Fi/BLE over a co-processor
- co-processor OTA update support

A major part of the design is the use of an ESP32-C6 hosted co-processor over SDIO. This allows the ESP32-P4 to use a separate wireless controller for Wi-Fi and Bluetooth functionality while the main application runs on the P4 core.

### 5. Bluetooth LE support

The project includes BLE support using NimBLE and ESP-Hosted integration. It is set up for use cases including:

- BLE scanning
- device tracking
- connected peer management
- HID host features
- MIDI host support
- future BLE-based protocol integrations

This makes the project useful not only for wired USB interfaces but also for wireless HID and MIDI use cases in connected embedded hardware.

### 6. LVGL display and touch UI

The project contains a strong UI and display abstraction layer around LVGL.

This includes:

- MIPI DSI display initialization
- touch input handling
- hardware-accelerated or optimized frame flushing paths
- keyboard and mouse feeding into LVGL input devices
- board-specific display calibration routines

The display layer is designed so the application can host an actual screen UI without needing to manually rewire everything for each board.

### 7. Filesystem and web server

The project has filesystem support for:

- LittleFS data partition
- separate web-static partition for served assets
- static file HTTP server

The web server is designed to serve files out of a dedicated `www` partition, which keeps the UI assets separate from the main application filesystem and makes it easy to rebuild only the web content if desired.

This is useful for:

- configuration pages
- status dashboards
- embedded admin interfaces
- captive portal landing pages

### 8. CLI and debug interface

The code includes an interactive command-line interface based on `esp_console`.

Features include:

- help output
- memory info
- uptime and system info
- chip info
- top/htop-style task output
- Wi-Fi commands
- Ethernet commands
- ping support
- audio commands (volume/mute, plus a `tone` test generator that plays through whichever codec/I2S port is active, or over the USB Audio microphone stream)
- continuous I2C bus scanning (`i2c-scan <port>`, refreshes once a second until a key is pressed) -- alongside a one-shot scan logged at boot right after each I2C bus comes up (`main/boards/board_init.cpp`) -- both for chasing an intermittently-vanishing device on the bus
- USB commands
- BLE commands
- filesystem and disk usage commands

This is a very practical debug layer for embedded development and field test scenarios.

---

## Architecture overview

The project is organized around a layered system design:

- `main/boards` — board-specific GPIO, display, touch, and peripheral configuration
- `main/features` — functional modules (audio, USB, web, Wi-Fi, BLE, filesystem, etc.)
- `main/cli` — console commands and REPL layer
- `main/app_startup.hpp` — startup orchestration for the enabled subsystems
- `main/app_events.hpp` — event glue for keyboard, mouse, MIDI, CDC, and USB audio callbacks
- `main/main.cpp` — application entry point
- `main/Kconfig.projbuild` — configuration system for enabling/disabling features

The startup flow is intentionally modular:

- board initialization runs first
- USB device support can be started early
- network bring-up is started in a background task
- filesystem and web services are started based on enabled config flags
- audio, USB host, and other features are added conditionally

The app also defines a core-affinity strategy through `App::MAIN_CORE` and `App::AUDIO_CORE` to keep time-critical audio and USB data paths on the correct core.

---

## Project layout

```text
ESP32P4_Template/
├── CMakeLists.txt
├── partitions.csv
├── sdkconfig
├── sdkconfig.defaults
├── sdkconfig.defaults.custom_p4
├── sdkconfig.defaults.jc1060
├── sdkconfig.defaults.jc8012p4a1
├── sdkconfig.defaults.waveshare
├── data/
├── documentation/
├── coprocessor_fw/
├── components/
├── managed_components/
├── main/
│   ├── app_core.hpp
│   ├── app_events.hpp
│   ├── app_startup.hpp
│   ├── main.cpp
│   ├── Kconfig.projbuild
│   ├── tusb_config.h
│   ├── boards/
│   ├── cli/
│   ├── features/
│   └── idf_component.yml
└── README.md
```

---

## Hardware and software focus

This project is clearly aimed at advanced ESP32-P4 hardware and interactive embedded systems. It is better described as a flexible embedded platform template than as a simple example project.

The project is designed for cases such as:

- display-based control panels
- industrial or kiosk devices
- audio interfaces and USB gadgets
- wireless keyboards/mice or BLE device hosts
- networked embedded devices with a captive portal
- product prototypes with rapid feature toggling

---

## Build and configuration

This project is built with ESP-IDF.

### Typical setup on Windows

From the project root, use the ESP-IDF environment:

```powershell
$env:IDF_PATH = 'C:\esp\v6.1-rc1\esp-idf'
. "$env:IDF_PATH\export.ps1"
```

Then configure the project:

```powershell
idf.py menuconfig
```

Then build:

```powershell
idf.py build
```

Then flash:

```powershell
idf.py flash
```

You can also monitor the output:

```powershell
idf.py monitor
```

### Co-processor firmware flashing

This project can use an ESP32-C6 as a Wi-Fi/BLE co-processor over the ESP-Hosted SDIO link. The co-processor firmware is located in the separate project under [coprocessor_fw](coprocessor_fw).

#### 1) Build the co-processor firmware

```powershell
cd coprocessor_fw
idf.py set-target esp32c6
idf.py build
```

This produces a firmware image such as:

```text
coprocessor_fw/build/esp32p4_template_coprocessor.bin
```

#### 2) Stage it on the P4 host flash

This repo is set up so the C6 firmware is not flashed directly to the C6 in the usual way. Instead, the image is written into the ESP32-P4 host's `slave_fw` partition, and the P4 later pushes it to the C6 over the existing ESP-Hosted link.

Use the host serial port of the ESP32-P4:

```powershell
python -m esptool --chip esp32p4 -p COM16 write_flash --force 0x310000 coprocessor_fw\build\esp32p4_template_coprocessor.bin
```

The `--force` flag is intentional here: the staged file is a firmware image for the C6, not for the P4, so esptool would otherwise reject it as a chip mismatch. This is expected because the binary is only being stored on the P4 flash as a staged payload.

#### 3) Trigger the actual update on the C6

After the image is staged, reboot the ESP32-P4 or trigger the OTA flow from the app. The project contains a helper script for this:

```powershell
coprocessor_fw\build_and_stage.bat COM16
```

The script builds the C6 firmware and writes it to the P4 `slave_fw` partition automatically.

The host-side firmware then checks the C6 firmware version and, if needed, pushes the staged image over the existing SDIO/ESP-Hosted connection, activates it, and restarts the peer if necessary.

> In other words: the P4 acts as the transport and staging host, while the C6 is the actual Wi-Fi/BLE co-processor.

### Typical board configuration flow

From `menuconfig`, the following are commonly adjusted:

- selected board target
- audio subsystem enable/disable
- ES8311 or WM8960 codec configuration
- USB host/device classes
- Wi-Fi/ETH/BLE enablement
- LVGL display configuration
- storage options
- developer/debug features

The Kconfig system is the main control surface for customizing the firmware for a specific board or use case.

---

## Important design notes

### Core assignment strategy

The code intentionally assigns tasks to different cores:

- `MAIN_CORE` is used for general app logic, networking, HID input, display interaction, CLI, and non-audio tasks
- `AUDIO_CORE` is used for audio-intensive and USB audio flows

This reduces the chance of audio dropouts or USB timing issues interfering with other parts of the application.

### Separate web asset partition

The project uses a dedicated `www` LittleFS partition for static web files, rather than mixing those with the main application filesystem. This makes asset updates easier and avoids rebuilding the full app just to change a web page.

### Modular enable/disable system

Most major capabilities are off by default unless configured in `Kconfig`. This allows the same base project to evolve into different product variants without a large amount of code duplication.

---

## Typical use cases

This firmware is suitable for:

- ESP32-P4 UI prototypes with touch displays
- USB host keyboard/mouse interfaces
- USB audio devices or MIDI bridges
- Wi-Fi-connected control panels
- BLE HID/MIDI clients or hubs
- connected equipment with a simple management web UI
- platform-level starter firmware for an embedded product

---

## What makes this repository useful

The main value is that it is not a single-purpose demo. It combines several common embedded patterns into one coherent project skeleton:

- board abstraction
- modular feature enablement
- USB host/device stacks
- audio codec support
- display + input UI stack
- network connectivity
- debug CLI
- web assets and filesystem support

This makes it a good starting point for advanced ESP32-P4 work and experimentation.

---

## Why it is a good template

If you want to build a custom ESP32-P4 device quickly, this repo already contains a lot of the plumbing you usually have to write by hand:

- hardware selection logic
- GPIO and board config separation
- display and touch setup
- USB stack integration
- codec configuration
- Wi-Fi and BLE networking
- command-line access
- web UI hosting
- task/core organization

That is exactly the kind of groundwork that speeds up development of a real embedded product.

---

## Summary

This project is a comprehensive ESP32-P4 application template and firmware framework for advanced embedded development. It combines board abstraction, USB host/device support, audio processing, network connectivity, BLE, display UI, command-line debugging, and filesystem/web hosting into one coherent system.

It is built to be configurable, extensible, and practical for real-world embedded use instead of being just a minimal example.

If you are building an ESP32-P4 product, panel, sound interface, or developer platform, this repository gives you a strong base to extend and customize.
