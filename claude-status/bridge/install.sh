#!/bin/sh
# Installs the Claude status bridge as a launchd agent, adds the Claude Code hooks and
# installs the menu bar app. Re-run safely at any time; ./uninstall.sh reverses it.
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

# Menu bar app: plan usage, night mode and the other options, over the helper's local API.
# It's optional, so nothing here fails the install. CLAUDE_STATUS_NO_MENUBAR=1 skips it.
MENUBAR_LABEL="com.claude-status.menubar"
MENUBAR_PLIST="$HOME/Library/LaunchAgents/$MENUBAR_LABEL.plist"
MENUBAR_APP="$HOME/Applications/Claude Monitor.app"
MENUBAR_LOG="$HOME/Library/Logs/claude-monitor-menubar.log"
MENUBAR_TMP=""

# Runs from an if, where set -e is off, so every step checks for itself
install_menubar() {
  osver="$(sw_vers -productVersion 2>/dev/null)"
  if ! [ "${osver%%.*}" -ge 13 ] 2>/dev/null; then
    echo "menu bar app skipped: it needs macOS 13 or later"
    return 0
  fi
  # An update.py from before the menu bar app copies plain files only, so releases
  # carry the app zipped as well
  if [ ! -d "$DIR/Claude Monitor.app" ] && [ -f "$DIR/Claude Monitor.app.zip" ]; then
    ditto -x -k "$DIR/Claude Monitor.app.zip" "$DIR" || return 1
  fi
  if [ -d "$DIR/Claude Monitor.app" ]; then
    src="$DIR/Claude Monitor.app"  # release package: built already
  elif [ -f "$DIR/../menubar/build.sh" ] && xcode-select -p >/dev/null 2>&1 \
      && xcrun swiftc --version >/dev/null 2>&1; then
    # git checkout: build it for this Mac (xcode-select first, or xcrun offers to install tools)
    version=dev
    if [ -s "$DIR/VERSION" ]; then version="$(cat "$DIR/VERSION")"; fi
    MENUBAR_TMP="$(mktemp -d "${TMPDIR:-/tmp}/claude-monitor.XXXXXX")" || return 1
    echo "building the menu bar app..."
    if ! MENUBAR_HOST_ARCH_ONLY=1 /bin/sh "$DIR/../menubar/build.sh" "$MENUBAR_TMP" "$version"; then
      echo "Couldn't build the menu bar app. Building it needs Swift 6, which comes with Xcode or" >&2
      echo "the Command Line Tools 16 or later (on macOS 14 or later)." >&2
      return 1
    fi
    src="$MENUBAR_TMP/Claude Monitor.app"
  else
    echo "menu bar app skipped: there's no built app here, and building one needs Xcode or the Command Line Tools (16 or later)"
    return 0
  fi
  if [ ! -x "$src/Contents/MacOS/Claude Monitor" ]; then
    echo "$src is incomplete" >&2
    return 1
  fi

  # Stop the old copy before replacing it. pkill catches one opened by hand, which would
  # otherwise keep the new one from starting (the app quits if it's already running)
  launchctl bootout "gui/$(id -u)/$MENUBAR_LABEL" 2>/dev/null || true
  pkill -x -U "$(id -u)" "Claude Monitor" 2>/dev/null || true
  # Neither waits for it to quit, and a new copy started while it's still going would leave
  # straight away, so give it up to 5 seconds
  i=0
  while [ "$i" -lt 50 ] && pgrep -x -U "$(id -u)" "Claude Monitor" >/dev/null 2>&1; do
    sleep 0.1
    i=$((i + 1))
  done
  mkdir -p "$HOME/Applications" || return 1
  rm -rf "$MENUBAR_APP"
  if ! cp -R "$src" "$MENUBAR_APP"; then
    rm -f "$MENUBAR_PLIST"  # the app it starts is gone
    return 1
  fi
  xattr -dr com.apple.quarantine "$MENUBAR_APP" 2>/dev/null || true

  # The helper's port, in case config.json moves it from the default
  port="$("$PY" -c 'import json, sys
p = int(json.load(open(sys.argv[1])).get("listen_port", 47823))
print(p if 0 < p < 65536 else 47823)' "$DIR/config.json" 2>/dev/null)" || port=47823

  # Starts at login. Quit keeps it quit until the next login (a clean exit); a crash restarts it
  cat > "$MENUBAR_PLIST" <<PLIST || return 1
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$MENUBAR_LABEL</string>
  <key>ProgramArguments</key>
  <array><string>$MENUBAR_APP/Contents/MacOS/Claude Monitor</string><string>--port</string><string>$port</string></array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><dict><key>SuccessfulExit</key><false/></dict>
  <key>LimitLoadToSessionType</key><string>Aqua</string>
  <key>ProcessType</key><string>Interactive</string>
  <key>StandardOutPath</key><string>$MENUBAR_LOG</string>
  <key>StandardErrorPath</key><string>$MENUBAR_LOG</string>
</dict>
</plist>
PLIST

  # Same retry as the bridge's above
  for attempt in 1 2 3 4 5 6 7 8 9 10; do
    launchctl bootstrap "gui/$(id -u)" "$MENUBAR_PLIST" 2>/dev/null && break
    if [ "$attempt" = 10 ]; then
      # It's installed either way, and launchd should start it at the next login. Until
      # then, open it directly. Warnings go to stderr, which update.py passes on
      echo "menu bar app installed ($MENUBAR_APP), log: $MENUBAR_LOG"
      if open -a "$MENUBAR_APP" --args --port "$port" 2>/dev/null; then
        echo "(launchctl wouldn't start it just now, so it was opened directly)"
      else
        echo "The menu bar app is installed but couldn't be started just now (launchctl bootstrap" >&2
        echo "kept failing). It should start at your next login, or open it from ~/Applications." >&2
      fi
      return 0
    fi
    sleep 1
  done
  echo "menu bar app installed ($MENUBAR_APP), log: $MENUBAR_LOG"
}

if [ "$CLAUDE_STATUS_NO_MENUBAR" = 1 ]; then
  echo "menu bar app skipped (CLAUDE_STATUS_NO_MENUBAR=1)"
elif ! install_menubar; then
  echo "The menu bar app wasn't installed (see above), but everything else is set up." >&2
fi
if [ -n "$MENUBAR_TMP" ]; then rm -rf "$MENUBAR_TMP"; fi

