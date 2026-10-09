#!/bin/bash
# Hackulator - build + flash (Linux / macOS)
# Needs ESP-IDF 5.x. If idf.py is missing, this script tries the usual
# install locations before giving up.
set -e
cd "$(dirname "$0")"

# Auto-source ESP-IDF if not already exported in this shell.
if ! command -v idf.py >/dev/null; then
  for f in "$IDF_PATH/export.sh" \
           "$HOME/esp-idf/export.sh" \
           "$HOME/esp/esp-idf/export.sh" \
           /opt/esp-idf/export.sh \
           /opt/esp/esp-idf/export.sh; do
    if [ -f "$f" ]; then
      echo "Sourcing $f ..."
      # shellcheck disable=SC1090
      source "$f"
      break
    fi
  done
fi

if ! command -v idf.py >/dev/null; then
  cat >&2 <<'EOF'
ERROR: ESP-IDF not found.

Install it (https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html)
then either source it yourself:

    source ~/esp-idf/export.sh

or re-run this script, which auto-detects ~/esp-idf and /opt/esp-idf.
EOF
  exit 1
fi

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