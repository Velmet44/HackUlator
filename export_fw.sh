#!/bin/bash
# Hackulator - export flashable firmware pack to firmware/ (Debian).
# Builds (if needed) and copies bootloader + partition table + app.
set -e
cd "$(dirname "$0")"

if ! command -v idf.py >/dev/null; then
  for f in "$HOME/esp-idf/export.sh" "$HOME/esp/esp-idf/export.sh" \
           /opt/esp-idf/export.sh; do
    if [ -f "$f" ]; then # shellcheck disable=SC1090
      source "$f"
      break
    fi
  done
fi
command -v idf.py >/dev/null || {
  echo "ESP-IDF not found. Run: source ~/esp-idf/export.sh"
  exit 1
}

echo "Building..."
idf.py build

for f in build/bootloader/bootloader.bin \
         build/partition_table/partition-table.bin \
         build/hackulator.bin; do
  [ -f "$f" ] || { echo "Missing $f"; exit 1; }
done

mkdir -p firmware
cp -f build/bootloader/bootloader.bin firmware/bootloader.bin
cp -f build/partition_table/partition-table.bin firmware/partition-table.bin
cp -f build/hackulator.bin firmware/hackulator.bin
chmod +x firmware/flash.sh
ls -l firmware/
echo
echo "Pack ready in firmware/ - zip it and share with README.txt + flash scripts."
echo "Recipients need only Python + esptool: pip install esptool"
