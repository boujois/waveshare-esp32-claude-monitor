#!/bin/sh
# Builds everything for a release into dist/:
#   dist/Claude-Monitor-Mac.zip          installer: the display's software, Mac helper + menu bar app
#   dist/claude-monitor-firmware.bin     single image to flash at 0x0 (any flasher)
#   dist/web/                            browser installer page (ESP Web Tools) + firmware parts,
#                                        and install.sh, the one-line Mac install
# Usage: tools/build-release.sh [version]     (used locally and by .github/workflows/release.yml)
# Needs macOS with Xcode or the Command Line Tools 16 or later, for the menu bar app (Swift 6).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${1:-dev}"
DIST="$ROOT/dist"
FW="$ROOT/claude-status/firmware"
BUILD="$FW/.pio/build/waveshare-s3-lcd128"
PIO_HOME="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"

# Check before the long firmware build, not after
if [ "$(uname -s)" != Darwin ]; then
  echo "ERROR: the Mac package includes the menu bar app, which can only be built on macOS" >&2
  exit 1
fi
if ! xcrun --find swiftc >/dev/null 2>&1; then
  echo "ERROR: swiftc not found - install Xcode or the Command Line Tools 16 or later (xcode-select --install)" >&2
  exit 1
fi

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
cp "$BUILD/firmware.bin" "$DIST/claude-monitor-app.bin"  # app only: used for Wi-Fi updates
esptool --chip esp32s3 merge_bin -o "$DIST/claude-monitor-firmware.bin" \
  --flash_mode keep --flash_freq keep --flash_size keep \
  0x0 "$BUILD/bootloader.bin" 0x8000 "$BUILD/partitions.bin" \
  0xe000 "$BOOT_APP0" 0x10000 "$BUILD/firmware.bin"

# Browser installer: separate parts so updates don't wipe the saved Wi-Fi (NVS at 0x9000)
cp "$ROOT/claude-status/package/web/index.html" "$ROOT/claude-status/package/web/install.sh" "$DIST/web/"
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
cp "$ROOT/claude-status/package/Install.command" "$ROOT/claude-status/package/Update.command" \
  "$ROOT/claude-status/package/Uninstall.command" "$PKG/"
(cd "$ROOT/claude-status/bridge" && cp claude_status_bridge.py chat_mcp.py hooks.py desktop_config.py update.py \
  update-skill.md find_python.sh install.sh uninstall.sh update.sh install-chat-connector.sh "$PKG/helper/")
echo "$VERSION" > "$PKG/helper/VERSION"
# The display's software, for Install.command to put on over USB: the installer page's parts
mkdir -p "$PKG/firmware"
cp "$DIST/web/bootloader.bin" "$DIST/web/partitions.bin" "$DIST/web/boot_app0.bin" "$DIST/web/firmware.bin" \
  "$ROOT/claude-status/package/flash-display.sh" "$PKG/firmware/"
chmod +x "$PKG"/*.command "$PKG"/helper/*.sh "$PKG"/firmware/*.sh
echo "== Building the menu bar app"
/bin/sh "$ROOT/claude-status/menubar/build.sh" "$PKG/helper" "$VERSION"
# Zipped as well, for installs updating from before the menu bar app: their update.py
# copies plain files only, and install.sh unpacks this when the app didn't come across
(cd "$PKG/helper" && zip -qry "Claude Monitor.app.zip" "Claude Monitor.app")
cat > "$PKG/READ ME FIRST.txt" <<EOF
Claude Monitor $VERSION - setup

1. Plug the display into this Mac with a USB-C cable that carries data, unless
   it's already set up and on your Wi-Fi.
2. Double-click "Install.command". macOS stops it the first time ("Apple could
   not verify..."): click Done, then open System Settings > Privacy & Security
   and click "Open Anyway". (On macOS 14 or older, right-click it and choose
   Open instead.)
   Or skip that step: paste this line into Terminal, which downloads and runs
   the same installer:
   /bin/bash -c "\$(curl -fsSL https://boujois.github.io/waveshare-esp32-claude-monitor/install.sh)"
3. Follow along. It puts the software on the display, tells you when to connect
   the display to your Wi-Fi from your phone, and sets up your Mac.
4. Then click the Claude Monitor icon in the menu bar (a small circle inside a
   circle, near the clock) to connect your plan usage, set up night mode and
   change other options. It needs macOS 13 or later.

To update later, click Update in the menu bar app when it offers one,
double-click "Update.command", or ask Claude Code to "update my Claude
monitor". To remove everything, double-click "Uninstall.command", or run it
from the Claude Monitor folder in ~/Library/Application Support.
EOF
(cd "$DIST/pkg" && zip -qry "$DIST/Claude-Monitor-Mac.zip" "Claude Monitor")
rm -rf "$DIST/pkg"

echo "== Done"
ls -la "$DIST" "$DIST/web"
