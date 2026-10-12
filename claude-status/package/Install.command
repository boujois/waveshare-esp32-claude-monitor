#!/bin/bash
# Claude Monitor - installer. Double-click to run (macOS asks you to allow it the first
# time), or let the one-line Terminal install (web/install.sh) download and run it.
# Puts the software on a display plugged in over USB, if it isn't on the Wi-Fi yet, then
# copies the helper to ~/Library/Application Support/Claude Monitor and sets everything up,
# including the menu bar app. Safe to run again to update or repair.

cd "$(dirname "$0")" || exit 1
SRC="$(pwd)/helper"
DEST="$HOME/Library/Application Support/Claude Monitor"
MENUBAR_APP="$HOME/Applications/Claude Monitor.app"  # put there by install.sh
# Set by the one-line install, which runs this in a terminal window that's already open:
# so no clearing it first, and no waiting for Enter at the end
ONE_LINE="${CLAUDE_MONITOR_ONE_LINE:-}"
FIRMWARE="$(pwd)/firmware"  # the display's software, and flash-display.sh to put it on over USB
INSTALLER_PAGE="https://boujois.github.io/waveshare-esp32-claude-monitor/"

bold() { printf '\n\033[1m%s\033[0m\n' "$1"; }
ask() { printf '%s [y/N] ' "$1"; read -r a; case "$a" in [yY]*) return 0 ;; *) return 1 ;; esac; }
# Like ask, but yes unless they say no. No answer at all (no terminal to type in) is a no.
ask_yes() { printf '%s [Y/n] ' "$1"; read -r a || return 1; case "$a" in [nN]*) return 1 ;; *) return 0 ;; esac; }
pause() { [ -n "$ONE_LINE" ] || read -r -p "$1"; }
app_running() { osascript -e 'application "Claude" is running' 2>/dev/null | grep -q true; }
# True when this runs in a terminal inside the Claude app, which restarting the app would close
inside_claude_app() {
  pid=$$
  while [ -n "$pid" ] && [ "$pid" -gt 1 ]; do
    case "$(ps -o comm= -p "$pid")" in */Claude.app/Contents/*) return 0 ;; esac
    pid=$(ps -o ppid= -p "$pid" | tr -d ' ')
  done
  return 1
}
# The menu bar app, giving launchd a few seconds to start it
menubar_running() {
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    pgrep -x -U "$(id -u)" "Claude Monitor" >/dev/null 2>&1 && return 0
    sleep 0.3
  done
  return 1
}
# The display answers on the Wi-Fi (any version), or answers with firmware that updates
# itself over Wi-Fi (v1.1.0 and later, which have /info)
display_online() { curl -4 -s -m 5 http://claude-status.local/ >/dev/null 2>&1; }
display_updates_over_wifi() { curl -4 -sf -m 5 http://claude-status.local/info >/dev/null 2>&1; }
# Waits up to 10 minutes for the display to join the Wi-Fi; Control-C stops waiting
wait_for_display() {
  local stop=0 start=$SECONDS
  trap 'stop=1' INT
  while [ "$stop" = 0 ] && [ $((SECONDS - start)) -lt 600 ]; do
    if display_online; then
      trap - INT
      printf '\n'
      return 0
    fi
    printf '\r  Still waiting (%d:%02d). Press Control-C to stop waiting. ' \
      $(((SECONDS - start) / 60)) $(((SECONDS - start) % 60))
    sleep 3
  done
  trap - INT
  printf '\n'
  return 1
}

[ -n "$ONE_LINE" ] || clear
bold "Claude Monitor - setup"
echo "This connects your Mac to the round desk display, so it can show what"
echo "Claude is doing and light up when Claude needs you."

# 1. The display: if it's plugged in over USB and not on the Wi-Fi yet, put the software on it
bold "Looking for the display..."
FLASHED=0
shopt -s nullglob
PORTS=(/dev/cu.usbmodem* /dev/cu.wchusbserial* /dev/cu.usbserial*)
shopt -u nullglob
if display_updates_over_wifi; then
  echo "It's already on your Wi-Fi, so it's set up. It gets new software over Wi-Fi."
elif [ ${#PORTS[@]} -gt 0 ]; then
  PORT="${PORTS[0]}"
  if [ ${#PORTS[@]} -gt 1 ]; then
    echo "More than one USB device is plugged in:"
    for i in "${!PORTS[@]}"; do echo "  $((i + 1))) ${PORTS[$i]}"; done
    printf 'Which one is the display? Type its number, or press Return to skip: '
    read -r n || n=""
    PORT=""
    case "$n" in
      ''|*[!0-9]*) ;;
      *) if [ "$n" -ge 1 ] && [ "$n" -le ${#PORTS[@]} ]; then PORT="${PORTS[$((n - 1))]}"; fi ;;
    esac
  fi
  if [ -n "$PORT" ]; then
    echo "Found a USB device: $PORT"
    echo "If that's the display, this puts the Claude Monitor software on it. Any Wi-Fi"
    echo "it's already set up with is kept."
    if ask_yes "Put the software on the display now?"; then
      if /bin/bash "$FIRMWARE/flash-display.sh" "$PORT"; then
        FLASHED=1
        bold "Now connect the display to your Wi-Fi"
        echo "The display restarts. When it shows a QR code, scan it with your phone, or"
        echo "join the Wi-Fi network \"Claude-Monitor-Setup\" on your phone. A page opens:"
        echo "pick your home Wi-Fi (2.4 GHz) and enter its password. If the display was on"
        echo "your Wi-Fi before, it reconnects by itself. Carry on here in the meantime."
      else
        echo
        echo "Couldn't put the software on the display (see above). Unplug it, plug it"
        echo "back in and run this again, or use the installer page in Chrome or Edge:"
        echo "  $INSTALLER_PAGE"
      fi
    fi
  fi
else
  echo "It isn't on your Wi-Fi yet, and nothing is plugged in over USB. If it's new,"
  echo "plug it into this Mac with a USB-C cable that carries data (not a charge-only"
  echo "one) and run this again. Carrying on with your Mac for now."
fi

# 2. Copy the helper somewhere permanent
mkdir -p "$DEST"
rm -rf "$DEST/Claude Monitor.app"  # replaced whole, not merged into the old one
cp -R "$SRC/." "$DEST/"
# Kept with the helper, so it's there to run later however this was installed
[ -f Uninstall.command ] && cp Uninstall.command "$DEST/" && chmod +x "$DEST/Uninstall.command"
xattr -dr com.apple.quarantine "$DEST" 2>/dev/null
chmod +x "$DEST"/*.sh

# 3. Background helper + Claude Code hooks (+ chat connector if the Claude app is closed)
bold "Installing the background helper..."
APP_WAS_RUNNING=0; app_running && APP_WAS_RUNNING=1
CLAUDE_STATUS_DEFER_CONNECTOR=1 "$DEST/install.sh" || { echo "Something went wrong - see the messages above."; pause "Press Enter to close."; exit 1; }
. "$DEST/find_python.sh"

# 4. Chat alerts need the Claude app restarted
if [ "$APP_WAS_RUNNING" = 1 ]; then
  bold "Chat alerts (optional)"
  echo "To alert you when a Claude chat needs you, the Claude app has to restart"
  echo "once. Your conversations will still be there."
  if inside_claude_app; then
    echo "This window is inside the Claude app, so restarting the app would close it."
    echo "To add chat alerts later, run this in the Terminal app:"
    echo "  \"$DEST/install-chat-connector.sh\""
  elif ask "Restart the Claude app now?"; then
    "$DEST/install-chat-connector.sh"
  else
    echo "Skipped. To add it later, run:"
    echo "  \"$DEST/install-chat-connector.sh\""
  fi
fi

# 5. Plan usage, night mode and the other options live in the menu bar app. When it isn't
# running (it needs macOS 13, and may not have started), ask about plan usage here instead
MENUBAR_RUNNING=0
if [ -d "$MENUBAR_APP" ] && menubar_running; then MENUBAR_RUNNING=1; fi
if [ "$MENUBAR_RUNNING" = 1 ]; then
  bold "Options are in the menu bar"
  echo "Claude Monitor's icon is near the clock at the top right of your screen: a"
  echo "small circle inside a circle. Click it, then click Connect under Plan usage"
  echo "to show how much of your 5-hour and weekly limits you've used (Pro and Max"
  echo "plans). Night mode, which darkens the screen overnight, is there too."
  echo "Can't see the icon? Your menu bar may be full. Open Claude Monitor from the"
  echo "Applications folder in your home folder (or search for it with Command-Space)"
  echo "to get the same options in a window."
elif command -v claude >/dev/null 2>&1 || [ -x "$HOME/.local/bin/claude" ]; then
  CLAUDE="$(command -v claude || echo "$HOME/.local/bin/claude")"
  bold "Plan usage rings (optional, Pro and Max plans)"
  echo "The display can show how much of your 5-hour and weekly limits you've used."
  if ask "Turn this on?"; then
    if ! "$CLAUDE" auth status 2>/dev/null | grep -q '"loggedIn": true'; then
      echo "Your browser will open so you can sign in to Claude..."
      "$CLAUDE" auth login
    fi
    "$PY" - "$DEST/config.json" <<'PYEOF'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1])
cfg = json.loads(p.read_text()) if p.exists() else {}
cfg["plan_usage"] = True
p.write_text(json.dumps(cfg, indent=2) + "\n")
PYEOF
    launchctl kickstart -k "gui/$(id -u)/com.claude-status.bridge"
    echo "Done. If macOS asks whether \"security\" can use \"Claude Code-credentials\","
    echo "click Always Allow."
  fi
fi

# 6. The one thing we can't do for them: the chat preferences line
PREF='When a reply ends with a question for me, or you need me to choose or decide something before you can continue, call the claude-status `waiting_for_user` tool as the last step, with a short version of the question and a 2-5 word title for the chat.'
printf '%s' "$PREF" | pbcopy
bold "One last step for chat questions"
echo "We've copied a sentence to your clipboard. In the Claude app, open"
echo "Settings > Profile and paste it into \"What personal preferences should"
echo "Claude consider in responses?\""

# 7. Is the display reachable?
bold "Checking the display..."
if display_online; then
  echo "Found it! It should show your Claude status within a few seconds."
elif [ "$FLASHED" = 1 ]; then
  echo "Waiting for the display to join your Wi-Fi. Finish Wi-Fi setup on your phone"
  echo "if you haven't yet."
  if wait_for_display; then
    echo "Found it! It should show your Claude status within a few seconds."
  else
    echo "It hasn't joined your Wi-Fi yet. Once you've picked your Wi-Fi on your phone,"
    echo "the display connects and the helper finds it by itself."
  fi
else
  echo "Couldn't reach the display yet. Make sure it's powered on and has joined"
  echo "your Wi-Fi (it shows \"Waiting for Mac\" when it has). The helper keeps"
  echo "trying in the background, so it'll connect by itself once the display is ready."
  echo "A new display needs its software first: plug it into this Mac with a USB-C"
  echo "data cable and run this again, or use the installer page in Chrome or Edge:"
  echo "  $INSTALLER_PAGE"
fi

bold "All set."
if [ "$MENUBAR_RUNNING" = 1 ]; then
  echo "Change options any time from the Claude Monitor icon in the menu bar."
fi
echo "To remove everything later, run this in Terminal:"
echo "  \"$DEST/Uninstall.command\""
pause "Press Enter to close this window."
