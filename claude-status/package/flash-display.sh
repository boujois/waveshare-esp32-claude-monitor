#!/bin/bash
# Puts the Claude Monitor software on a display plugged into this Mac over USB:
#
#   flash-display.sh /dev/cu.usbmodem...
#
# Install.command runs this from the firmware folder of the Mac package, next to the four
# files it writes. They're the same parts at the same addresses as the installer page, so
# the display keeps any Wi-Fi and pairing it already has. Flashing uses Espressif's
# standalone esptool, downloaded for the job and checked against a pinned checksum, so
# neither Python nor a browser is needed. CLAUDE_MONITOR_ESPTOOL=<path> uses another esptool.

PORT="$1"
DIR="$(cd "$(dirname "$0")" && pwd)"
ESPTOOL_VERSION="v5.4.0"
# SHA-256 of esptool-$ESPTOOL_VERSION-macos-<arch>.tar.gz, from the release on GitHub
ESPTOOL_SHA256_ARM64="ba332671130939e2e6db90c2784488f7e62a1459b0fe3c5ec66e9a366821de7a"
ESPTOOL_SHA256_AMD64="910bb64fe39a84c792752701293c8aa294faeef229fe8705ecd6955b01db3778"
TMP=""
ESPTOOL="${CLAUDE_MONITOR_ESPTOOL:-}"

# Downloads esptool for this Mac's processor into a temporary folder
get_esptool() {
  local arch sha url
  if [ "$(sysctl -n hw.optional.arm64 2>/dev/null)" = 1 ]; then
    arch=arm64 sha="$ESPTOOL_SHA256_ARM64"
  else
    arch=amd64 sha="$ESPTOOL_SHA256_AMD64"
  fi
  url="https://github.com/espressif/esptool/releases/download/$ESPTOOL_VERSION/esptool-$ESPTOOL_VERSION-macos-$arch.tar.gz"
  TMP="$(mktemp -d)" || return 1
  trap '[ -n "$TMP" ] && rm -rf "$TMP"' EXIT
  echo "Downloading Espressif's esptool $ESPTOOL_VERSION to do it (about 60 MB)..."
  if ! curl -fL --progress-bar --retry 2 "$url" -o "$TMP/esptool.tar.gz"; then
    echo "Couldn't download esptool from $url" >&2
    return 1
  fi
  if ! echo "$sha  $TMP/esptool.tar.gz" | shasum -a 256 -c - >/dev/null 2>&1; then
    echo "The esptool download doesn't match its checksum, so it wasn't used." >&2
    return 1
  fi
  tar -xzf "$TMP/esptool.tar.gz" -C "$TMP" "esptool-macos-$arch/esptool" || return 1
  ESPTOOL="$TMP/esptool-macos-$arch/esptool"
}

main() {
  if [ -z "$PORT" ]; then
    echo "Usage: $0 <serial port, e.g. /dev/cu.usbmodem...>" >&2
    return 2
  fi
  for part in bootloader.bin partitions.bin boot_app0.bin firmware.bin; do
    if [ ! -f "$DIR/$part" ]; then
      echo "$part is missing from $DIR" >&2
      return 1
    fi
  done
  if [ -z "$ESPTOOL" ]; then
    get_esptool || return 1
  fi
  echo "Putting the software on the display. Don't unplug it until this finishes."
  # --chip makes esptool refuse anything that isn't an ESP32-S3. 921600 baud is what
  # PlatformIO uses for this board. One connection only: each one restarts the display.
  "$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 921600 write-flash \
    0x0 "$DIR/bootloader.bin" 0x8000 "$DIR/partitions.bin" \
    0xe000 "$DIR/boot_app0.bin" 0x10000 "$DIR/firmware.bin"
}

main "$@"
