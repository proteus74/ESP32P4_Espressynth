@echo off
setlocal

rem Builds the ESP32-C6 co-processor firmware and stages it into the P4 host's
rem "slave_fw" partition, ready for Feature::Hosted::OTA to push at boot (or via the
rem host's CLI "coprocessor-update" command). See README.md for background.
rem
rem Usage: build_and_stage.bat <p4-serial-port>   (e.g. COM16)

rem Offset of the "slave_fw" partition in main\partitions.csv on the P4 host. Keep in
rem sync if that partition table changes.
set SLAVE_FW_OFFSET=0x310000

if "%~1"=="" (
    echo Usage: %~nx0 ^<p4-serial-port^>  ^(e.g. COM16^)
    exit /b 1
)
set PORT=%~1

where idf.py >nul 2>nul
if errorlevel 1 (
    echo idf.py not found on PATH. Run the ESP-IDF export script first ^(export.bat in your ESP-IDF install^).
    exit /b 1
)

cd /d "%~dp0"

echo ==^> Setting target to esp32c6
call idf.py set-target esp32c6
if errorlevel 1 exit /b 1

echo ==^> Building coprocessor firmware
call idf.py build
if errorlevel 1 exit /b 1

echo ==^> Staging image into slave_fw on %PORT% at offset %SLAVE_FW_OFFSET%
rem --force: this .bin is built for esp32c6, so esptool's chip-mismatch check would otherwise
rem refuse to write it -- expected here, since it's only raw staged data on the P4's flash,
rem never booted by the P4 itself.
python -m esptool --chip esp32p4 -p %PORT% write_flash --force %SLAVE_FW_OFFSET% build\esp32p4_template_coprocessor.bin
if errorlevel 1 exit /b 1

echo ==^> Done. Power-cycle the ESP32-P4 (or run 'coprocessor-update' on its CLI) to push it to the co-processor.
