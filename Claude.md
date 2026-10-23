Wenn du den Quellcode kompilieren willst, dann nutze diese Zeilen:
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
idf.py build

Nicht `C:\esp\v6.1\esp-idf\export.ps1` nutzen: das aktiviert eine andere Python-venv als das EIM-Profil,
und `idf.py` verweigert dann den Build in `build/`, der mit der jeweils anderen venv konfiguriert wurde.

Flashen: idf.py -p <COM-Port> flash

Es gibt nur eine Konfiguration: die `sdkconfig` im Projektverzeichnis, bearbeitet mit `idf.py menuconfig`.
Immer von ihr aus bauen - kein `-B <anderer Build-Ordner>`, kein `-D SDKCONFIG=...`, keine
board-spezifischen `SDKCONFIG_DEFAULTS`. Das Board wird in menuconfig gewählt (Espressynth -> Board
Configuration) und leitet seine festen Pins/Fähigkeiten selbst ab (main/Kconfig.projbuild, siehe README
"Configuration model"). Zusatzhardware wie WM8960 ist kein Board-Default, sondern ein Feature in menuconfig.

Einzige Ausnahme: `python deploy.py --all` baut Release-Images für alle Boards außer Custom, jedes in
`build/boards/<board>/` mit einer generierten sdkconfig (`sdkconfig.defaults` + Board-Wahl). Das macht
nur deploy.py, nie von Hand. `python deploy.py` ohne `--all` baut das in menuconfig gewählte Board aus
der Root-`sdkconfig` in `build/`. Ergebnis jeweils `build/EspressiveSynths-<board>.bin`.

Nur wenn `--all` für alle Boards klappt: deploy.py ersetzt `../Espressynth_Website/static/firmware/`
(je Board Partition-Binaries + ESP-Web-Tools-`manifest.json`, flashbar über `/flash` der Webseite) und
erhöht danach `FIRMWARE_VERSION` in `main/version.h` um 0.01. `main/version.h` nicht von Hand hochzählen.
