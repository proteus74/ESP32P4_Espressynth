#!/usr/bin/env python3
"""
Builds the ESP32-C6 co-processor firmware (coprocessor_fw/) and stages it into the P4's
"slave_fw" flash partition, ready for Feature::Hosted::OTA to push to the C6 at the next boot.

  python deploy_coprocessor.py COM9     build + stage on COM9
  python deploy_coprocessor.py          build only (no port given)

The build and the partition offset come from deploy.py, so they stay in sync with the full
release build. Needs the ESP-IDF environment (same as deploy.py):
  . C:\\Espressif\\tools\\Microsoft.v6.1.PowerShell_profile.ps1

The host only pushes the image when the running co-processor reports another ESP-Hosted version
than the host was built with. After staging a rebuild of the same version, run the CLI command
`coprocessor-update force` on the board (or reset it after bumping the esp_hosted pin).
"""

import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent / "tools"))
import deploy  # tools/deploy.py


def stage(app_bin, port):
    offset, size = deploy.PARTITIONS['slave_fw']
    if app_bin.stat().st_size > size:
        print(f"✗ {app_bin.name} ({app_bin.stat().st_size} bytes) does not fit slave_fw ({size} bytes)")
        sys.exit(1)
    # --force: the image is built for esp32c6, esptool would refuse it for an esp32p4 flash.
    deploy.run_command(
        f'"{sys.executable}" -m esptool --chip esp32p4 -p {port} write-flash --force {offset:#x} "{app_bin}"',
        f"Staging {app_bin.name} into slave_fw ({offset:#x}) on {port}")


def main():
    if len(sys.argv) > 2 or (len(sys.argv) == 2 and sys.argv[1].startswith('-')):
        print(__doc__)
        sys.exit(1)
    port = sys.argv[1] if len(sys.argv) == 2 else None

    deploy.ensure_idf_env()
    app_bin = deploy.build_coprocessor()
    if port is None:
        print(f"✓ Built {app_bin}. Pass a port to stage it, e.g. python deploy_coprocessor.py COM9")
        return
    stage(app_bin, port)
    print("✓ Staged. Reset the board: the host pushes the image to the C6 if the versions differ "
          "(or run `coprocessor-update force` on the CLI).")


if __name__ == '__main__':
    main()
