HACKULATOR firmware pack - flash with esptool, no ESP-IDF needed.
=============================================================

Requirements: Python 3 + esptool  (pip install esptool)
  Windows: run flash.bat  |  Debian: chmod +x flash.sh && ./flash.sh

Files (offsets for a 2 MB flash, MUST match):
  0x1000  bootloader.bin
  0x8000  partition-table.bin   (nvs 24K @0x9000, phy 4K @0xf000,
                                 factory app 1920K @0x10000)
  0x10000 hackulator.bin        (ESP32 classic, ILI9341 240x320)

Flash settings: --chip esp32, DIO, 40 MHz, 2 MB. Equivalent manual cmd:
  python -m esptool --chip esp32 -p PORT -b 460800 \
    --before default_reset --after hard_reset write_flash \
    --flash_mode dio --flash_freq 40m --flash_size 2MB \
    0x1000 bootloader.bin 0x8000 partition-table.bin \
    0x10000 hackulator.bin

After flashing: reset the board. Headless units stream the display over
UART0 @ 460800 baud (PKC protocol, see tools/hacku_viewer.py).
