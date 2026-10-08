#!/bin/bash
# Hackulator firmware flasher - no ESP-IDF needed.
# Requires: python3 + esptool (auto-installed if missing).
set -e
cd "$(dirname "$0")"

command -v python3 >/dev/null || { echo "Install Python 3 first."; exit 1; }
python3 -m esptool version >/dev/null 2>&1 || python3 -m pip install esptool

read -rp "Enter serial port [/dev/ttyUSB0]: " PORT
[ -z "$PORT" ] && { echo "No port given."; exit 1; }

exec python3 -m esptool --chip esp32 -p "$PORT" -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_freq 40m --flash_size 2MB \
  0x1000 bootloader.bin 0x8000 partition-table.bin 0x10000 hackulator.bin
