#!/bin/sh
# Removes the launchd agents, the menu bar app and the Claude Code hooks added by install.sh.
DIR="$(cd "$(dirname "$0")" && pwd)"
. "$DIR/find_python.sh"

# Menu bar app first, so it doesn't sit there saying the helper stopped
# (pkill catches a copy opened by hand)
MENUBAR_LABEL="com.claude-status.menubar"
launchctl bootout "gui/$(id -u)/$MENUBAR_LABEL" 2>/dev/null || true
pkill -x -U "$(id -u)" "Claude Monitor" 2>/dev/null || true
rm -f "$HOME/Library/LaunchAgents/$MENUBAR_LABEL.plist"
rm -rf "$HOME/Applications/Claude Monitor.app"
rm -f "$HOME/Library/Logs/claude-monitor-menubar.log"
echo "menu bar app removed"

LABEL="com.claude-status.bridge"
launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
rm -f "$HOME/Library/LaunchAgents/$LABEL.plist"
echo "bridge agent removed"
"$PY" "$DIR/hooks.py" uninstall
"$PY" "$DIR/desktop_config.py" uninstall
rm -rf "$HOME/.claude/skills/update-claude-monitor"
