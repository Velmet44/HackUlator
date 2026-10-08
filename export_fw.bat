@echo off
REM Hackulator - export flashable firmware pack to firmware\ (Windows).
REM Builds (if needed) and copies bootloader + partition table + app.
setlocal EnableDelayedExpansion

if "%IDF_PATH%"=="" (
  if exist "%IDF_TOOLS_PATH%\tools\idf-python\3.11.2\python.exe" (
    set "PATH=%IDF_TOOLS_PATH%\tools\idf-python\3.11.2;!PATH!"
  ) else if exist "C:\Espressif\tools\idf-python\3.11.2\python.exe" (
    set "PATH=C:\Espressif\tools\idf-python\3.11.2;!PATH!"
  )
  set "EXPORT_BAT="
  if exist "%IDF_TOOLS_PATH%\frameworks\esp-idf-v5.5.5\export.bat" set "EXPORT_BAT=%IDF_TOOLS_PATH%\frameworks\esp-idf-v5.5.5\export.bat"
  if not defined EXPORT_BAT if exist "C:\Espressif\frameworks\esp-idf-v5.5.5\export.bat" set "EXPORT_BAT=C:\Espressif\frameworks\esp-idf-v5.5.5\export.bat"
  if not defined EXPORT_BAT if exist "%USERPROFILE%\esp\esp-idf\export.bat" set "EXPORT_BAT=%USERPROFILE%\esp\esp-idf\export.bat"
  if not defined EXPORT_BAT (
    echo Could not find ESP-IDF export.bat.
    pause
    exit /b 1
  )
  call "!EXPORT_BAT!"
  if errorlevel 1 (
    echo Export failed.
    pause
    exit /b 1
  )
)

cd /d "%~dp0"
echo Building...
call idf.py build
if errorlevel 1 (
  echo BUILD FAILED - firmware\ not updated.
  pause
  exit /b 1
)

if not exist "build\bootloader\bootloader.bin" (
  echo Missing build\bootloader\bootloader.bin
  pause
  exit /b 1
)
if not exist "build\partition_table\partition-table.bin" (
  echo Missing build\partition_table\partition-table.bin
  pause
  exit /b 1
)
if not exist "build\hackulator.bin" (
  echo Missing build\hackulator.bin
  pause
  exit /b 1
)

mkdir firmware 2>nul
copy /y build\bootloader\bootloader.bin firmware\bootloader.bin >nul
copy /y build\partition_table\partition-table.bin firmware\partition-table.bin >nul
copy /y build\hackulator.bin firmware\hackulator.bin >nul
dir firmware\*.bin
echo.
echo Pack ready in firmware\ - zip it and share with README.txt + flash scripts.
echo Recipients need only Python + esptool: pip install esptool
pause
