#!/usr/bin/env python3
"""
Espressynth deploy script: builds complete merged binary for all partitions.
Creates EspressiveSynths-Merge.bin containing:
  - bootloader (0x2000) and partition table (0x8000)
  - factory app firmware
  - coprocessor firmware (slave_fw partition)
  - www partition (web server static files)
  - littlefs partition (seed data) Hallo
"""

import os
import subprocess
import sys
import struct
from pathlib import Path

# Configuration
PROJECT_ROOT = Path(__file__).parent.absolute()
BUILD_DIR = PROJECT_ROOT / "build"
COPROCESSOR_BUILD = PROJECT_ROOT / "coprocessor_fw" / "build"
OUTPUT_FILENAME = BUILD_DIR / "EspressiveSynths-Merge.bin"
FLASH_SIZE = 16 * 1024 * 1024  # 16 MB

# Populated by build_binaries()
COPROCESSOR_APP_BIN = None
MAIN_APP_BIN = None

# Partition offsets and sizes (from partitions.csv)
PARTITIONS = {
    'bootloader': (0x2000, 0x6000),
    'partition_table': (0x8000, 0x1000),
    'nvs': (0x9000, 0x6000),
    'phy_init': (0xf000, 0x1000),
    'factory': (0x10000, 0x300000),
    'slave_fw': (0x310000, 0x200000),
    'www': (0x510000, 0x380000),
    'littlefs': (0x890000, 0x770000),
}

def run_command(cmd, description):
    """Run command and report progress."""
    print(f"→ {description}...")
    result = subprocess.run(cmd, shell=True)
    if result.returncode != 0:
        print(f"✗ Failed: {description}")
        sys.exit(1)
    print(f"✓ {description}")

def ensure_idf_env():
    """Verify ESP-IDF environment is active."""
    if not os.environ.get('IDF_PATH'):
        print("✗ IDF_PATH not set. Run: C:\\esp\\v6.1-rc1\\esp-idf\\export.ps1")
        sys.exit(1)

def find_app_bin(build_dir):
    """Find the app .bin in a build dir, ignoring bootloader/partition-table/littlefs/www/stale images."""
    exclude = {"www.bin", "littlefs.bin", "ota_data_initial.bin"}
    candidates = [
        p for p in build_dir.glob("*.bin")
        if p.name not in exclude and "_flashed" not in p.name
    ]
    if len(candidates) != 1:
        print(f"✗ Expected exactly one app .bin in {build_dir}, found: {[p.name for p in candidates]}")
        sys.exit(1)
    return candidates[0]

def build_binaries():
    """Build all required binaries."""
    global COPROCESSOR_APP_BIN, MAIN_APP_BIN

    # Coprocessor firmware
    os.chdir(PROJECT_ROOT / "coprocessor_fw")

    # Set target to esp32c6 for coprocessor
    os.environ['IDF_TARGET'] = 'esp32c6'

    run_command("idf.py set-target esp32c6", "Setting coprocessor target")
    run_command("idf.py build", "Building coprocessor firmware")

    COPROCESSOR_APP_BIN = find_app_bin(COPROCESSOR_BUILD)
    print(f"  Using: {COPROCESSOR_APP_BIN.name} ({COPROCESSOR_APP_BIN.stat().st_size} bytes)")

    # Restore main target
    os.environ['IDF_TARGET'] = 'esp32p4'

    # Main firmware
    os.chdir(PROJECT_ROOT)
    run_command("idf.py build", "Building main firmware")

    MAIN_APP_BIN = find_app_bin(BUILD_DIR)
    print(f"  Using: {MAIN_APP_BIN.name} ({MAIN_APP_BIN.stat().st_size} bytes)")

    # LittleFS www partition
    run_command("idf.py littlefs_www_bin", "Building www partition")

    # LittleFS data partition
    run_command("idf.py littlefs_littlefs_bin", "Building littlefs partition")

def merge_binaries():
    """Merge all binaries into single flash image."""
    print(f"\n→ Merging binaries into {OUTPUT_FILENAME.name}...")

    # Create blank 16 MB image (0xFF filled)
    merged = bytearray([0xFF] * FLASH_SIZE)

    # Bootloader and partition table: without them the merged image does not boot (the image is
    # written from flash offset 0, which also covers 0x2000..0x8fff).
    for name, path in (('bootloader', BUILD_DIR / "bootloader" / "bootloader.bin"),
                       ('partition_table', BUILD_DIR / "partition_table" / "partition-table.bin")):
        with open(path, 'rb') as f:
            data = f.read()
        offset, size = PARTITIONS[name]
        if len(data) > size:
            print(f"✗ {name} ({len(data)} bytes) exceeds its slot ({size} bytes)")
            sys.exit(1)
        merged[offset:offset+len(data)] = data
        print(f"  ✓ {name} at 0x{offset:06x} ({len(data)} bytes)")

    # Read and place factory app
    with open(MAIN_APP_BIN, 'rb') as f:
        data = f.read()
    if len(data) > PARTITIONS['factory'][1]:
        print(f"✗ factory app ({len(data)} bytes) exceeds partition size ({PARTITIONS['factory'][1]} bytes)")
        sys.exit(1)
    offset = PARTITIONS['factory'][0]
    merged[offset:offset+len(data)] = data
    print(f"  ✓ factory app at 0x{offset:06x} ({len(data)} bytes)")

    # Read and place coprocessor firmware
    with open(COPROCESSOR_APP_BIN, 'rb') as f:
        data = f.read()
    if len(data) > PARTITIONS['slave_fw'][1]:
        print(f"✗ coprocessor fw ({len(data)} bytes) exceeds partition size ({PARTITIONS['slave_fw'][1]} bytes)")
        sys.exit(1)
    offset = PARTITIONS['slave_fw'][0]
    merged[offset:offset+len(data)] = data
    print(f"  ✓ slave_fw at 0x{offset:06x} ({len(data)} bytes)")

    # Read and place www partition
    www_bin = BUILD_DIR / "www.bin"
    with open(www_bin, 'rb') as f:
        data = f.read()
    if len(data) > PARTITIONS['www'][1]:
        print(f"✗ www partition ({len(data)} bytes) exceeds partition size ({PARTITIONS['www'][1]} bytes)")
        sys.exit(1)
    offset = PARTITIONS['www'][0]
    merged[offset:offset+len(data)] = data
    print(f"  ✓ www at 0x{offset:06x} ({len(data)} bytes)")

    # Read and place littlefs partition
    littlefs_bin = BUILD_DIR / "littlefs.bin"
    with open(littlefs_bin, 'rb') as f:
        data = f.read()
    if len(data) > PARTITIONS['littlefs'][1]:
        print(f"✗ littlefs partition ({len(data)} bytes) exceeds partition size ({PARTITIONS['littlefs'][1]} bytes)")
        sys.exit(1)
    offset = PARTITIONS['littlefs'][0]
    merged[offset:offset+len(data)] = data
    print(f"  ✓ littlefs at 0x{offset:06x} ({len(data)} bytes)")

    # Write merged binary
    with open(OUTPUT_FILENAME, 'wb') as f:
        f.write(merged)

    size_mb = len(merged) / (1024 * 1024)
    print(f"\n✓ Merged binary created: {OUTPUT_FILENAME.name} ({size_mb:.1f} MB)")
    print(f"  Total size: {len(merged)} bytes (0x{len(merged):x})")
    print(f"\n  To flash all partitions at once:")
    print(f"  python -m esptool --chip esp32p4 -p COM3 write-flash 0x0 {OUTPUT_FILENAME.name}")

def main():
    print("━" * 60)
    print("Espressynth Merged Binary Builder")
    print("━" * 60)

    ensure_idf_env()
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    build_binaries()
    merge_binaries()

if __name__ == '__main__':
    main()
