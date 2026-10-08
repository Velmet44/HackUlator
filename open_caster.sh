#!/bin/bash
# Hackulator - open caster viewer (Debian)
# Streams TFT over UART0 @ 460800 baud. Keys: WASD=arrows, E/Enter=OK, B/Esc=BACK.
# Deps: pip install pyserial pillow ; sudo apt install python3-tk
cd "$(dirname "$0")"

read -rp "Enter caster port [/dev/ttyUSB0, empty=auto-detect, L=list]: " PORT
if [[ "$PORT" == [Ll] ]]; then
  python3 tools/hacku_viewer.py --list-ports
  read -rp "Enter caster port: " PORT
fi

if [ -z "$PORT" ]; then
  echo "Connecting [auto-detect]..."
  exec python3 tools/hacku_viewer.py
else
  echo "Connecting on $PORT..."
  exec python3 tools/hacku_viewer.py --port "$PORT"
fi
