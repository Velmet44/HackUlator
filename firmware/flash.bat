@echo off
REM Hackulator firmware flasher - no ESP-IDF needed.
REM Requires: Python 3 + esptool (auto-installed if missing).
setlocal
cd /d "%~dp0"

where python >nul 2>nul
if errorlevel 1 (
  echo Install Python 3 first: https://www.python.org/downloads/
  pause
  exit /b 1
)
python -m esptool version >nul 2>nul
if errorlevel 1 (
  echo Installing esptool...
  python -m pip install esptool
  if errorlevel 1 (
    pause
    exit /b 1
  )
)

set /p PORT=Enter serial port [e.g. COM5]: 
if "%PORT%"=="" (
  echo No port given.
  pause
  exit /b 1
)

python -m esptool --chip esp32 -p %PORT% -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 40m --flash_size 2MB 0x1000 bootloader.bin 0x8000 partition-table.bin 0x10000 hackulator.bin
if errorlevel 1 (
  echo FLASH FAILED - check port, USB driver, cable, BOOT/EN buttons.
  pause
  exit /b 1
)

echo Done. Reset the board.
pause
