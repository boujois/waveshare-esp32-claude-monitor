---
name: update-claude-monitor
description: Update Claude Monitor, the round ESP32-S3 desk display that shows Claude Code status, to the latest release. Updates the Mac helper and the display's firmware (over Wi-Fi). Use when someone asks to update, upgrade or check the version of their Claude Monitor, Claude status display or desk display, or when the display shows "Update" with a version number.
---

# Update Claude Monitor

Claude Monitor's Mac helper lives in `~/Library/Application Support/Claude Monitor`. Its updater checks GitHub for the latest release, then updates the Mac helper (keeping `config.json`), replaces the menu bar app in `~/Applications/Claude Monitor.app` (installing it if it wasn't there yet), and sends new firmware to the display over Wi-Fi. The menu bar panel's **Update to v…** button runs the same updater in Terminal.

1. **Check what's installed:**
   ```bash
   "$HOME/Library/Application Support/Claude Monitor/update.sh" --check
   ```
   It prints the latest release, the Mac helper's version and the display's version. If the folder doesn't exist, Claude Monitor isn't installed (or was installed from a git checkout). Point them to https://github.com/boujois/waveshare-esp32-claude-monitor#install.

2. **Update:** if anything is behind, tell them what will change, then run:
   ```bash
   "$HOME/Library/Application Support/Claude Monitor/update.sh"
   ```
   Use a timeout of at least 5 minutes. The display shows an "Updating" progress ring and restarts by itself, lighting up for it even during night mode; tell them not to unplug it meanwhile. The menu bar app restarts too.

3. **Read the result and help with anything that didn't finish:**
   - **"needs USB":** the display runs firmware from before Wi-Fi updates (older than v1.1.0). 🙋 They open https://boujois.github.io/waveshare-esp32-claude-monitor/ in Chrome or Edge, plug the display into the Mac and click **Install**. Saved Wi-Fi is kept. After that, updates go over Wi-Fi.
   - **"paired with a different Mac":** 🙋 they power the display on 3 times in a row, each within 10 seconds of the last. That resets its Wi-Fi and pairing. They then redo Wi-Fi setup from their phone (join `Claude-Monitor-Setup`), and you run the update again.
   - **"Can't reach the display":** check it's powered and shows the status screen, not "Wi-Fi setup". Then try `curl -s http://claude-status.local/info`.
   - **"developer install":** the helper runs from a git checkout. Update it with `git pull` and that checkout's `claude-status/bridge/install.sh`.
   - **"An update is already running in another Terminal window":** only one update runs at a time. Let the other one finish (perhaps started from the menu bar's Update button), then check again.
   - **Lines starting with `!`** are warnings from the reinstall, such as the menu bar app not starting. Everything else updated; pass the warning on. The app should start at the next login, or they can open it from `~/Applications`.

4. **Confirm:** run `update.sh --check` again. Both versions should match the latest release. The display's "Update" notice disappears within a minute.

Night mode runs on the display, so it needs the display's firmware updated too, not just the Mac helper. If the menu bar app says *Update the display to use night mode*, the display is still on older firmware: run the update, and help with whatever step 3 reports.

Never read or print `config.json`'s `device_key`; it's the display's pairing key.
