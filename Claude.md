If you want to compile the source code, use these lines:
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
idf.py build

Do not use `C:\esp\v6.1\esp-idf\export.ps1`: it activates a different Python venv than the EIM profile,
and `idf.py` then refuses to build in `build/`, which was configured with the other venv.

Flashing: idf.py -p <COM-Port> flash

There is only one configuration: the `sdkconfig` in the project directory, edited with `idf.py menuconfig`.
Always build from it - no `-B <other build folder>`, no `-D SDKCONFIG=...`, no
board-specific `SDKCONFIG_DEFAULTS`. The board is selected in menuconfig (Espressynth -> Board
Configuration) and derives its fixed pins/capabilities itself (main/Kconfig.projbuild, see README
"Configuration model"). Additional hardware such as WM8960 is not a board default but a feature in menuconfig.

Only exception: `python tools/deploy.py --all` builds release images for all boards except Custom, each in
`build/boards/<board>/` with a generated sdkconfig (`sdkconfig.defaults` + board choice). Only
tools/deploy.py does this, never by hand. `python tools/deploy.py` without `--all` builds the board selected in menuconfig from the
root `sdkconfig` into `build/`. Result in each case: `build/EspressiveSynths-<board>.bin`.

Only if `--all` succeeds for all boards: deploy.py replaces `../../../JoLaBi_Homepage/static/firmware/espressynth/`
(per board partition binaries + ESP Web Tools `manifest.json`, flashable via `/de/projekte/espressynth/flashen` on jolabi.de) and
then increments `FIRMWARE_VERSION` in `main/version.h` by 0.01. Do not increment `main/version.h` by hand.
