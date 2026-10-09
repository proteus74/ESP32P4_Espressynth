Wenn du den Quellcode kompilieren willst, dann nutze diese Zeilen:
C:\esp\v6.1\esp-idf\export.ps1 
idf.py build

Flashen: idf.py -p <COM-Port> flash

Es gibt nur eine Konfiguration: die `sdkconfig` im Projektverzeichnis, bearbeitet mit `idf.py menuconfig`.
Immer von ihr aus bauen - kein `-B <anderer Build-Ordner>`, kein `-D SDKCONFIG=...`, keine
board-spezifischen `SDKCONFIG_DEFAULTS`. Das Board wird in menuconfig gewählt (Espressynth -> Board
Configuration) und leitet seine festen Pins/Fähigkeiten selbst ab (main/Kconfig.projbuild, siehe README
"Configuration model"). Zusatzhardware wie WM8960 ist kein Board-Default, sondern ein Feature in menuconfig.
