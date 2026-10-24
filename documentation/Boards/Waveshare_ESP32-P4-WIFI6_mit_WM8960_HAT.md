# Waveshare ESP32-P4-WIFI6-DEV-KIT with WM8960 Audio HAT

Status: 2026-10-23. This page describes how the Waveshare board and the Waveshare **WM8960 Audio HAT**
(Raspberry Pi form factor) are wired together and which GPIOs are occupied in the process.

Sources:

- Board schematic: [WAVESHARE_ESP32-P4-WIFI6-DEV-KIT.pdf](WAVESHARE_ESP32-P4-WIFI6-DEV-KIT.pdf), block "Interface" (pin header `P6`)
- HAT schematic: [WM8960_Audio_HAT_Schematic.pdf](../AudioHats/WM8960_Audio_HAT_Schematic.pdf), pin header `P1`
- Board definition in the project: [waveshare_p4_wifi_dev_kit.hpp](../../main/boards/waveshare_p4_wifi_dev_kit.hpp)
- Codec driver: [wm8960_manager.hpp](../../main/features/audio/wm8960_manager.hpp)

## 1. The HAT on the 40-pin header

The 40-pin header `P6` of the board has the pin layout of a Raspberry Pi header. The HAT uses only the
following pins. The "Standard" column gives the ESP32-P4 GPIO that a directly plugged-in HAT connects to. The "Project" column shows what is set in this project's `sdkconfig`.

| HAT signal | Pin on `P6` | Raspberry Pi name | Standard (ESP32-P4) | Project (`sdkconfig`) |
| --- | --- | --- | --- | --- |
| I2C SDA | 3 | BCM2 | GPIO7 | GPIO7 (I2C0) |
| I2C SCL | 5 | BCM3 | GPIO8 | GPIO8 (I2C0) |
| I2S BCLK (`I2S_CLK`) | 12 | BCM18 | GPIO22 | GPIO22 |
| I2S LRCLK (`I2S_LRCLK`) | 35 | BCM19 | GPIO53 | GPIO53 |
| I2S ADC, microphone to ESP (`I2S_ADC`) | 38 | BCM20 | GPIO27 | **GPIO48** |
| I2S DAC, ESP to loudspeaker (`I2S_DAC`) | 40 | BCM21 | GPIO45 | **GPIO47** |
| Button `K1` | 11 | BCM17 | GPIO21 | off (`-1`) |
| 3.3 V | 1, 17 | | | |
| 5 V | 2, 4 | | | |
| GND | 6, 9, 14, 20, 25, 30, 34, 39 | | | |

The master clock (MCLK) does not come from the header. The HAT has its own 24 MHz crystal oscillator
(`XTAL1`). In the driver, `mclk` is therefore set to "not assigned".

**DIN and DOUT deviate from the standard.** The project uses GPIO48 (pin 33) and GPIO47 (pin 37),
not pins 38 and 40, where the HAT has its data lines. The reason is
that both standard pins are already occupied on this board:

- **GPIO27** (ADC data, pin 38) belongs together with GPIO26 to the USB device. The build check rejects GPIO27
  as long as `APP_ENABLE_USB_DEVICE` is on.
- **GPIO45** (DAC data, pin 40) is the power enable of the SD card (`PIN_SDMMC_PWR_EN`). It is
  reserved as long as `APP_ENABLE_SDCARD` is on.

Therefore the HAT lines ADC (pin 38) and DAC (pin 40) are routed with jumper wires to pin 33 (GPIO48) and
pin 37 (GPIO47). This is how the setup was actually built (confirmed). If you plug the HAT in directly,
you must reset the two pins in `menuconfig` to 27 and 45 and disable USB device and SD card.

### Properties of the HAT

- Codec WM8960, I2C address `0x1A` (7 bit), I2C bus 0.
- Two analog MEMS microphones (AOS3729A-T42) on `LINPUT1` and `RINPUT1`, with microphone bias (`MICBIAS`).
- 3.5 mm headphone jack and loudspeaker output (class D, four pins `LP`, `LN`, `RN`, `RP`).
- The ADC settings (input gain, microphone boost, ADC volume, bias) can be adjusted in the widget
  "WM8960-In" via double-click, see [wm8960-in.html](../../data/help/widgets/wm8960-in.html).

## 2. Settings for this combination

`idf.py menuconfig`, board "Waveshare ESP32-P4 WiFi Dev Kit":

| Entry | Value |
| --- | --- |
| `ONBOARD_AUDIO_WM8960` | on |
| `ONBOARD_AUDIO_WM8960_I2S_PORT` | 1 |
| `ONBOARD_AUDIO_WM8960_I2C_PORT` | 0 |
| `ONBOARD_AUDIO_WM8960_PIN_BCLK` | 22 |
| `ONBOARD_AUDIO_WM8960_PIN_LRCK` | 53 |
| `ONBOARD_AUDIO_WM8960_PIN_DIN` | 48 |
| `ONBOARD_AUDIO_WM8960_PIN_DOUT` | 47 |
| `ONBOARD_AUDIO_WM8960_PIN_BUTTON` | -1 (for button `K1`: 21) |
| `ONBOARD_AUDIO_ES8311` | **off** (see below) |
| `I2C0_PIN_SDA` / `I2C0_PIN_SCL` | 7 / 8 |

**The onboard codec ES8311 and the HAT are mutually exclusive.** The amplifier control pin of the ES8311 is
GPIO53, and that is the LRCLK line of the HAT.

Button `K1` is connected to GND and has a 4.7 kΩ pull-up to 3.3 V. It is active low.

## 3. What else is occupied on the board

These GPIOs are marked as occupied in the board definition. The HAT and your own functions must not
use them. The build check reports a collision with pin number and name.

| Function | GPIOs |
| --- | --- |
| ES8311 (I2S + clock) | 9, 10, 11, 12, 13 |
| Amplifier of the ES8311 (only with ES8311 on) | 53 |
| ESP32-C6 via SDIO (ESP-Hosted) | 14, 15, 16, 17, 18, 19 |
| ESP32-C6 reset / wake | 54 / 6 |
| SD card (SDMMC) | 39, 40, 41, 42, 43, 44 |
| SD card power enable (only with SD card on) | 45 |
| Ethernet (RMII and management) | 28, 29, 30, 31, 34, 35, 49, 50, 51, 52 |
| USB device (project rule) | 26, 27 |
| I2C0 (project) | 7, 8 |

Also on the 40-pin header are GPIO24 and GPIO25 (pins 27 and 28). They are connected via 0 Ω resistors
to the USB lines `USB1P1_N` and `USB1P1_P` of the board.

Free on the header (in this configuration, no guarantee against additional assignments by other
functions): GPIO0, 1, 2, 3, 4, 5, 20, 21, 23, 32, 33, 36, 37, 38, 46.

## 4. The 40-pin header `P6` in full

Read from the board schematic. The "Project" column shows the current use.

| Pin | GPIO | Pi name | Project | | Pin | GPIO | Pi name | Project |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 3.3 V | | | | 2 | 5 V | | |
| 3 | GPIO7 | SDA | I2C0 SDA | | 4 | 5 V | | |
| 5 | GPIO8 | SCL | I2C0 SCL | | 6 | GND | | |
| 7 | GPIO23 | BCM4 | free | | 8 | GPIO37 | TXD | free |
| 9 | GND | | | | 10 | GPIO38 | RXD | free |
| 11 | GPIO21 | BCM17 | free (HAT button) | | 12 | GPIO22 | BCM18 | WM8960 BCLK |
| 13 | GPIO20 | BCM27 | free | | 14 | GND | | |
| 15 | GPIO6 | BCM22 | ESP32-C6 wake | | 16 | GPIO5 | BCM23 | free |
| 17 | 3.3 V | | | | 18 | GPIO4 | BCM24 | free |
| 19 | GPIO3 | MOSI | free | | 20 | GND | | |
| 21 | GPIO2 | MISO | free | | 22 | GPIO1 | BCM25 | free |
| 23 | GPIO0 | SCLK | free | | 24 | GPIO36 | CE0 | free |
| 25 | GND | | | | 26 | GPIO32 | CE1 | free |
| 27 | GPIO24 | ID_SD | USB (board) | | 28 | GPIO25 | ID_SC | USB (board) |
| 29 | GPIO33 | BCM5 | free | | 30 | GND | | |
| 31 | GPIO26 | BCM6 | USB device | | 32 | GPIO54 | BCM12 | ESP32-C6 reset |
| 33 | GPIO48 | BCM13 | WM8960 DIN (project) | | 34 | GND | | |
| 35 | GPIO53 | BCM19 | WM8960 LRCK | | 36 | GPIO46 | BCM16 | free |
| 37 | GPIO47 | BCM26 | WM8960 DOUT (project) | | 38 | GPIO27 | BCM20 | USB device |
| 39 | GND | | | | 40 | GPIO45 | BCM21 | SD card power |

## 5. What has been verified

- **Schematics:** The pin assignment comes from the two PDFs. It matches the Raspberry Pi pin layout
  and the pins the project already runs with (I2C 7/8, BCLK 22, LRCK 53).
- **On the device (2026-10-23):** The codec reports itself after startup ("WM8960 initialized on I2S1, BCLK 22,
  LRCK 53, DOUT 47, DIN 48"). The widget "WM8960-In" delivers microphone data via GPIO48. A 1 kHz tone
  from a PC loudspeaker was recorded on both channels. The gain settings in the widget reach the codec
  via I2C, and the noise level follows them.
- **Not verified:** The output via GPIO47 (DAC data) and button `K1`.
