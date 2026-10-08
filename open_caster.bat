@echo off
REM Hackulator - open caster viewer (Windows)
REM Streams TFT over UART0 @ 460800 baud. Keys: WASD=arrows, E/Enter=OK, B/Esc=BACK.
setlocal
cd /d "%~dp0"

set /p PORT=Enter caster port [e.g. COM5, empty=auto-detect, L=list]: 
if /i "%PORT%"=="L" (
  python tools\hacku_viewer.py --list-ports
  set /p PORT=Enter caster port [e.g. COM5]: 
)

if "%PORT%"=="" (
  echo Connecting [auto-detect]...
  python tools\hacku_viewer.py
) else (
  echo Connecting on %PORT%...
  python tools\hacku_viewer.py --port %PORT%
)
pause
