#!/bin/sh
# Adds (or with --remove, removes) the claude-status connector in the Claude app.
# The app rewrites its config file while it's running, which would drop the change,
# so this quits the app, edits the config, then reopens it.
# Run it from Terminal.app, not from a terminal inside the Claude app.
DIR="$(cd "$(dirname "$0")" && pwd)"
. "$DIR/find_python.sh"
ACTION=install; [ "$1" = "--remove" ] && ACTION=uninstall

running() { osascript -e 'application "Claude" is running' 2>/dev/null | grep -q true; }

WAS_RUNNING=0
if running; then
  WAS_RUNNING=1
  echo "Quitting the Claude app (your sessions will be there when it reopens)..."
  osascript -e 'quit app "Claude"'
  while running; do sleep 1; done
  sleep 1
fi
CLAUDE_STATUS_APP_CLOSED=1 "$PY" "$DIR/desktop_config.py" "$ACTION" "$PY"
if [ "$WAS_RUNNING" = 1 ]; then
  echo "Reopening the Claude app..."
  open -a Claude
fi
