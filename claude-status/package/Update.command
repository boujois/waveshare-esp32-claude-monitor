#!/bin/bash
# Claude Monitor - updates the Mac helper and the display to the latest release.
# Double-click to run (the first time, right-click > Open).
DEST="$HOME/Library/Application Support/Claude Monitor"
printf '\n\033[1mClaude Monitor - update\033[0m\n\n'
if [ ! -x "$DEST/update.sh" ]; then
  echo "This Mac has no Claude Monitor install that can update itself (it's either not"
  echo "installed, or from before v1.1.0). Right-click Install.command and choose Open"
  echo "instead - it installs the latest version over the top."
else
  "$DEST/update.sh"
fi
echo
read -r -p "Press Enter to close this window."
