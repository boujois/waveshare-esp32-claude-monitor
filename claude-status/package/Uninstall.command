#!/bin/bash
# Claude Monitor - removes the Mac helper, hooks, chat connector and menu bar app.
# Double-click to run (macOS asks you to allow it the first time). The installer also keeps a
# copy in ~/Library/Application Support/Claude Monitor, to run from Terminal.

DEST="$HOME/Library/Application Support/Claude Monitor"
printf '\n\033[1mClaude Monitor - uninstall\033[0m\n'

if [ ! -d "$DEST" ]; then
  echo "Claude Monitor isn't installed."
  read -r -p "Press Enter to close."
  exit 0
fi

"$DEST/uninstall.sh"
if osascript -e 'application "Claude" is running' 2>/dev/null | grep -q true; then
  printf 'Removing the chat connector restarts the Claude app (your conversations stay). Do it now? [y/N] '
  read -r a
  case "$a" in [yY]*) "$DEST/install-chat-connector.sh" --remove ;; esac
fi
rm -rf "$DEST"

echo
echo "Removed. You can also delete the claude-status line from Claude > Settings > Profile."
read -r -p "Press Enter to close."
