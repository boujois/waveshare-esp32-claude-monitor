#!/bin/sh
# Builds everything for a release into dist/:
#   dist/Claude-Monitor-Mac.zip          double-click Mac installer + helper
#   dist/claude-monitor-firmware.bin     single image to flash at 0x0 (any flasher)
#   dist/web/                            browser installer page (ESP Web Tools) + firmware parts
# Usage: tools/build-release.sh [version]     (used locally and by .github/workflows/release.yml)
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${1:-dev}"
DIST="$ROOT/dist"
FW="$ROOT/claude-status/firmware"
BUILD="$FW/.pio/build/waveshare-s3-lcd128"
PIO_HOME="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"

rm -rf "$DIST" && mkdir -p "$DIST/web"

# Firmware (no secrets.h needed: Wi-Fi is set up from a phone)
echo "== Building firmware $VERSION"
# RELEASE_BUILD makes the firmware ignore any local secrets.h (your Wi-Fi password)
(cd "$FW" && PLATFORMIO_BUILD_FLAGS="-DRELEASE_BUILD -DFW_VERSION=\\\"$VERSION\\\"" pio run)
if [ -f "$FW/src/secrets.h" ]; then
  SSID=$(sed -n 's/^#define WIFI_SSID "\(.*\)"/\1/p' "$FW/src/secrets.h")
  if [ -n "$SSID" ] && grep -q "$SSID" "$BUILD/firmware.bin"; then
    echo "ERROR: firmware contains the Wi-Fi name from secrets.h - refusing to package it" >&2
    exit 1
  fi
fi
BOOT_APP0="$PIO_HOME/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"
# PlatformIO's own esptool when installed the usual way; otherwise (e.g. pip in CI) the esptool package
if [ -x "$PIO_HOME/penv/bin/python" ]; then
  esptool() { "$PIO_HOME/penv/bin/python" "$PIO_HOME/packages/tool-esptoolpy/esptool.py" "$@"; }
else
  esptool() { python3 -m esptool "$@"; }
fi

cp "$BUILD/bootloader.bin" "$BUILD/partitions.bin" "$BUILD/firmware.bin" "$BOOT_APP0" "$DIST/web/"
esptool --chip esp32s3 merge_bin -o "$DIST/claude-monitor-firmware.bin" \
  --flash_mode keep --flash_freq keep --flash_size keep \
  0x0 "$BUILD/bootloader.bin" 0x8000 "$BUILD/partitions.bin" \
  0xe000 "$BOOT_APP0" 0x10000 "$BUILD/firmware.bin"

# Browser installer: separate parts so updates don't wipe the saved Wi-Fi (NVS at 0x9000)
cp "$ROOT/claude-status/package/web/index.html" "$DIST/web/"
cat > "$DIST/web/manifest.json" <<EOF
{
  "name": "Claude Monitor",
  "version": "$VERSION",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "parts": [
        { "path": "bootloader.bin", "offset": 0 },
        { "path": "partitions.bin", "offset": 32768 },
        { "path": "boot_app0.bin", "offset": 57344 },
        { "path": "firmware.bin", "offset": 65536 }
      ]
    }
  ]
}
EOF

# Mac package
echo "== Packaging Mac installer"
PKG="$DIST/pkg/Claude Monitor"
mkdir -p "$PKG/helper"
cp "$ROOT/claude-status/package/Install.command" "$ROOT/claude-status/package/Uninstall.command" "$PKG/"
(cd "$ROOT/claude-status/bridge" && cp claude_status_bridge.py chat_mcp.py hooks.py desktop_config.py \
  find_python.sh install.sh uninstall.sh install-chat-connector.sh "$PKG/helper/")
chmod +x "$PKG"/*.command "$PKG"/helper/*.sh
cat > "$PKG/READ ME FIRST.txt" <<EOF
Claude Monitor $VERSION - Mac setup

1. Make sure the display is flashed and on your Wi-Fi (see the installer page
   linked from the project README).
2. Right-click "Install.command" and choose Open. (macOS blocks double-clicking
   downloaded scripts the first time; if it still refuses, open System Settings >
   Privacy & Security and click "Open Anyway".)
3. Follow the questions in the window that opens.

To remove everything later, right-click "Uninstall.command" and choose Open.
EOF
(cd "$DIST/pkg" && zip -qry "$DIST/Claude-Monitor-Mac.zip" "Claude Monitor")
rm -rf "$DIST/pkg"

echo "== Done"
ls -la "$DIST" "$DIST/web"
