#!/bin/bash
# Hackulator firmware flasher - no ESP-IDF needed.
# Requires Python 3 + esptool (auto-installed if missing).
set -e
cd "$(dirname "$0")"

# Pick the interpreter: python3 on most systems, python on some (macOS
# Homebrew, minimal containers).
PY=""
for c in python3 python; do
  if command -v "$c" >/dev/null; then PY="$c"; break; fi
done
if [ -z "$PY" ]; then
  echo "ERROR: Python 3 not found. Install it and re-run." >&2
  exit 1
fi

"$PY" -m esptool version >/dev/null 2>&1 || "$PY" -m pip install esptool

if [ -t 0 ]; then
  read -rp "Enter serial port [/dev/ttyUSB0]: " PORT
else
  PORT="${1:-}"
fi
if [ -z "$PORT" ]; then
  echo "No port given. Usage: ./flash.sh /dev/ttyUSB0" >&2
  exit 1
fi

exec "$PY" -m esptool --chip esp32 -p "$PORT" -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_freq 40m --flash_size 2MB \
  0x1000 bootloader.bin 0x8000 partition-table.bin 0x10000 hackulator.bin