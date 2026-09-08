#!/usr/bin/env bash
# Builds the ESP32-C6 co-processor firmware and stages it into the P4 host's
# "slave_fw" partition, ready for Feature::Hosted::OTA to push at boot (or via the
# host's CLI "coprocessor-update" command). See README.md for background.
#
# Usage: ./build_and_stage.sh <p4-serial-port>   (e.g. /dev/ttyUSB0, /dev/ttyACM0)

set -e

# Offset of the "slave_fw" partition in main/partitions.csv on the P4 host. Keep in sync
# if that partition table changes.
SLAVE_FW_OFFSET=0x310000

if [ -z "$1" ]; then
    echo "Usage: $0 <p4-serial-port>  (e.g. /dev/ttyUSB0, /dev/ttyACM0)"
    exit 1
fi
PORT="$1"

if ! command -v idf.py >/dev/null 2>&1; then
    echo "idf.py not found on PATH. Source the ESP-IDF export script first (e.g. . \$IDF_PATH/export.sh)."
    exit 1
fi

cd "$(dirname "$0")"

echo "==> Setting target to esp32c6"
idf.py set-target esp32c6

echo "==> Building coprocessor firmware"
idf.py build

echo "==> Staging image into slave_fw on $PORT at offset $SLAVE_FW_OFFSET"
# --force: this .bin is built for esp32c6, so esptool's chip-mismatch check would otherwise
# refuse to write it -- expected here, since it's only raw staged data on the P4's flash,
# never booted by the P4 itself.
python -m esptool --chip esp32p4 -p "$PORT" write_flash --force "$SLAVE_FW_OFFSET" build/esp32p4_template_coprocessor.bin

echo "==> Done. Power-cycle the ESP32-P4 (or run 'coprocessor-update' on its CLI) to push it to the co-processor."
