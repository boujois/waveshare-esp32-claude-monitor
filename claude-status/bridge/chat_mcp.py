#!/usr/bin/env python3
"""claude-status connector for Claude app chats (a minimal stdio MCP server).

Gives Claude one tool, `waiting_for_user`, which it calls when a reply ends with a
question or decision for you. The call is forwarded to the local bridge, which
lights up the desk display. Standard library only; installed into
claude_desktop_config.json by desktop_config.py (run via install.sh).
"""

import json
import sys
import urllib.request

BRIDGE_URL = "http://127.0.0.1:47823/chat"
SERVER_INFO = {"name": "claude-status", "version": "1.0.0"}

TOOL = {
    "name": "waiting_for_user",
    "description": (
        "Lights up the user's desk display to tell them you are waiting for their input. "
        "Call this as the LAST step of any reply that ends with a question for the user, "
        "asks them to choose or decide something, or cannot continue until they respond. "
        "Do not call it for replies that are complete and need nothing from the user."
    ),
    "inputSchema": {
        "type": "object",
        "properties": {
            "question": {
                "type": "string",
                "description": "The question or decision you need from the user, in under 80 characters.",
            },
            "chat_title": {
                "type": "string",
                "description": "A 2-5 word title for this conversation, so the user knows which chat is waiting.",
            },
        },
        "required": ["question"],
    },
    "annotations": {"readOnlyHint": True, "openWorldHint": False},
}


def notify(args):
    body = json.dumps({"question": args.get("question", ""), "chat_title": args.get("chat_title", "")}).encode()
    req = urllib.request.Request(BRIDGE_URL, data=body, headers={"Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=3):
            return "The user's display is now showing that you're waiting for them."
    except OSError:
        return "The desk display isn't reachable right now; carry on as normal."


def handle(msg):
    method, mid = msg.get("method"), msg.get("id")
    if method == "initialize":
        version = (msg.get("params") or {}).get("protocolVersion", "2025-06-18")
        result = {"protocolVersion": version, "capabilities": {"tools": {}}, "serverInfo": SERVER_INFO}
    elif method == "tools/list":
        result = {"tools": [TOOL]}
    elif method == "tools/call":
        params = msg.get("params") or {}
        if params.get("name") != TOOL["name"]:
            return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32602, "message": "Unknown tool"}}
        text = notify(params.get("arguments") or {})
        result = {"content": [{"type": "text", "text": text}], "isError": False}
    elif method == "ping":
        result = {}
    elif mid is None:
        return None  # notifications (e.g. notifications/initialized) need no reply
    else:
        return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": f"Method not found: {method}"}}
    return {"jsonrpc": "2.0", "id": mid, "result": result}


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            reply = handle(json.loads(line))
        except json.JSONDecodeError:
            reply = {"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "Parse error"}}
        if reply is not None:
            sys.stdout.write(json.dumps(reply) + "\n")
            sys.stdout.flush()


if __name__ == "__main__":
    main()
