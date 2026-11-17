# Waveshare ESP32-P4-Nano

Board page: <https://docs.waveshare.com/ESP32-P4-NANO> · Schematic: <https://files.waveshare.com/wiki/ESP32-P4-NANO/ESP32-P4-NANO-schematic.pdf>

The Nano shares its pin layout with the ESP32-P4-WIFI6-DEV-KIT. The pins below were checked against the Nano schematic
and are defined in `main/boards/waveshare_p4_nano.hpp`; the board is selected in menuconfig under
**Espressynth > Board Configuration > Target Board**.

| Function | Pins |
| --- | --- |
| I2C0 | SDA GPIO7, SCL GPIO8 |
| ES8311 codec (I2S) | MCLK GPIO13, BCLK GPIO12, LRCK GPIO10, DOUT GPIO9, DIN GPIO11, PA enable GPIO53 |
| ESP32-C6 co-processor (SDIO) | CLK GPIO18, CMD GPIO19, D0-D3 GPIO14-17, reset GPIO54, wake GPIO6 |
| TF card (SDMMC) | CLK GPIO43, CMD GPIO44, D0-D3 GPIO39-42, power enable GPIO45 |
| Ethernet (IP101, RMII) | MDC GPIO31, MDIO GPIO52, reset GPIO51, clock GPIO50, TX_EN GPIO49, TXD GPIO34/35, RXD GPIO29/30, CRS_DV GPIO28 |

## What is different from the dev kit

- **Audio:** ES8311 codec with an NS4150B **mono** speaker amplifier (one 8 ohm speaker connector) and an on-board microphone.
  Audio hardware is optional in the sense that other codecs (for example a WM8960 HAT) can be added in menuconfig.
- **Display:** optional. A MIPI-DSI display can be attached; without one the firmware detects that at start and runs without display.
- **USB:** one USB-A 2.0 host port, no 4-port hub.
- **Chip:** ESP32-P4 v1.x, so the firmware uses the "revisions below 3.0" mode.

## Not yet verified on hardware

The firmware for this board was built from the schematic and the vendor documentation but has not been run on a Nano yet.
Check the boot log for the chip revision (`Chip rev`), the ES8311, Ethernet and Wi-Fi lines, and the "No display connected"
or display lines.
