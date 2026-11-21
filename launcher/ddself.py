"""The launcher updating itself.

Every push to main that changes the launcher makes the Launcher workflow build the .exe and put
it on one GitHub pre-release, tag `launcher`, next to `launcher.json`:
    {"build": 57, "commit": "<sha>", "sha256": "<of the .exe>", "size": 12345678}
That pre-release only ever holds the newest launcher; it is not a mod release (the releases
channel reads /releases/latest, which skips pre-releases). See docs/updates.md.

On start the launcher reads launcher.json. If its build number is higher than ours
(build_info.BUILD), it downloads the .exe next to itself as <name>.new, checks size and sha256,
then starts a small script that waits for this launcher to close, moves the new .exe over the
old one and opens it. A launcher not built by the workflow (BUILD 0) never replaces itself.
"""
import json, os, subprocess, sys, tempfile
from pathlib import Path

import build_info
import ddcore

TAG = "launcher"
INFO = "launcher.json"
EXE = "DarkestDiabloLauncher.exe"


def can_update():
    return getattr(sys, "frozen", False) and sys.platform == "win32" and build_info.BUILD > 0


def newer_build(info, current=None):
    """True if launcher.json (info) describes a newer build than ours."""
    current = build_info.BUILD if current is None else current
    try:
        return current > 0 and int(info["build"]) > current and bool(info.get("sha256"))
    except (KeyError, TypeError, ValueError):
        return False


def check():
    """launcher.json of the newest launcher if it is newer than this one, else None.
    Never raises: a failed check just means no update this time."""
    if not can_update():
        return None
    try:
        rel = ddcore.fetch_json(f"https://api.github.com/repos/{ddcore.REPO}/releases/tags/{TAG}")
        assets = {a["name"]: a for a in rel.get("assets", [])}
        if INFO not in assets or EXE not in assets:
            return None
        info = json.loads(ddcore._get_bytes(assets[INFO]["browser_download_url"]))
        if not newer_build(info):
            return None
        info["url"] = assets[EXE]["browser_download_url"]
        return info
    except Exception:
        return None


def download(info, progress=lambda done, total: None):
    """Download the new .exe next to ours as <name>.new and check it. Returns its path."""
    exe = Path(sys.executable).resolve()
    new = exe.with_name(exe.name + ".new")
    try:
        ddcore.download(info["url"], new, progress=progress)
        if "size" in info and new.stat().st_size != int(info["size"]):
            raise RuntimeError("The new launcher didn't download completely.")
        if ddcore.sha256_file(new) != info["sha256"].lower():
            raise RuntimeError("The new launcher download is damaged (checksum differs).")
    except BaseException:
        try:
            new.unlink()
        except OSError:
            pass
        raise
    return new


def swap_script(new, exe, pids):
    """A cmd script: wait for pids to end, move new over exe, open exe, delete itself.
    If the move keeps failing, the old launcher opens again instead."""
    waits = "\n".join(
        f'tasklist /FI "PID eq {p}" /NH 2>nul | find " {p} " >nul && (set /a n+=1 & '
        f'if !n! lss 60 (timeout /t 1 /nobreak >nul & goto wait))' for p in pids)
    return f"""@echo off
setlocal enabledelayedexpansion
set n=0
:wait
{waits}
set m=0
:move
move /y "{new}" "{exe}" >nul 2>nul
if errorlevel 1 (
  set /a m+=1
  if !m! lss 20 (timeout /t 1 /nobreak >nul & goto move)
  del "{new}" >nul 2>nul
)
start "" "{exe}"
del "%~f0"
"""


def install_and_restart(new):
    """Start the swap script. The caller must then close the launcher."""
    exe = Path(sys.executable).resolve()
    # The one-file .exe runs as two processes (an unpacker and us); both hold the file.
    pids = sorted({os.getpid(), os.getppid()})
    fd, script = tempfile.mkstemp(prefix="dd-launcher-update-", suffix=".cmd")
    with os.fdopen(fd, "w", encoding="mbcs" if sys.platform == "win32" else "utf-8") as f:
        f.write(swap_script(new, exe, pids))
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    # The script must outlive us; leave our job object if Windows lets us (it fails if not allowed).
    try:
        ddcore.popen(["cmd", "/c", script], cwd=str(exe.parent),
                     creationflags=flags | getattr(subprocess, "CREATE_BREAKAWAY_FROM_JOB", 0))
    except OSError:
        ddcore.popen(["cmd", "/c", script], cwd=str(exe.parent), creationflags=flags)
