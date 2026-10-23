# Waveshare ESP32-P4-WIFI6-DEV-KIT mit WM8960 Audio HAT

Stand: 2026-10-23. Diese Seite beschreibt, wie das Waveshare-Board und das Waveshare **WM8960 Audio HAT**
(Raspberry-Pi-Bauform) zusammen verdrahtet sind und welche GPIOs dabei belegt werden.

Quellen:

- Schaltplan des Boards: [WAVESHARE_ESP32-P4-WIFI6-DEV-KIT.pdf](WAVESHARE_ESP32-P4-WIFI6-DEV-KIT.pdf), Block "Interface" (Stiftleiste `P6`)
- Schaltplan des HATs: [WM8960_Audio_HAT_Schematic.pdf](../AudioHats/WM8960_Audio_HAT_Schematic.pdf), Stiftleiste `P1`
- Board-Definition im Projekt: [waveshare_p4_wifi_dev_kit.hpp](../../main/boards/waveshare_p4_wifi_dev_kit.hpp)
- Codec-Treiber: [wm8960_manager.hpp](../../main/features/audio/wm8960_manager.hpp)

## 1. Das HAT auf der 40-Pin-Leiste

Die 40-Pin-Leiste `P6` des Boards hat die Belegung einer Raspberry Pi Leiste. Das HAT nutzt davon nur
die folgenden Pins. In der Spalte "Standard" steht der GPIO des ESP32-P4, auf den ein direkt
aufgestecktes HAT trifft. In der Spalte "Projekt" steht, was in der `sdkconfig` dieses Projekts eingestellt ist.

| HAT-Signal | Pin auf `P6` | Raspberry-Pi-Name | Standard (ESP32-P4) | Projekt (`sdkconfig`) |
| --- | --- | --- | --- | --- |
| I2C SDA | 3 | BCM2 | GPIO7 | GPIO7 (I2C0) |
| I2C SCL | 5 | BCM3 | GPIO8 | GPIO8 (I2C0) |
| I2S BCLK (`I2S_CLK`) | 12 | BCM18 | GPIO22 | GPIO22 |
| I2S LRCLK (`I2S_LRCLK`) | 35 | BCM19 | GPIO53 | GPIO53 |
| I2S ADC, Mikrofon zum ESP (`I2S_ADC`) | 38 | BCM20 | GPIO27 | **GPIO48** |
| I2S DAC, ESP zum Lautsprecher (`I2S_DAC`) | 40 | BCM21 | GPIO45 | **GPIO47** |
| Taster `K1` | 11 | BCM17 | GPIO21 | aus (`-1`) |
| 3,3 V | 1, 17 | | | |
| 5 V | 2, 4 | | | |
| GND | 6, 9, 14, 20, 25, 30, 34, 39 | | | |

Der Master-Takt (MCLK) kommt nicht von der Leiste. Das HAT hat einen eigenen 24-MHz-Quarzoszillator
(`XTAL1`). Im Treiber ist `mclk` deshalb auf "nicht belegt" gesetzt.

**DIN und DOUT weichen vom Standard ab.** Im Projekt sind GPIO48 (Pin 33) und GPIO47 (Pin 37)
konfiguriert, also nicht die Pins 38 und 40, an denen das HAT seine Datenleitungen hat. Der Grund ist,
dass beide Standard-Pins auf diesem Board schon belegt sind:

- **GPIO27** (ADC-Daten, Pin 38) gehört mit GPIO26 zum USB-Device. Die Prüfung beim Bauen lehnt GPIO27 ab,
  solange `APP_ENABLE_USB_DEVICE` an ist.
- **GPIO45** (DAC-Daten, Pin 40) ist die Stromfreigabe der SD-Karte (`PIN_SDMMC_PWR_EN`). Sie ist
  reserviert, solange `APP_ENABLE_SDCARD` an ist.

Deshalb sind die HAT-Leitungen ADC (Pin 38) und DAC (Pin 40) mit Drahtbrücken auf Pin 33 (GPIO48) und
Pin 37 (GPIO47) geführt. So ist der Aufbau tatsächlich gebaut (bestätigt). Wer das HAT direkt aufsteckt,
muss die beiden Pins in `menuconfig` auf 27 und 45 zurücksetzen und dafür USB-Device und SD-Karte abschalten.

### Eigenschaften des HATs

- Codec WM8960, I2C-Adresse `0x1A` (7 Bit), I2C-Bus 0.
- Zwei analoge MEMS-Mikrofone (AOS3729A-T42) an `LINPUT1` und `RINPUT1`, mit Mikrofon-Bias (`MICBIAS`).
- Kopfhörerbuchse 3,5 mm sowie Lautsprecherausgang (Klasse D, vier Pins `LP`, `LN`, `RN`, `RP`).
- Die ADC-Einstellungen (Eingangsverstärkung, Mikrofon-Boost, ADC-Lautstärke, Bias) lassen sich im Widget
  "WM8960-In" per Doppelklick einstellen, siehe [wm8960-in.html](../../data/help/widgets/wm8960-in.html).

## 2. Einstellungen für diese Kombination

`idf.py menuconfig`, Board "Waveshare ESP32-P4 WiFi Dev Kit":

| Eintrag | Wert |
| --- | --- |
| `ONBOARD_AUDIO_WM8960` | an |
| `ONBOARD_AUDIO_WM8960_I2S_PORT` | 1 |
| `ONBOARD_AUDIO_WM8960_I2C_PORT` | 0 |
| `ONBOARD_AUDIO_WM8960_PIN_BCLK` | 22 |
| `ONBOARD_AUDIO_WM8960_PIN_LRCK` | 53 |
| `ONBOARD_AUDIO_WM8960_PIN_DIN` | 48 |
| `ONBOARD_AUDIO_WM8960_PIN_DOUT` | 47 |
| `ONBOARD_AUDIO_WM8960_PIN_BUTTON` | -1 (für den Taster `K1`: 21) |
| `ONBOARD_AUDIO_ES8311` | **aus** (siehe unten) |
| `I2C0_PIN_SDA` / `I2C0_PIN_SCL` | 7 / 8 |

**Der Onboard-Codec ES8311 und das HAT schließen sich aus.** Der Verstärker-Steuerpin des ES8311 ist
GPIO53, und das ist die LRCLK-Leitung des HATs.

Der Taster `K1` liegt gegen GND und hat einen 4,7-kΩ-Pull-up nach 3,3 V. Er ist aktiv bei Low.

## 3. Was sonst auf dem Board belegt ist

Diese GPIOs sind in der Board-Definition als belegt eingetragen. Das HAT und eigene Funktionen dürfen
sie nicht nutzen. Die Bauprüfung meldet eine Kollision mit Pinnummer und Namen.

| Funktion | GPIOs |
| --- | --- |
| ES8311 (I2S + Takt) | 9, 10, 11, 12, 13 |
| Verstärker des ES8311 (nur mit ES8311 an) | 53 |
| ESP32-C6 über SDIO (ESP-Hosted) | 14, 15, 16, 17, 18, 19 |
| ESP32-C6 Reset / Wake | 54 / 6 |
| SD-Karte (SDMMC) | 39, 40, 41, 42, 43, 44 |
| SD-Karte Stromfreigabe (nur mit SD-Karte an) | 45 |
| Ethernet (RMII und Verwaltung) | 28, 29, 30, 31, 34, 35, 49, 50, 51, 52 |
| USB-Device (Projektregel) | 26, 27 |
| I2C0 (Projekt) | 7, 8 |

An der 40-Pin-Leiste liegen außerdem GPIO24 und GPIO25 (Pins 27 und 28). Sie sind über 0-Ω-Widerstände
mit den USB-Leitungen `USB1P1_N` und `USB1P1_P` des Boards verbunden.

Frei an der Leiste (in dieser Konfiguration, ohne Gewähr für Zusatzbelegungen durch weitere
Funktionen): GPIO0, 1, 2, 3, 4, 5, 20, 21, 23, 32, 33, 36, 37, 38, 46.

## 4. Die 40-Pin-Leiste `P6` komplett

Aus dem Schaltplan des Boards abgelesen. Die Spalte "Projekt" zeigt die aktuelle Nutzung.

| Pin | GPIO | Pi-Name | Projekt | | Pin | GPIO | Pi-Name | Projekt |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 3,3 V | | | | 2 | 5 V | | |
| 3 | GPIO7 | SDA | I2C0 SDA | | 4 | 5 V | | |
| 5 | GPIO8 | SCL | I2C0 SCL | | 6 | GND | | |
| 7 | GPIO23 | BCM4 | frei | | 8 | GPIO37 | TXD | frei |
| 9 | GND | | | | 10 | GPIO38 | RXD | frei |
| 11 | GPIO21 | BCM17 | frei (HAT-Taster) | | 12 | GPIO22 | BCM18 | WM8960 BCLK |
| 13 | GPIO20 | BCM27 | frei | | 14 | GND | | |
| 15 | GPIO6 | BCM22 | ESP32-C6 Wake | | 16 | GPIO5 | BCM23 | frei |
| 17 | 3,3 V | | | | 18 | GPIO4 | BCM24 | frei |
| 19 | GPIO3 | MOSI | frei | | 20 | GND | | |
| 21 | GPIO2 | MISO | frei | | 22 | GPIO1 | BCM25 | frei |
| 23 | GPIO0 | SCLK | frei | | 24 | GPIO36 | CE0 | frei |
| 25 | GND | | | | 26 | GPIO32 | CE1 | frei |
| 27 | GPIO24 | ID_SD | USB (Board) | | 28 | GPIO25 | ID_SC | USB (Board) |
| 29 | GPIO33 | BCM5 | frei | | 30 | GND | | |
| 31 | GPIO26 | BCM6 | USB-Device | | 32 | GPIO54 | BCM12 | ESP32-C6 Reset |
| 33 | GPIO48 | BCM13 | WM8960 DIN (Projekt) | | 34 | GND | | |
| 35 | GPIO53 | BCM19 | WM8960 LRCK | | 36 | GPIO46 | BCM16 | frei |
| 37 | GPIO47 | BCM26 | WM8960 DOUT (Projekt) | | 38 | GPIO27 | BCM20 | USB-Device |
| 39 | GND | | | | 40 | GPIO45 | BCM21 | SD-Karte Strom |

## 5. Was geprüft ist

- **Schaltpläne:** Die Zuordnung der Pins stammt aus den beiden PDFs. Sie passt zur Raspberry-Pi-Belegung
  und zu den Pins, mit denen das Projekt bereits läuft (I2C 7/8, BCLK 22, LRCK 53).
- **Am Gerät (2026-10-23):** Der Codec meldet sich nach dem Start ("WM8960 initialized on I2S1, BCLK 22,
  LRCK 53, DOUT 47, DIN 48"). Das Widget "WM8960-In" liefert Mikrofondaten über GPIO48. Ein 1-kHz-Ton
  vom PC-Lautsprecher wurde auf beiden Kanälen aufgenommen. Die Gain-Einstellungen im Widget gelangen
  per I2C zum Codec, der Rauschpegel folgt ihnen.
- **Nicht geprüft:** Der Ausgang über GPIO47 (DAC-Daten) und der Taster `K1`.
