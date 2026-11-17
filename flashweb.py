#!/usr/bin/env python3
"""
Flash only the "www" partition (web server static files from data/, see partitions.csv).

  python flashweb.py           port auto-detected by esptool
  python flashweb.py COM5      explicit port

Builds build/www.bin from data/ with `idf.py littlefs_www_bin` and writes it with esptool;
app, coprocessor firmware and the littlefs user data partition stay untouched.
esptool is called directly instead of via `idf.py www-flash`: behind ninja its output is no
terminal, so it shows no progress bar. Fast reflashing is kept (--diff-with www_flashed.bin,
only changed sectors are written; a full image takes ~20 s at 460800 baud). esptool erases its
progress bar at the end of each write stage, so for a few changed sectors it is only briefly
visible. If the flash content differs from www_flashed.bin (e.g. another board was connected),
esptool's post-flash hash check fails and it rewrites the whole image by itself.
"""

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent / "tools"))
from deploy import BUILD_DIR, PARTITIONS, ensure_build_dir

IDF_PROFILE = r"C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1"


def main():
    if not os.environ.get('IDF_PATH'):
        # Re-run inside the EIM environment (its Python venv is the one build/ is configured with).
        args = " ".join(f"'{a}'" for a in sys.argv[1:])
        cmd = f". '{IDF_PROFILE}'; python '{os.path.abspath(__file__)}' {args}; exit $LASTEXITCODE"
        sys.exit(subprocess.run(["pwsh", "-NoProfile", "-Command", cmd]).returncode)
    # The venv/path mismatch error ("fullclean needed") is fixed up front, same as deploy.py.
    ensure_build_dir(BUILD_DIR)
    if subprocess.run("idf.py littlefs_www_bin", shell=True).returncode:
        sys.exit(1)

    # Same esptool arguments `idf.py www-flash` would use (incl. its default baud rate).
    flasher = json.loads((BUILD_DIR / "flasher_args.json").read_text())
    tool, flash = flasher["extra_esptool_args"], flasher["flash_settings"]
    cmd = [sys.executable, "-m", "esptool", "--chip", tool["chip"],
           "-b", os.environ.get("ESPBAUD", "921600")]
    if len(sys.argv) > 1:
        cmd += ["-p", sys.argv[1]]
    cmd += [f"--before={tool['before']}", f"--after={tool['after']}", "write-flash",
            "--flash-mode", flash["flash_mode"], "--flash-freq", flash["flash_freq"],
            "--flash-size", flash["flash_size"], hex(PARTITIONS['www'][0]), "www.bin"]
    if (BUILD_DIR / "www_flashed.bin").exists():
        cmd += ["--diff-with", "www_flashed.bin"]
    if subprocess.run(cmd, cwd=BUILD_DIR).returncode:
        sys.exit(1)
    # Keeps a later `idf.py www-flash` fast reflash in sync with what is now in flash.
    shutil.copyfile(BUILD_DIR / "www.bin", BUILD_DIR / "www_flashed.bin")


if __name__ == '__main__':
    main()
