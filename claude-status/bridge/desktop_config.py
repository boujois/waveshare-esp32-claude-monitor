#!/usr/bin/env python3
"""Adds/removes the claude-status connector (chat_mcp.py) in the Claude app's
claude_desktop_config.json. Other settings and connectors are left untouched.

The Claude app rewrites this file while it's running (dropping outside edits), so
the change is only made while the app is closed - install-chat-connector.sh
quits the app, runs this, and reopens it."""

import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

CONFIG = Path.home() / "Library/Application Support/Claude/claude_desktop_config.json"
NAME = "claude-status"


def app_running():
    res = subprocess.run(["osascript", "-e", 'application "Claude" is running'], capture_output=True, text=True)
    return res.stdout.strip() == "true"


def main():
    action = sys.argv[1] if len(sys.argv) > 1 else "install"
    python = sys.argv[2] if len(sys.argv) > 2 else sys.executable
    if not CONFIG.parent.exists():
        print("Claude desktop app not found - skipping the chat connector")
        return
    if not os.environ.get("CLAUDE_STATUS_APP_CLOSED") and app_running():
        if os.environ.get("CLAUDE_STATUS_DEFER_CONNECTOR"):
            return  # Install.command asks about restarting the app itself
        flag = "" if action == "install" else " --remove"
        print("The Claude app is running and would overwrite this change. To "
              f"{'add' if action == 'install' else 'remove'} the chat connector, run this from Terminal.app:\n"
              f"  {Path(__file__).with_name('install-chat-connector.sh')}{flag}")
        return

    cfg = json.loads(CONFIG.read_text()) if CONFIG.exists() else {}
    if CONFIG.exists():
        shutil.copy2(CONFIG, CONFIG.with_name(f"claude_desktop_config.json.bak-claude-status-{int(time.time())}"))

    servers = cfg.get("mcpServers") or {}  # the app may store null here
    servers.pop(NAME, None)
    if action == "install":
        servers[NAME] = {"command": python, "args": [str(Path(__file__).with_name("chat_mcp.py"))]}
    if servers:
        cfg["mcpServers"] = servers
    else:
        cfg.pop("mcpServers", None)

    CONFIG.write_text(json.dumps(cfg, indent=2) + "\n")
    verb = "added to" if action == "install" else "removed from"
    print(f"chat connector {verb} {CONFIG.name}")


if __name__ == "__main__":
    main()
