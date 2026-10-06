---
name: setup-claude-monitor
description: Set up the Waveshare ESP32-S3 Claude status display from scratch, update it, or repair a setup. Covers checking prerequisites, flashing the published firmware (or building it), Wi-Fi setup from a phone, installing the Mac helper, hooks and chat connector, plan usage rings, and an end-to-end test. Use when someone asks to set up, install, update, flash or configure the display or "Claude monitor", change its Wi-Fi, or fix a display that isn't showing anything.
---

# Set up the Claude status display

You're helping someone get the round desk display from this project working: a Waveshare ESP32-S3-LCD-1.28 that shows Claude Code status and alerts when Claude needs them.

- **Project:** https://github.com/boujois/waveshare-esp32-claude-monitor. It's public, so anyone can download its releases.
- **Latest release:** https://github.com/boujois/waveshare-esp32-claude-monitor/releases/latest
- **Firmware installer site:** https://boujois.github.io/waveshare-esp32-claude-monitor/, which also serves the current firmware files.

`README.md` is the human-facing guide; this skill is the procedure for you to follow. Work through the steps in order, verify each one before moving on, and keep the person informed in plain language. Many of them won't be developers.

**Ground rules**
- Ask for things only the person knows, such as whether they want the optional parts. (Wi-Fi is entered on their phone, and the timezone comes from the Mac.) Find everything else yourself.
- Some steps **must be done by the person**, not you. They're marked 🙋. Explain the step, give them the exact command, and wait.
- Never read, print or search for Claude sign-in tokens or Keychain contents, and never read `~/Library/Application Support/Claude/claude_desktop_config.json` values beyond key names (other connectors keep secrets there).
- Never commit `secrets.h` or `config.json` (both are git-ignored), and never print `config.json`'s `device_key`, the display's pairing key.
- If something is broken in the project itself, rather than in their setup, point them to https://github.com/boujois/waveshare-esp32-claude-monitor/issues.

## 0. Check the basics

| Check | Command | If it's missing |
|---|---|---|
| macOS | `uname -s` should print `Darwin` | Stop. The Mac helper uses launchd and the Keychain, so this project is macOS-only. |
| Python 3 | `xcode-select -p` (Apple's Python) or `which python3` | Run `xcode-select --install`. 🙋 They click **Install** in the dialog and wait for it to finish. The install scripts find a usable Python themselves (Homebrew's or Apple's). |
| esptool | `which esptool.py esptool` | Needed to flash the published firmware in step 3. Install with `brew install esptool` if Homebrew is there (`which brew`); otherwise `python3 -m pip install --user esptool`, which installs `esptool.py` into the user's Python `bin` folder. |
| Claude CLI | `which claude` | Only needed for the plan usage rings (step 6). |

PlatformIO (`brew install platformio`) is only needed if they want to build the firmware from source (step 3, option B).

## 1. Find the board

Ask them to plug the display into the Mac with a **data** USB-C cable (charge-only cables are common and won't work). Then:

```bash
ls /dev/cu.usbmodem* /dev/cu.wchusbserial* 2>/dev/null
```

Then confirm it's the right chip: `esptool.py --port <PORT> flash_id`. You're expecting `ESP32-S3`, `Embedded PSRAM 2MB` and `16MB` flash.

- **No port appears:** try another cable or USB port. The board uses a WCH CH343 USB-serial chip (USB vendor ID `0x1a86`), which macOS supports without drivers.
- **Make sure it's the non-touch board** (ESP32-S3-LCD-1.28 or the "-B" CNC-case version). The touch version (ESP32-S3-Touch-LCD-1.28) uses different reset and backlight pins (RST 14, BL 2), and the screen will stay blank with this firmware.

## 2. Wi-Fi and timezone

There's nothing to configure in code. **Wi-Fi is set up from a phone after flashing** (step 3), and the **timezone comes from the Mac** automatically, once the helper connects in step 4.

## 3. Flash the firmware

**Option A: published firmware (default).** This is fast, with nothing to build. Download the current release's firmware parts from the installer site and flash them at their offsets. Writing them as separate parts keeps any Wi-Fi the board has already saved:

```bash
FW=$(mktemp -d) && cd "$FW" && for f in bootloader partitions boot_app0 firmware; do curl -fsSLO "https://boujois.github.io/waveshare-esp32-claude-monitor/$f.bin"; done
esptool.py --chip esp32s3 --port <PORT> --baud 921600 write_flash 0x0 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin
```

Use `esptool` instead of `esptool.py` if that's the name installed. Then check the version: `https://boujois.github.io/waveshare-esp32-claude-monitor/manifest.json` shows the release version, and after Wi-Fi is set up, `curl -s http://claude-status.local/` reports it as well.

> If the person would rather click a button themselves, they can open the installer site in **Chrome or Edge** and click **Install Claude Monitor** instead. It does the same thing.

**Option B: build from source.** Use this if they've changed the code, or they ask for it. It needs PlatformIO, and the first build downloads the toolchain, so use a long timeout:

```bash
cd claude-status/firmware && pio run -t upload
```

PlatformIO auto-detects the port; if several boards are plugged in, add `upload_port = <PORT>` to `platformio.ini`. An optional git-ignored `src/secrets.h` (copy `secrets.h.example`) pre-fills Wi-Fi for dev builds. It's never used in release builds.

**Then, either way:**

1. 🙋 **Wi-Fi setup from their phone:** the display shows a QR code and the network name `Claude-Monitor-Setup`. They join it (scanning the code works on most phones), pick their Wi-Fi in the page that opens, and enter the password. Only **2.4 GHz** networks work; if the page doesn't open, they go to `192.168.4.1`. If the board already has a saved network, it skips this and connects.
2. Read the serial log for about 15 seconds to confirm it joined Wi-Fi. Use a short Python script with pyserial, which comes with esptool. You're looking for:
   ```
   Wi-Fi connected, IP 192.168.x.x
   mDNS: claude-status.local
   ```
   - **The log says `Wi-Fi setup open`:** it's waiting for phone setup. A wrong password reopens setup. Check the network is 2.4 GHz.
   - **Flashing fails:** close anything else holding the serial port (a serial monitor, the Arduino IDE), then retry.
3. Check the Mac can reach it with `curl -s http://claude-status.local/`. If the name doesn't resolve, use the IP from the log, and remember it for step 4.

The screen should now say **"Waiting for Mac"**. You can see exactly what's on the screen with `curl -s http://claude-status.local/screen.bmp -o /tmp/screen.bmp` and then reading the image. Use this whenever you want to verify the display yourself instead of asking the person.

## 4. Install the Mac helper and Claude Code hooks

Install the helper from the **latest release** into a permanent folder, the same one the downloadable `Install.command` uses. Then it keeps working even if they delete the downloaded project, and it can update itself later:

```bash
DEST="$HOME/Library/Application Support/Claude Monitor"; Z=$(mktemp -d)
curl -fsSL -o "$Z/mac.zip" https://github.com/boujois/waveshare-esp32-claude-monitor/releases/latest/download/Claude-Monitor-Mac.zip
unzip -q "$Z/mac.zip" -d "$Z" && mkdir -p "$DEST" && cp -R "$Z/Claude Monitor/helper/." "$DEST/" && chmod +x "$DEST"/*.sh
"$DEST/install.sh"
```

This installs a launchd agent (`com.claude-status.bridge`), adds async hooks to `~/.claude/settings.json` (backed up first), adds the chat connector to the Claude app if the app is closed, and adds the global `update-claude-monitor` skill. From here on, the helper's files and `config.json` live in `$DEST`. The helper pairs with the display by itself within a minute, which is what makes Wi-Fi firmware updates possible. Only if they're changing the helper's code should you install from `claude-status/bridge` instead; that's a "dev" install, which can't update itself.

- **If mDNS didn't work in step 3**, create `"$DEST/config.json"` with `{"device_host": "<IP>"}` and restart the helper with `launchctl kickstart -k gui/$(id -u)/com.claude-status.bridge`. Suggest a DHCP reservation in their router so the IP doesn't change.
- **Verify the helper:**
  - `tail -5 ~/Library/Logs/claude-status-bridge.log` should show `pushing to claude-status.local (…)`.
  - The display should switch to the status screen. Confirm with a screenshot.
- **Verify the hooks:** run `curl -s http://127.0.0.1:47823/state | jq .hook_events_seen`, then run any tool, then check again. The number should go up. Hooks load in new sessions; if this session started before the install, the count may stay at 0 until the session is restarted.

## 5. Chat connector (only if they use the Claude desktop app)

The installer only adds the connector while the Claude app is closed, because the running app rewrites its own config file and would drop the change. If the installer says the app is running:

🙋 **They must run this from Terminal.app themselves**, because it quits the Claude app, and running it from inside the app would kill this session partway through:

```bash
"$HOME/Library/Application Support/Claude Monitor/install-chat-connector.sh"
```

Their sessions are restored when the app reopens. After that, the connector should be running: `pgrep -fl chat_mcp.py`.

🙋 **They add one line to Claude → Settings → Profile → personal preferences** (you can't change their claude.ai settings):

> When a reply ends with a question for me, or you need me to choose or decide something before you can continue, call the claude-status `waiting_for_user` tool as the last step, with a short version of the question and a 2-5 word title for the chat.

## 6. Plan usage rings (optional, Pro/Max plans only)

The rings use the **terminal** `claude` login, which is separate from the desktop app's sign-in.

1. 🙋 They run `claude auth login`, which opens the browser. Check with `claude auth status` that `"loggedIn": true`. That command doesn't print the token.
2. Set `"plan_usage": true` in `"$HOME/Library/Application Support/Claude Monitor/config.json"`. Merge it with any existing keys, such as `device_host`, rather than overwriting the file. Then restart the helper:
   ```bash
   launchctl kickstart -k gui/$(id -u)/com.claude-status.bridge
   ```
3. 🙋 macOS asks whether `security` can read "Claude Code-credentials". They should click **Always Allow**.
4. Verify with `curl -s http://127.0.0.1:47823/state | jq .usage`. You should see `h5` and `d7` percentages.

The same sign-in also powers the **reply check**. When a Claude Code turn ends without an obvious question, the helper asks Haiku whether the reply is waiting on them, so requests like "after you approve, I'll…" still alert. Without the sign-in, only phrase matching runs. `"check_replies": false` in `config.json` turns it off.

The helper renews an expired token by itself with one tiny `claude -p` call. Don't suggest `claude setup-token`: those tokens get HTTP 403 from the usage endpoint.

## 7. End-to-end test

1. Use your question tool to ask them something harmless, such as how the display looks. While the question is open, the display should show a pulsing orange **Question** alert with this session's name. Confirm by taking a screenshot yourself, and ask them too. It should clear after they answer.
2. If they set up chats: ask them to open a Chat and say *"ask me a question"*, choosing **Always allow** the first time. The helper log should show `chat: question from …`.

Finish with a short summary of what's set up, anything they skipped, and where the README covers configuration and troubleshooting.

## Updating

Use the `update-claude-monitor` skill, or run `"$HOME/Library/Application Support/Claude Monitor/update.sh"` (`--check` to only compare versions). It updates the Mac helper from the latest release, keeping `config.json`, and the display over Wi-Fi. A display on v1.0.x firmware needs one last USB flash (step 3, option A) before Wi-Fi updates work; a Mac install without `update.sh` needs step 4 again.

## Changing Wi-Fi later

Run `curl -X POST http://claude-status.local/wifi/reset` yourself. Or 🙋 they unplug and replug the display 3 times in a row, each within 10 seconds of the last. The BOOT button also works if it's reachable, but the CNC metal case usually covers it. Either way, the display restarts into Wi-Fi setup and they repeat the phone setup.

A dev build with a filled-in `secrets.h` reconnects to that network instead of opening setup.

## Uninstalling

Run `"$HOME/Library/Application Support/Claude Monitor/uninstall.sh"` (it also removes the update skill), then delete that folder. If the Claude app is open, the uninstaller says to run `install-chat-connector.sh --remove` from Terminal.app to remove the connector. They should also delete the personal-preferences line themselves.

## For maintainers

Releases are built by `.github/workflows/release.yml` when a `v*` tag is pushed. It attaches `Claude-Monitor-Mac.zip` and `claude-monitor-firmware.bin` to the release and publishes the installer site to GitHub Pages. The `github-pages` environment allows `main` and `v*` tags. `tools/build-release.sh` builds the same files locally into `dist/`.
