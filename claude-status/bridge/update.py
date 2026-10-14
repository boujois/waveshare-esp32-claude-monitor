#!/usr/bin/env python3
"""Updates Claude Monitor to the latest GitHub release.

    update.sh                 update the Mac helper and the display, if they're behind
    update.sh --check         only report what's installed and what's available
    update.sh --firmware      update the display even if it's on the latest version

The Mac helper and its menu bar app are updated from the release's Claude-Monitor-Mac.zip
(your config.json is kept). The display is updated over Wi-Fi from the release's
claude-monitor-app.bin, using the key the helper paired with it. Standard library only.
"""

import argparse
import fcntl
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

import claude_status_bridge as cs

HERE = Path(__file__).resolve().parent
KEEP = {"config.json"}  # never overwritten by an update


def say(msg=""):
    print(msg, flush=True)


def download(url):
    req = urllib.request.Request(url, headers={"User-Agent": "claude-status-display"})
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read()


def extract(zip_path, dest):
    """Like ZipFile.extractall, but keeps file permissions and symlinks, which extractall
    drops and the menu bar app needs (its executable, mainly)."""
    links = []
    with zipfile.ZipFile(zip_path) as z:
        for info in z.infolist():
            mode = info.external_attr >> 16
            path = z.extract(info, dest)
            if stat.S_ISLNK(mode):
                links.append((path, os.fsdecode(Path(path).read_bytes())))  # the contents are the target
                os.remove(path)
            elif mode & 0o777 and not info.is_dir():
                os.chmod(path, mode & 0o777)
    # Links last, so nothing gets extracted through one
    for path, target in links:
        os.symlink(target, path)


def replace_dir(src, dest):
    if dest.is_symlink() or dest.is_file():
        dest.unlink()
    elif dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(src, dest, symlinks=True)


def update_helper(release):
    """Replaces the helper files with the release's and re-runs install.sh."""
    zip_url = release["assets"].get("Claude-Monitor-Mac.zip")
    if not zip_url:
        say("✗ The release has no Claude-Monitor-Mac.zip")
        return False
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "mac.zip"
        path.write_bytes(download(zip_url))
        extract(path, tmp)
        src = Path(tmp) / "Claude Monitor" / "helper"
        if not src.is_dir():
            say("✗ Unexpected zip layout")
            return False
        for f in src.iterdir():
            if f.name in KEEP:
                continue
            if f.suffix == ".app" and f.is_dir() and not f.is_symlink():
                # App bundles (the menu bar app) are replaced whole; install.sh installs them
                replace_dir(f, HERE / f.name)
            elif f.is_file():
                shutil.copy2(f, HERE / f.name)
                if f.suffix == ".sh":
                    os.chmod(HERE / f.name, 0o755)
    env = dict(os.environ, CLAUDE_STATUS_DEFER_CONNECTOR="1")  # the connector's path doesn't change
    res = subprocess.run([str(HERE / "install.sh")], env=env, capture_output=True, text=True)
    if res.returncode != 0:
        say("✗ Reinstalling the helper failed:\n" + (res.stderr or res.stdout))
        return False
    # install.sh carries on when an optional part (the menu bar app) has trouble, saying why on stderr
    if res.stderr.strip():
        say("! " + res.stderr.strip().replace("\n", "\n  "))
    return True


def wait_for_display(device, want, timeout=90):
    """Waits for the display to come back after restarting, running version `want`."""
    end = time.time() + timeout
    while time.time() < end:
        time.sleep(3)
        try:
            device.ip = None  # re-resolve; the IP can change across a restart
            if device.info().get("version") == want:
                return True
        except (OSError, ValueError):
            pass
    return False


def update_display(release, cfg, force):
    device = cs.Device(cfg["device_host"])
    try:
        info = device.info()
    except urllib.error.HTTPError as e:
        if e.code == 404:
            say("! The display's firmware is from before Wi-Fi updates, so this one update needs USB:")
            say(f"  open {cs.INSTALLER_SITE} in Chrome or Edge, plug the display in and click Install.")
            say("  After that, updates go over Wi-Fi.")
            return False
        raise
    except OSError as e:
        say(f"! Can't reach the display at {cfg['device_host']} ({e}). Is it on and connected to Wi-Fi?")
        return False

    current, latest = info.get("version"), release["tag"]
    if not force and not cs.is_newer(latest, current):
        say(f"✓ Display firmware is up to date ({current})")
        return True

    key = cs.device_key(cfg)
    if not info.get("paired"):
        device.pair(key)
    asset = cs.firmware_asset(info.get("board"))
    app_url = release["assets"].get(asset)
    if not app_url:
        say(f"✗ The release has no {asset} (firmware for this display's board, {info.get('board') or cs.DEFAULT_BOARD})")
        return False
    say(f"Updating the display {current} -> {latest} over Wi-Fi (about a minute, don't unplug it)...")
    try:
        device.push_firmware(key, download(app_url))
    except urllib.error.HTTPError as e:
        if e.code == 403:
            say("✗ The display is paired with a different Mac (or a lost key). Power it on 3 times in a")
            say("  row, each within 10 seconds, to reset its Wi-Fi and pairing, then run this again.")
        else:
            say(f"✗ The display refused the update: {e.read().decode(errors='replace').strip()}")
        return False
    if wait_for_display(device, latest):
        say(f"✓ Display updated to {latest}")
        return True
    say("! The display hasn't come back on the new version yet. Give it a minute, then check")
    say(f"  {cs.INSTALLER_SITE} if it stays blank.")
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="only report versions")
    ap.add_argument("--firmware", action="store_true", help="update the display even if it's up to date")
    args = ap.parse_args()

    # One update at a time: two would trip over each other replacing files and reinstalling,
    # and both would send firmware to the display. Held until this process exits
    if not args.check:
        lock = open(HERE / ".update.lock", "a")
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            say("An update is already running in another Terminal window.")
            return 1

    cfg = cs.load_config(argparse.Namespace(device=None, plan_usage=False))
    try:
        release = cs.latest_release()
    except (OSError, ValueError, KeyError) as e:
        say(f"✗ Couldn't check GitHub for the latest release: {e}")
        return 1
    helper = cs.helper_version()
    try:
        display = cs.Device(cfg["device_host"]).info().get("version")
    except urllib.error.HTTPError:
        display = "older than v1.1.0"
    except (OSError, ValueError):
        display = "not reachable"
    say(f"Latest release: {release['tag']}")
    say(f"Mac helper:     {helper}")
    say(f"Display:        {display}")
    if args.check:
        return 0
    say()

    ok = True
    if helper == "dev":
        say("• Mac helper is a developer install (from a git checkout): update it with")
        say("  `git pull` and `claude-status/bridge/install.sh`, or install the release instead.")
    elif cs.is_newer(release["tag"], helper):
        say(f"Updating the Mac helper {helper} -> {release['tag']}...")
        ok = update_helper(release) and ok
        if ok:
            say(f"✓ Mac helper updated to {release['tag']}")
    else:
        say(f"✓ Mac helper is up to date ({helper})")
    ok = update_display(release, cfg, args.firmware) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
