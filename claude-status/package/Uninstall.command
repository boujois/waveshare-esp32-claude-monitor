#!/bin/bash
# Claude Monitor - removes the Mac helper, hooks and chat connector.
# Double-click to run (the first time, right-click > Open).

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
