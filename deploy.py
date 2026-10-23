#!/usr/bin/env python3
"""
Espressynth deploy script: builds complete merged flash images.

  python deploy.py         board selected in menuconfig, built from the root sdkconfig in build/
                           (exactly what `idf.py build` builds, including local menuconfig settings)
  python deploy.py --all   every board of the "Target Board" choice in main/Kconfig.projbuild
                           except the custom board (new boards there are picked up automatically).
                           Each board gets its own build dir build/boards/<board>/ with a generated sdkconfig
                           (sdkconfig.defaults + the board choice). The root sdkconfig is NOT used:
                           release images must not depend on local menuconfig tweaks (or carry the
                           local Wi-Fi credentials).
                           When every board succeeds, the images are also published to the website
                           (../Espressynth_Website/static/firmware/<board>/: partition binaries plus an
                           ESP Web Tools manifest.json, flashable from the browser) and FIRMWARE_VERSION
                           in main/version.h is raised by 0.01 for the next release.

Each build/EspressiveSynths-<board>.bin contains:
  - bootloader (0x2000) and partition table (0x8000)
  - factory app firmware
  - coprocessor firmware (slave_fw partition, built once, shared by all boards)
  - www partition (web server static files)
  - littlefs partition (seed data)
"""

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from decimal import Decimal
from pathlib import Path

# Configuration
PROJECT_ROOT = Path(__file__).parent.absolute()
BUILD_DIR = PROJECT_ROOT / "build"
BOARDS_BUILD_DIR = BUILD_DIR / "boards"
COPROCESSOR_BUILD = PROJECT_ROOT / "coprocessor_fw" / "build"
KCONFIG = PROJECT_ROOT / "main" / "Kconfig.projbuild"
VERSION_H = PROJECT_ROOT / "main" / "version.h"
WEB_FIRMWARE_DIR = PROJECT_ROOT.parent / "Espressynth_Website" / "static" / "firmware"
FLASH_SIZE = 16 * 1024 * 1024  # 16 MB

# Skipped by --all: the custom board only has placeholder pins until configured in menuconfig.
NOT_IN_ALL = {"BOARD_CUSTOM_P4"}

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

def run_command(cmd, description, fatal=True):
    """Run command and report progress. Returns True on success."""
    print(f"→ {description}...")
    result = subprocess.run(cmd, shell=True)
    if result.returncode != 0:
        print(f"✗ Failed: {description}")
        if fatal:
            sys.exit(1)
        return False
    print(f"✓ {description}")
    return True

def ensure_idf_env():
    """Verify ESP-IDF environment is active."""
    if not os.environ.get('IDF_PATH'):
        print("✗ IDF_PATH not set. Run: . C:\\Espressif\\tools\\Microsoft.v6.1.PowerShell_profile.ps1")
        sys.exit(1)

def ensure_build_dir(build_dir, project_dir=PROJECT_ROOT):
    """idf.py refuses to build a dir configured with another Python venv (two ESP-IDF venvs on this
    machine: EIM C:/Espressif/tools/python and ~/.espressif/python_env) or for another project path
    (the checkout was moved). Same checks as idf.py."""
    cache = build_dir / "CMakeCache.txt"
    if not cache.exists():
        return
    expected = {"PYTHON": sys.executable, "CMAKE_HOME_DIRECTORY": str(project_dir)}
    for line in cache.read_text(errors="ignore").splitlines():
        key = line.split(":", 1)[0]
        if key in expected and "=" in line:
            configured = line.split("=", 1)[1]
            if os.path.normcase(os.path.normpath(configured)) != os.path.normcase(os.path.normpath(expected[key])):
                print(f"  {build_dir} was configured with {key}={configured}")
                # Not 'idf.py fullclean': in IDF 6.1 it also wipes managed_components and aborts on
                # any hash mismatch there. Deleting the build dir is all the check needs.
                print(f"→ Removing {build_dir} ({key} changed)")
                shutil.rmtree(build_dir)
                return

def read_boards():
    """Board symbols of the 'choice BOARD_TARGET' block, in menuconfig order -> {name: symbol}."""
    text = KCONFIG.read_text(encoding="utf-8")
    block = re.search(r"^\s*choice BOARD_TARGET\b(.*?)^\s*endchoice", text, re.S | re.M)
    if not block:
        print(f"✗ 'choice BOARD_TARGET' not found in {KCONFIG}")
        sys.exit(1)
    boards = {}
    for symbol in re.findall(r"^\s*config (\w+)", block.group(1), re.M):
        name = re.sub(r"^BOARD_|_BOARD$", "", symbol).lower()
        boards[name] = symbol
    return boards

def board_label(symbol):
    """Prompt text of a board symbol in the Kconfig, e.g. 'Waveshare ESP32-P4-WIFI6-DEV-KIT'."""
    text = KCONFIG.read_text(encoding="utf-8")
    m = re.search(rf'^\s*config {symbol}\s*\n\s*bool "((?:[^"\\]|\\.)*)"', text, re.M)
    return m.group(1).replace('\\"', '"') if m else symbol

VERSION_RE = re.compile(r'(#define FIRMWARE_VERSION ")([\d.]+)(")')

def read_version():
    m = VERSION_RE.search(VERSION_H.read_text(encoding="utf-8"))
    if not m:
        print(f"✗ FIRMWARE_VERSION not found in {VERSION_H}")
        sys.exit(1)
    return m.group(2)

def bump_version(version):
    """Raise FIRMWARE_VERSION by 0.01 (0.31 -> 0.32, 0.99 -> 1.00)."""
    step = Decimal("0.01")
    new = str((Decimal(version) + step).quantize(step))
    text = VERSION_H.read_text(encoding="utf-8")
    VERSION_H.write_text(VERSION_RE.sub(rf"\g<1>{new}\g<3>", text, count=1), encoding="utf-8")
    print(f"✓ FIRMWARE_VERSION {version} -> {new} ({VERSION_H.relative_to(PROJECT_ROOT)})")

def find_app_bin(build_dir):
    """Find the app .bin in a build dir, ignoring bootloader/partition-table/littlefs/www/stale images."""
    exclude = {"www.bin", "littlefs.bin", "ota_data_initial.bin"}
    candidates = [
        p for p in build_dir.glob("*.bin")
        if p.name not in exclude and "_flashed" not in p.name
    ]
    if len(candidates) != 1:
        raise ValueError(f"expected exactly one app .bin in {build_dir}, found: {[p.name for p in candidates]}")
    return candidates[0]

def build_coprocessor():
    """Build the ESP32-C6 coprocessor firmware (board independent) and return its app .bin."""
    os.chdir(PROJECT_ROOT / "coprocessor_fw")
    os.environ['IDF_TARGET'] = 'esp32c6'
    # set-target wipes the build dir (full ~1200 step rebuild), so only run it when needed.
    sdkconfig = PROJECT_ROOT / "coprocessor_fw" / "sdkconfig"
    if not sdkconfig.exists() or 'CONFIG_IDF_TARGET="esp32c6"' not in sdkconfig.read_text(errors="ignore"):
        run_command("idf.py set-target esp32c6", "Setting coprocessor target")
    ensure_build_dir(COPROCESSOR_BUILD, COPROCESSOR_BUILD.parent)
    run_command("idf.py build", "Building coprocessor firmware")
    os.environ['IDF_TARGET'] = 'esp32p4'
    os.chdir(PROJECT_ROOT)

    app_bin = find_app_bin(COPROCESSOR_BUILD)
    print(f"  Using: {app_bin.name} ({app_bin.stat().st_size} bytes)")
    return app_bin

def build_menuconfig_board(boards):
    """Build the root sdkconfig in build/ (the board chosen in menuconfig). Returns (name, build dir)."""
    config = (PROJECT_ROOT / "sdkconfig").read_text(encoding="utf-8", errors="ignore")
    name = next((n for n, s in boards.items() if f"\nCONFIG_{s}=y\n" in config), None)
    if name is None:
        print("✗ No board from 'choice BOARD_TARGET' is selected in sdkconfig")
        sys.exit(1)
    ensure_build_dir(BUILD_DIR)
    run_command("idf.py build littlefs_www_bin littlefs_littlefs_bin",
                f"Building {name} from sdkconfig (firmware, www, littlefs)")
    return name, BUILD_DIR

def build_board(name, symbol):
    """Build app, www and littlefs images for one board. Returns its build dir, or None on failure."""
    build_dir = BOARDS_BUILD_DIR / name
    ensure_build_dir(build_dir)
    build_dir.mkdir(parents=True, exist_ok=True)

    # Fresh sdkconfig whenever its inputs changed: an existing one would keep stale values
    # (sdkconfig.defaults only seeds symbols missing from it), and board-seeded prompted values
    # (ESP-Hosted SDIO pins, ...) are only applied to a new config. Unchanged inputs keep it, which
    # saves the CMake re-run of app and bootloader; the build itself is incremental either way.
    board_defaults = build_dir / "board.defaults"
    board_defaults.write_text(f"CONFIG_{symbol}=y\n")
    sdkconfig = build_dir / "sdkconfig"
    stamp = build_dir / "sdkconfig.inputs"
    inputs = sdkconfig_inputs_hash(board_defaults)
    if not stamp.exists() or stamp.read_text() != inputs:
        sdkconfig.unlink(missing_ok=True)
        stamp.unlink(missing_ok=True)

    defaults = f"{(PROJECT_ROOT / 'sdkconfig.defaults').as_posix()};{board_defaults.as_posix()}"
    cmd = (f'idf.py -B "{build_dir}" -DSDKCONFIG="{sdkconfig.as_posix()}" '
           f'-DSDKCONFIG_DEFAULTS="{defaults}" build littlefs_www_bin littlefs_littlefs_bin')
    if not run_command(cmd, f"Building {name} (firmware, www, littlefs)", fatal=False):
        return None
    stamp.write_text(inputs)  # only after success: a failed run regenerates the config next time
    return build_dir

def sdkconfig_inputs_hash(board_defaults):
    """Hash of everything a generated board sdkconfig depends on: both defaults files, every
    Kconfig of the project and its components, and the ESP-IDF checkout."""
    files = [PROJECT_ROOT / "sdkconfig.defaults", board_defaults]
    for pattern in ("main/Kconfig*", "components/**/Kconfig*", "managed_components/**/Kconfig*"):
        files += sorted(p for p in PROJECT_ROOT.glob(pattern) if p.is_file())
    h = hashlib.sha256(os.environ.get("IDF_PATH", "").encode())
    for f in files:
        h.update(f.as_posix().encode())
        h.update(f.read_bytes())
    return h.hexdigest()

def place(merged, name, path):
    """Copy a binary into its partition slot of the merged image."""
    data = path.read_bytes()
    offset, size = PARTITIONS[name]
    if len(data) > size:
        raise ValueError(f"{name} ({len(data)} bytes) exceeds its slot ({size} bytes)")
    merged[offset:offset+len(data)] = data
    print(f"  ✓ {name} at 0x{offset:06x} ({len(data)} bytes)")

def board_parts(build_dir, coprocessor_bin):
    """(partition, binary) pairs of one board's flash image."""
    # Bootloader and partition table: without them the image does not boot. The bootloader differs
    # per board (chip revision selection), so it always comes from the board's build dir.
    return [
        ('bootloader', build_dir / "bootloader" / "bootloader.bin"),
        ('partition_table', build_dir / "partition_table" / "partition-table.bin"),
        ('factory', find_app_bin(build_dir)),
        ('slave_fw', coprocessor_bin),
        ('www', build_dir / "www.bin"),
        ('littlefs', build_dir / "littlefs.bin"),
    ]

def merge_binaries(parts, output):
    """Merge all binaries of one board into a single 16 MB flash image."""
    print(f"→ Merging binaries into {output.name}...")
    merged = bytearray([0xFF] * FLASH_SIZE)
    for name, path in parts:
        place(merged, name, path)
    output.write_bytes(merged)
    print(f"✓ {output.name} ({len(merged) / (1024 * 1024):.1f} MB)")

def publish_web(published, version):
    """Replace the website's firmware dir with the partition binaries of every board plus one ESP Web
    Tools manifest per board. Separate parts instead of the merged image: the 16 MB image is mostly
    0xFF padding the browser would have to download and flash."""
    print(f"→ Publishing {version} to {WEB_FIRMWARE_DIR}...")
    if WEB_FIRMWARE_DIR.exists():
        shutil.rmtree(WEB_FIRMWARE_DIR)  # boards removed from the Kconfig disappear from the site
    for name, (symbol, parts) in published.items():
        board_dir = WEB_FIRMWARE_DIR / name
        board_dir.mkdir(parents=True)
        for part, path in parts:
            shutil.copyfile(path, board_dir / f"{part}.bin")
        manifest = {
            "name": f"Espressynth – {board_label(symbol)}",
            "version": version,
            "new_install_prompt_erase": True,
            "builds": [{
                "chipFamily": "ESP32-P4",
                "parts": [{"path": f"{part}.bin", "offset": PARTITIONS[part][0]} for part, _ in parts],
            }],
        }
        (board_dir / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
                                                 encoding="utf-8")
        print(f"  ✓ {name}")

def main():
    sys.stdout.reconfigure(line_buffering=True)  # keep our lines in order with idf.py output when piped
    print("━" * 60)
    print("Espressynth Merged Binary Builder")
    print("━" * 60)

    ensure_idf_env()
    args = sys.argv[1:]
    if args not in ([], ["--all"]):
        print("Usage: python deploy.py [--all]")
        sys.exit(1)
    build_all = args == ["--all"]
    boards = read_boards()
    version = read_version()
    if build_all and not WEB_FIRMWARE_DIR.parent.is_dir():
        print(f"✗ Website not found: {WEB_FIRMWARE_DIR.parent}")
        sys.exit(1)

    coprocessor_bin = build_coprocessor()

    builds = []  # (name, build dir or None)
    if build_all:
        for name, symbol in boards.items():
            if symbol in NOT_IN_ALL:
                continue
            print(f"\n{'━' * 20} {name} ({symbol}) {'━' * 20}")
            builds.append((name, build_board(name, symbol)))
    else:
        builds.append(build_menuconfig_board(boards))

    results = {}
    published = {}  # name -> (symbol, parts)
    for name, build_dir in builds:
        if build_dir is None:
            results[name] = None
            continue
        output = BUILD_DIR / f"EspressiveSynths-{name}.bin"
        try:
            parts = board_parts(build_dir, coprocessor_bin)
            merge_binaries(parts, output)
            results[name] = output
            published[name] = (boards[name], parts)
        except (OSError, ValueError) as e:
            print(f"✗ Merging {name} failed: {e}")
            results[name] = None

    print(f"\n{'━' * 60}")
    for name, output in results.items():
        status = output.relative_to(PROJECT_ROOT) if output else "FAILED"
        print(f"  {'✓' if output else '✗'} {name:<28} {status}")
    print("\n  Flash one image (all partitions at once):")
    print("  python -m esptool --chip esp32p4 -p <COM-Port> write-flash 0x0 build\\EspressiveSynths-<board>.bin")
    if not all(results.values()):
        sys.exit(1)

    if build_all:
        print()
        publish_web(published, version)
        bump_version(version)

if __name__ == '__main__':
    main()
