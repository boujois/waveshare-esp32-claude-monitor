#!/bin/bash
# Claude Monitor - one-line Mac installer. Paste this into Terminal:
#
#   /bin/bash -c "$(curl -fsSL https://boujois.github.io/waveshare-esp32-claude-monitor/install.sh)"
#
# Downloads the latest release's Mac package and runs its Install.command. macOS stops a
# downloaded .command the first time ("Apple could not verify..."), but not files fetched with
# curl, so this skips that step. Safe to run again to update or repair.
#
# CLAUDE_MONITOR_ZIP=<url> installs a different package instead, such as one release's
# Claude-Monitor-Mac.zip or a local build (file:///path/to/Claude-Monitor-Mac.zip).

ZIP_URL="${CLAUDE_MONITOR_ZIP:-https://github.com/boujois/waveshare-esp32-claude-monitor/releases/latest/download/Claude-Monitor-Mac.zip}"
TMP=""

main() {
  if [ "$(uname -s)" != "Darwin" ]; then
    echo "Claude Monitor's Mac helper only runs on macOS." >&2
    return 1
  fi
  if [ "$(id -u)" = 0 ]; then
    echo "Run this without sudo: Claude Monitor installs for your own user." >&2
    return 1
  fi
  TMP="$(mktemp -d)" || return 1
  trap 'rm -rf "$TMP"' EXIT

  echo "Downloading Claude Monitor..."
  if ! curl -fsSL --retry 2 "$ZIP_URL" -o "$TMP/Claude-Monitor-Mac.zip"; then
    echo "Couldn't download $ZIP_URL - check your internet connection and try again." >&2
    return 1
  fi
  if ! ditto -x -k "$TMP/Claude-Monitor-Mac.zip" "$TMP" 2>/dev/null \
      || [ ! -f "$TMP/Claude Monitor/Install.command" ]; then
    echo "The download from $ZIP_URL isn't a Claude Monitor package." >&2
    return 1
  fi
  CLAUDE_MONITOR_ONE_LINE=1 /bin/bash "$TMP/Claude Monitor/Install.command"
}

# Everything runs from here, so a download cut off halfway can't run half a script
main "$@"
