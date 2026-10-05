#!/bin/sh
# Installs the Claude status bridge as a launchd agent and adds the Claude Code hooks.
# Re-run safely at any time; ./uninstall.sh reverses it.
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
. "$DIR/find_python.sh"
LABEL="com.claude-status.bridge"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/Library/Logs/claude-status-bridge.log"

mkdir -p "$HOME/Library/LaunchAgents"
cat > "$PLIST" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key>
  <array><string>$PY</string><string>$DIR/claude_status_bridge.py</string></array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>$LOG</string>
  <key>StandardErrorPath</key><string>$LOG</string>
</dict>
</plist>
PLIST

launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$PLIST"
echo "bridge agent installed ($PLIST), log: $LOG"

"$PY" "$DIR/hooks.py" install
"$PY" "$DIR/desktop_config.py" install "$PY"

