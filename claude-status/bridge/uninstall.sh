#!/bin/sh
# Removes the launchd agent and the Claude Code hooks added by install.sh.
DIR="$(cd "$(dirname "$0")" && pwd)"
. "$DIR/find_python.sh"
LABEL="com.claude-status.bridge"
launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
rm -f "$HOME/Library/LaunchAgents/$LABEL.plist"
echo "bridge agent removed"
"$PY" "$DIR/hooks.py" uninstall
"$PY" "$DIR/desktop_config.py" uninstall
rm -rf "$HOME/.claude/skills/update-claude-monitor"
