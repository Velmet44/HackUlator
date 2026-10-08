#!/bin/bash
# Hackulator - build + flash (Debian)
# Requires ESP-IDF 5.3 sourced:  source ~/esp-idf/export.sh
set -e
cd "$(dirname "$0")"

read -rp "Enter serial port [/dev/ttyUSB0, empty=auto-detect]: " PORT
PORT=${PORT:-}

echo "Building..."
idf.py build

if [ -z "$PORT" ]; then
  echo "Flashing [auto-detect]..."
  idf.py flash
else
  echo "Flashing on $PORT..."
  idf.py -p "$PORT" flash
fi

echo "Done. Reset board, then run ./open_caster.sh to view."
