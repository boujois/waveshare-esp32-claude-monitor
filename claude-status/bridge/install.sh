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
# bootout returns before the old helper has fully stopped, and bootstrap fails
# ("5: Input/output error") until it has, so retry for a few seconds
for attempt in 1 2 3 4 5 6 7 8 9 10; do
  launchctl bootstrap "gui/$(id -u)" "$PLIST" 2>/dev/null && break
  if [ "$attempt" = 10 ]; then
    echo "Couldn't start the helper: launchctl bootstrap kept failing" >&2
    exit 1
  fi
  sleep 1
done
echo "bridge agent installed ($PLIST), log: $LOG"

"$PY" "$DIR/hooks.py" install
"$PY" "$DIR/desktop_config.py" install "$PY"

# Global /update-claude-monitor skill, so any Claude Code session can run updates
if [ -f "$DIR/update-skill.md" ]; then
  mkdir -p "$HOME/.claude/skills/update-claude-monitor"
  cp "$DIR/update-skill.md" "$HOME/.claude/skills/update-claude-monitor/SKILL.md"
  echo "update skill installed (~/.claude/skills/update-claude-monitor)"
fi

