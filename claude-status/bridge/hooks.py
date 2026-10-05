#!/usr/bin/env python3
"""Adds/removes the claude-status hooks in ~/.claude/settings.json.

Each hook is an async command that forwards the hook's JSON to the local bridge
with curl, so it never slows Claude Code down and is silent if the bridge is off.
Our entries are recognised by the CLAUDE_STATUS marker in the command.
"""

import json
import shutil
import sys
import time
from pathlib import Path

SETTINGS = Path.home() / ".claude" / "settings.json"
MARKER = "CLAUDE_STATUS"
COMMAND = (f"{MARKER}=1 curl -s -m 2 -X POST -H 'Content-Type: application/json' "
           "--data-binary @- http://127.0.0.1:47823/hook >/dev/null 2>&1 || true")

# event -> matcher (None = no matcher)
EVENTS = {
    "SessionStart": None,
    "SessionEnd": None,
    "UserPromptSubmit": None,
    "PreToolUse": "AskUserQuestion|ExitPlanMode",
    "PermissionRequest": "",
    "PermissionDenied": "",
    "PostToolUse": "",
    "PostToolUseFailure": "",
    "Notification": "",
    "Elicitation": "",
    "ElicitationResult": "",
    "Stop": None,
    "StopFailure": None,
}


def is_ours(group):
    return any(MARKER in (h.get("command") or "") for h in group.get("hooks", []))


def strip(settings):
    hooks = settings.get("hooks") or {}
    for event in list(hooks):
        hooks[event] = [g for g in hooks[event] if not is_ours(g)]
        if not hooks[event]:
            del hooks[event]
    if "hooks" in settings and not settings["hooks"]:
        del settings["hooks"]


def main():
    action = sys.argv[1] if len(sys.argv) > 1 else "install"
    settings = json.loads(SETTINGS.read_text()) if SETTINGS.exists() else {}
    backup = SETTINGS.with_name(f"settings.json.bak-claude-status-{int(time.time())}")
    if SETTINGS.exists():
        shutil.copy2(SETTINGS, backup)

    strip(settings)
    if action == "install":
        hooks = settings.setdefault("hooks", {})
        for event, matcher in EVENTS.items():
            group = {"hooks": [{"type": "command", "command": COMMAND, "async": True}]}
            if matcher is not None:
                group = {"matcher": matcher, **group}
            hooks.setdefault(event, []).append(group)

    SETTINGS.write_text(json.dumps(settings, indent=2) + "\n")
    print(f"hooks {'installed' if action == 'install' else 'removed'} in {SETTINGS} (backup: {backup.name})")


if __name__ == "__main__":
    main()
