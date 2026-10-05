#!/bin/bash
# Claude Monitor - Mac installer. Double-click to run (the first time, right-click > Open).
# Copies the helper to ~/Library/Application Support/Claude Monitor and sets everything up.
# Safe to run again to update or repair.

cd "$(dirname "$0")" || exit 1
SRC="$(pwd)/helper"
DEST="$HOME/Library/Application Support/Claude Monitor"

bold() { printf '\n\033[1m%s\033[0m\n' "$1"; }
ask() { printf '%s [y/N] ' "$1"; read -r a; case "$a" in [yY]*) return 0 ;; *) return 1 ;; esac; }
app_running() { osascript -e 'application "Claude" is running' 2>/dev/null | grep -q true; }

clear
bold "Claude Monitor - Mac setup"
echo "This connects your Mac to the round desk display, so it can show what"
echo "Claude is doing and light up when Claude needs you."

# 1. Copy the helper somewhere permanent
mkdir -p "$DEST"
cp -R "$SRC/." "$DEST/"
xattr -dr com.apple.quarantine "$DEST" 2>/dev/null
chmod +x "$DEST"/*.sh

# 2. Background helper + Claude Code hooks (+ chat connector if the Claude app is closed)
bold "Installing the background helper..."
APP_WAS_RUNNING=0; app_running && APP_WAS_RUNNING=1
CLAUDE_STATUS_DEFER_CONNECTOR=1 "$DEST/install.sh" || { echo "Something went wrong - see the messages above."; read -r -p "Press Enter to close."; exit 1; }
. "$DEST/find_python.sh"

# 3. Chat alerts need the Claude app restarted
if [ "$APP_WAS_RUNNING" = 1 ]; then
  bold "Chat alerts (optional)"
  echo "To alert you when a Claude chat needs you, the Claude app has to restart"
  echo "once. Your conversations will still be there."
  if ask "Restart the Claude app now?"; then
    "$DEST/install-chat-connector.sh"
  else
    echo "Skipped. To add it later, run:"
    echo "  \"$DEST/install-chat-connector.sh\""
  fi
fi

# 4. Plan usage rings
if command -v claude >/dev/null 2>&1 || [ -x "$HOME/.local/bin/claude" ]; then
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

# 5. The one thing we can't do for them: the chat preferences line
PREF='When a reply ends with a question for me, or you need me to choose or decide something before you can continue, call the claude-status `waiting_for_user` tool as the last step, with a short version of the question and a 2-5 word title for the chat.'
printf '%s' "$PREF" | pbcopy
bold "One last step for chat questions"
echo "We've copied a sentence to your clipboard. In the Claude app, open"
echo "Settings > Profile and paste it into \"What personal preferences should"
echo "Claude consider in responses?\""

# 6. Is the display reachable?
bold "Checking the display..."
if curl -s -m 5 http://claude-status.local/ >/dev/null; then
  echo "Found it! It should show your Claude status within a few seconds."
else
  echo "Couldn't reach the display yet. Make sure it's powered on and has joined"
  echo "your Wi-Fi (it shows \"Waiting for Mac\" when it has). The helper keeps"
  echo "trying in the background, so it'll connect by itself once the display is ready."
fi

bold "All set."
read -r -p "Press Enter to close this window."
