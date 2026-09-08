# ESP32-C6 co-processor firmware

Turns the on-board ESP32-C6 into the ESP-Hosted co-processor that
`main/features/wifi/wifi_manager.hpp` and `main/features/ble/ble_manager.hpp` talk to over SDIO. Separate
ESP-IDF project (own target: `esp32c6`) so it builds and versions independently
from the ESP32-P4 host app in `main/`.

## Build

```
idf.py set-target esp32c6
idf.py build
```

Produces `build/esp32p4_template_coprocessor.bin`.

## Getting it onto the board

Two ways to get this firmware running on the C6:

### A. Direct flash (first bring-up, or if the C6 exposes its own UART/USB)

```
idf.py -p <c6_port> flash
```

Only possible if the board wires the C6's own download-mode pins out to a
connector. If it doesn't (this repo's boards don't), use B.

### B. Push it from the P4 host over the existing SDIO link (no extra wiring)

Steps below, or run `build_and_stage.sh <p4-port>` (Linux/macOS) /
`build_and_stage.bat <p4-port>` (Windows) to do the build + stage in one go.
Requires `idf.py` on PATH (source/run the ESP-IDF export script first).

The P4 host's `Feature::Hosted::OTA::update_coprocessor_if_needed()`
(`main/features/hosted/hosted_ota.hpp`, enabled via `Application Features -> Push
co-processor firmware update from the P4 at boot`) reads a staged image out of
the P4's own `slave_fw` flash partition and streams it to the C6 over
`esp_hosted_slave_ota_begin/write/end/activate()` -- the co-processor OTAs
itself from firmware sitting on the *other* chip's flash, using the link
that's already wired up.

Stage the image by writing this project's `.bin` directly into the P4's
`slave_fw` partition (offset `0x310000`, see `../partitions.csv`) with esptool,
over the P4's own USB/UART port:

```
python -m esptool --chip esp32p4 -p <p4_port> write_flash --force 0x310000 build/esp32p4_template_coprocessor.bin
```

`--force` is required: this `.bin` is built for `esp32c6`, so esptool's chip-mismatch
check would otherwise refuse to write it -- expected here, since it's only raw staged
data on the P4's flash, never booted by the P4 itself.

Then power-cycle the P4 (or just let the next boot run) with
`CONFIG_APP_ENABLE_COPROCESSOR_OTA=y` set. It checks the C6's reported version,
skips if already current, otherwise pushes the staged image, activates it, and
restarts the host to resync. Safe to leave enabled permanently -- it's a no-op
once versions match.

## Why this exists

The co-processor firmware and the ESP-Hosted host library
(`espressif/esp_hosted`, see `main/idf_component.yml`) must be on compatible
versions of the ESP-Hosted protocol -- see the version check at boot
(`eh_init_evt: ... fw versions: host=X coprocessor=Y`). `main/idf_component.yml`
and this project's `main/idf_component.yml` both pin the exact same
`espressif/esp_hosted` version for that reason; bump both together.
