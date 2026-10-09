#!/bin/bash
# Hackulator - open caster viewer (Linux / macOS)
# Streams the display over UART0 @ 460800 baud.
# Keys: WASD=arrows, E/Enter=OK, B/Esc=BACK.
# Deps: pip install pyserial pillow ; sudo apt install python3-tk
set -e
cd "$(dirname "$0")"

PY=""
for c in python3 python; do
  if command -v "$c" >/dev/null; then PY="$c"; break; fi
done
if [ -z "$PY" ]; then
  echo "ERROR: Python 3 not found." >&2
  exit 1
fi

"$PY" -c "import serial" 2>/dev/null || "$PY" -m pip install pyserial
"$PY" -c "import PIL"   2>/dev/null || "$PY" -m pip install pillow

if [ -t 0 ]; then
  read -rp "Enter caster port [/dev/ttyUSB0, empty=auto-detect, L=list]: " PORT
else
  PORT="${1:-}"
fi
if [[ "${PORT,,}" == "l" ]]; then
  "$PY" tools/hacku_viewer.py --list-ports
  read -rp "Enter caster port: " PORT
fi

if [ -z "$PORT" ]; then
  echo "Connecting [auto-detect]..."
  exec "$PY" tools/hacku_viewer.py
else
  echo "Connecting on $PORT..."
  exec "$PY" tools/hacku_viewer.py --port "$PORT"
fi