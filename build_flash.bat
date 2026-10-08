@echo off
REM Hackulator - build + flash (Windows, double-click friendly)
REM Auto-sources ESP-IDF export.bat if IDF_PATH is missing. No manual setup.
setlocal EnableDelayedExpansion

REM --- 1. If IDF already exported in this window, skip setup ---
if not "%IDF_PATH%"=="" goto :have_idf

REM --- 2. Put IDF python 3.11 first so export picks the existing env ---
REM (system python 3.14 would demand a missing py3.14_env)
if exist "%IDF_TOOLS_PATH%\tools\idf-python\3.11.2\python.exe" (
  set "PATH=%IDF_TOOLS_PATH%\tools\idf-python\3.11.2;!PATH!"
) else if exist "C:\Espressif\tools\idf-python\3.11.2\python.exe" (
  set "PATH=C:\Espressif\tools\idf-python\3.11.2;!PATH!"
)

REM --- 3. Locate export.bat (env-based, then Espressif defaults) ---
set "EXPORT_BAT="
if exist "%IDF_PATH%\export.bat" set "EXPORT_BAT=%IDF_PATH%\export.bat"
if not defined EXPORT_BAT if exist "%IDF_TOOLS_PATH%\frameworks\esp-idf-v5.5.5\export.bat" set "EXPORT_BAT=%IDF_TOOLS_PATH%\frameworks\esp-idf-v5.5.5\export.bat"
if not defined EXPORT_BAT if exist "C:\Espressif\frameworks\esp-idf-v5.5.5\export.bat" set "EXPORT_BAT=C:\Espressif\frameworks\esp-idf-v5.5.5\export.bat"
if not defined EXPORT_BAT if exist "%USERPROFILE%\esp\esp-idf\export.bat" set "EXPORT_BAT=%USERPROFILE%\esp\esp-idf\export.bat"

if not defined EXPORT_BAT (
  echo Could not find ESP-IDF export.bat.
  echo Install IDF 5.x from https://dl.espressif.com/dl/esp-idf/
  echo or set IDF_TOOLS_PATH and re-run.
  pause
  exit /b 1
)

echo Sourcing !EXPORT_BAT! ...
call "!EXPORT_BAT!"
if errorlevel 1 (
  echo Export failed. Try Start Menu -^> "ESP-IDF CMD" instead.
  pause
  exit /b 1
)

:have_idf
where idf.py >nul 2>nul
if errorlevel 1 (
  echo idf.py still not on PATH after export. Aborting.
  pause
  exit /b 1
)
echo IDF: %IDF_PATH%

set /p PORT=Enter serial port [e.g. COM5, empty=auto-detect]: 

echo Building...
call idf.py build
if errorlevel 1 (
  echo BUILD FAILED - if it mentions a stale CMakeCache from another
  echo machine, run:  idf.py fullclean
  echo then run build_flash.bat again.
  pause
  exit /b 1
)

if "%PORT%"=="" (
  echo Flashing [auto-detect]...
  call idf.py flash
) else (
  echo Flashing on %PORT%...
  call idf.py -p %PORT% flash
)
if errorlevel 1 (
  echo FLASH FAILED - check COM port, USB driver, BOOT/EN buttons
  pause
  exit /b 1
)

echo Done. Reset board, then run open_caster.bat to view.
pause
