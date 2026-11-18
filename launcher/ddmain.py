"""The main channel: build Darkest Diablo from the newest commit on main, on this PC.

Until there are releases, this is how the launcher installs and updates the mod:
1. Downloads the source of the newest commit on main (a zip from GitHub, no Git needed).
2. Reads the game files the build needs from the player's own game folder (only the first time,
   or when the list in tools/vanilla_files.txt or the game build changes). The reader is
   tools/extract.cpp, built with CascLib and bundled inside the launcher.
3. Runs the repo's own tools\\build-plugin.cmd, exactly as a contributor would. It needs Visual
   Studio's C++ tools for the plugin. Python comes from the launcher itself (see python_shim).
4. Copies the result into <game folder>\\Mods\\darkestdiablo, only the files that changed.
   Files the game or plugin write there, and the saves (which live in Saved Games), stay.

Everything is kept in <game folder>\\darkestdiablo-src: the source, vanilla/ and build/, so the
next build only redoes what changed.
"""
import hashlib, os, shutil, subprocess, sys, zipfile
from pathlib import Path

import ddcore

BRANCH = "main"
WORKSPACE = "darkestdiablo-src"
KEEP = {"vanilla", "build", ".python"}   # survive a source refresh
VS_PAGE = "https://visualstudio.microsoft.com/vs/community/"
# Used only if the downloaded source has no tools/vanilla_files.txt (main before that list
# existed). Keep it a copy of that file.
DEFAULT_VANILLA = ["data/global/excel/", "data/local/lng/strings/", "data/global/dataversionbuild.txt",
                   "data/global/tiles/act1/", "data/hd/env/preset/act1/",
                   "data/global/ui/layouts/", "data/hd/items/items.json", "data/hd/objects/objects.json",
                   "data/hd/env/model/global/prop/act1/town/prefab/pf_town_campfire01.json"]
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)


class NeedVisualStudio(Exception):
    """The plugin can't be built: Visual Studio's C++ tools are missing."""


class BuildFailed(Exception):
    def __init__(self, message, log):
        super().__init__(message)
        self.log = log


def workspace(game_dir):
    return Path(game_dir) / WORKSPACE


# ---------------------------------------------------------------- GitHub

def latest_commit():
    d = ddcore.fetch_json(f"https://api.github.com/repos/{ddcore.REPO}/commits/{BRANCH}")
    return {"sha": d["sha"], "message": d["commit"]["message"].splitlines()[0],
            "date": d["commit"]["committer"]["date"]}


def changes_since(old_sha, new_sha):
    """How many commits main is ahead of old_sha, or None if GitHub can't say."""
    try:
        d = ddcore.fetch_json(f"https://api.github.com/repos/{ddcore.REPO}/compare/{old_sha}...{new_sha}")
        return int(d.get("ahead_by", 0))
    except Exception:
        return None


def fetch_source(sha, ws, progress):
    """Put the source of commit sha into ws, keeping vanilla/ and build/ from earlier builds."""
    ws.mkdir(parents=True, exist_ok=True)
    zp = ws / ".source.zip"
    ddcore.download(f"https://codeload.github.com/{ddcore.REPO}/zip/{sha}", zp,
                    progress=lambda d, t: progress(d, t, "Downloading the latest main"))
    for entry in ws.iterdir():
        if entry.name in KEEP or entry == zp:
            continue
        shutil.rmtree(entry) if entry.is_dir() and not entry.is_symlink() else entry.unlink()
    with zipfile.ZipFile(zp) as z:
        for info in z.infolist():
            parts = info.filename.split("/", 1)   # GitHub puts everything in <repo>-<sha>/
            if len(parts) < 2 or not parts[1] or info.is_dir():
                continue
            out = ws / ddcore.safe_rel(parts[1])
            out.parent.mkdir(parents=True, exist_ok=True)
            with z.open(info) as f, open(out, "wb") as o:
                shutil.copyfileobj(f, o)
    zp.unlink()
    (ws / ".commit").write_text(sha, encoding="utf-8")


# ---------------------------------------------------------------- game files

def vanilla_list(ws):
    f = ws / "tools" / "vanilla_files.txt"
    if not f.is_file():
        return DEFAULT_VANILLA
    lines = [l.split("#", 1)[0].strip() for l in f.read_text(encoding="utf-8").splitlines()]
    return [l for l in lines if l]


def find_extractor():
    here = Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent))
    exe_dir = Path(sys.executable).resolve().parent
    for c in (here / "extract.exe", exe_dir / "extract.exe",
              Path(__file__).resolve().parent.parent / "build" / "extract" / "extract.exe"):
        if c.is_file():
            return c
    return None


def ensure_vanilla(game_dir, ws, progress):
    """Extract the game files the build reads into ws/vanilla, unless they are already there."""
    patterns = vanilla_list(ws)
    build = ddcore.read_build_version(game_dir) or "?"
    key = hashlib.sha256(("\n".join(patterns) + "|" + build).encode()).hexdigest()
    vanilla = ws / "vanilla"
    marker = vanilla / ".extracted"
    if marker.is_file() and marker.read_text().strip() == key:
        return
    exe = find_extractor()
    if not exe:
        raise RuntimeError("The game file reader (extract.exe) is missing from the launcher. "
                           "Download the launcher again.")
    progress(0, 0, "Reading the game files the build needs (first time only, a few minutes)")
    vanilla.mkdir(parents=True, exist_ok=True)
    r = subprocess.run([str(exe), str(game_dir), str(vanilla), *patterns], capture_output=True, text=True,
                       creationflags=NO_WINDOW)
    if r.returncode != 0:
        raise RuntimeError(f"Reading the game files failed: {(r.stderr or r.stdout).strip()[-400:]}")
    (vanilla / "D2R_VERSION").write_text(build, encoding="utf-8")
    marker.write_text(key, encoding="utf-8")


# ---------------------------------------------------------------- building

def find_visual_studio():
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / \
        "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vswhere.is_file():
        return None
    r = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires",
                        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
                       capture_output=True, text=True, creationflags=NO_WINDOW)
    path = r.stdout.strip().splitlines()
    return path[0] if path else None


def python_shim(ws):
    """A folder with a python.exe for the build to use.

    When the launcher runs as an .exe, it has Python inside it: a copy of the launcher named
    python.exe runs scripts instead of opening the window (see the launcher's main()). So
    players don't need Python. Run from source, the Python running the launcher is used.
    """
    if not getattr(sys, "frozen", False):
        return Path(sys.executable).parent
    shim = ws / ".python"
    shim.mkdir(exist_ok=True)
    target = shim / "python.exe"
    src = Path(sys.executable)
    if not target.is_file() or target.stat().st_size != src.stat().st_size:
        shutil.copyfile(src, target)
    return shim


def run_build(ws, progress):
    """Run the repo's tools\\build-plugin.cmd. Its output goes to ws/build.log."""
    env = dict(os.environ)
    env["PATH"] = str(python_shim(ws)) + os.pathsep + env.get("PATH", "")
    env["PYTHONUTF8"] = "1"
    env["PYINSTALLER_RESET_ENVIRONMENT"] = "1"  # the python.exe copy must unpack itself, not reuse ours
    log_path = ws / "build.log"
    with open(log_path, "w", encoding="utf-8", errors="replace") as log:
        p = subprocess.Popen(["cmd", "/c", str(ws / "tools" / "build-plugin.cmd")], cwd=str(ws), env=env,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                             creationflags=NO_WINDOW)
        tail = []
        for line in p.stdout:
            log.write(line)
            line = line.strip()
            if line:
                tail = (tail + [line])[-15:]
                progress(0, 0, "Building: " + line[:80])
        p.wait()
    if p.returncode != 0:
        raise BuildFailed("The build failed. The end of the build log:\n" + "\n".join(tail[-6:]), log_path)


# ---------------------------------------------------------------- installing

def install_build(game_dir, ws, commit):
    """Copy what changed from ws/build/darkestdiablo into the mod folder and record it."""
    if ddcore.is_dev_link(game_dir):
        raise RuntimeError("Mods/darkestdiablo is a developer link (tools/link.cmd). The launcher leaves it alone.")
    built = ws / "build" / ddcore.MOD_NAME
    files = ddcore.collect_build(built)
    if f"d2rloader/plugins/{ddcore.MOD_NAME}.dll" not in files:
        raise RuntimeError("The build finished without the plugin. See darkestdiablo-src\\build.log.")
    target = ddcore.mod_dir(game_dir)
    installed = ddcore.read_install(game_dir) or {}
    old = installed.get("files", {}) if target.exists() else {}
    changed = [rel for rel, f in files.items()
               if old.get(rel, {}).get("sha256") != f["sha256"] or not (target / rel).is_file()]
    removed = sorted(set(old) - set(files))
    ddcore.place_files(built, changed, target, removed)
    ddcore.write_install(game_dir, {
        "channel": "main", "version": f"main {commit['sha'][:7]}", "commit": commit["sha"],
        "commit_message": commit.get("message", ""), "commit_date": commit.get("date", ""),
        "d2r_version": (ws / "D2R_VERSION").read_text().strip() if (ws / "D2R_VERSION").is_file() else None,
        "files": files})
    return len(changed), len(removed)


def build_and_install(game_dir, progress, commit=None):
    """The whole main-channel install or update. Returns (commit, files changed, files removed)."""
    if sys.platform == "win32" and not find_visual_studio():
        raise NeedVisualStudio()
    ws = workspace(game_dir)
    commit = commit or latest_commit()
    fetch_source(commit["sha"], ws, progress)
    ensure_vanilla(game_dir, ws, progress)
    progress(0, 0, "Building")
    run_build(ws, progress)
    progress(1, 1, "Installing")
    changed, removed = install_build(game_dir, ws, commit)
    return commit, changed, removed
