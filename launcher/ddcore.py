"""Darkest Diablo launcher: everything except the window.

Kept separate from the window so it can be tested and reused (tools/release.py uses the
manifest helpers). Standard library only, so the launcher builds into one small .exe.

Words used here:
- D2R folder:  the player's normal Battle.net install. Only ever read, never changed.
- Game folder: the frozen copy the launcher makes, wherever the player chose to put it.
- Mod folder:  <game folder>/Mods/darkestdiablo, the files the launcher installs and updates.
"""
import hashlib, json, os, re, shutil, string, subprocess, sys, tempfile, time, urllib.error, urllib.parse, urllib.request, zipfile
from pathlib import Path, PurePosixPath

REPO = "Phridgey/DarkestDiablo"           # where releases come from
MOD_NAME = "darkestdiablo"
FALLBACK_D2R_VERSION = "3.4.95032"        # used only until a release manifest says otherwise
MANIFEST_FORMAT = 1
D2RLOADER_PAGE = "https://www.nexusmods.com/diablo2resurrected/mods/1119"
INSTALL_RECORD = "darkestdiablo-install.json"  # in the game folder, next to D2R.exe
COPY_SKIP_DIRS = {"mods"}                 # top-level folders not copied from the D2R folder
COPY_SKIP_FILES = {"d2rhud.dll", "d2rhud_config.txt"}  # leftovers of other mods
USER_AGENT = "DarkestDiablo-Launcher"


# ---------------------------------------------------------------- settings

def settings_path():
    base = os.environ.get("APPDATA") or str(Path.home() / ".config")
    return Path(base) / "DarkestDiablo" / "launcher.json"


def load_settings():
    try:
        return json.loads(settings_path().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def save_settings(s):
    p = settings_path()
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(s, indent=2), encoding="utf-8")


# ---------------------------------------------------------------- finding D2R

def is_d2r_folder(path):
    p = Path(path)
    return (p / "D2R.exe").is_file() and (p / ".build.info").is_file()


def read_build_version(folder):
    """The game build from .build.info (e.g. 3.4.95032), or None.

    .build.info is a pipe-separated table: line 1 names the columns, line 2 is this install.
    """
    try:
        lines = (Path(folder) / ".build.info").read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return None
    if len(lines) < 2:
        return None
    names = [c.split("!")[0].strip().lower() for c in lines[0].split("|")]
    values = [v.strip() for v in lines[1].split("|")]
    if "version" in names and names.index("version") < len(values):
        v = values[names.index("version")]
        if v:
            return v
    return values[-1] or None


def version_matches(found, wanted):
    """True when the install's build is the one the mod targets. The build number (the last part,
    95032 in 3.4.95032) is what identifies a build, so that is what is compared."""
    if not found or not wanted:
        return False
    return found == wanted or found.split(".")[-1] == wanted.split(".")[-1]


def _registry_candidates():
    if sys.platform != "win32":
        return []
    import winreg
    found = []
    roots = [(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall"),
             (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall"),
             (winreg.HKEY_CURRENT_USER, r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall")]
    for hive, key in roots:
        try:
            k = winreg.OpenKey(hive, key)
        except OSError:
            continue
        i = 0
        while True:
            try:
                sub = winreg.EnumKey(k, i)
            except OSError:
                break
            i += 1
            try:
                with winreg.OpenKey(k, sub) as s:
                    name = str(winreg.QueryValueEx(s, "DisplayName")[0])
                    if "diablo ii" in name.lower() and "resurrected" in name.lower():
                        found.append(str(winreg.QueryValueEx(s, "InstallLocation")[0]))
            except OSError:
                pass
    return found


def _common_candidates():
    drives = [f"{d}:\\" for d in string.ascii_uppercase if os.path.exists(f"{d}:\\")] if sys.platform == "win32" else ["/"]
    names = ["Diablo II Resurrected", "Diablo II: Resurrected"]
    parents = ["Program Files (x86)", "Program Files", "Games", "Battle.net", "Blizzard", ""]
    return [str(Path(d) / p / n) for d in drives for p in parents for n in names]


def find_d2r_installs():
    """Every D2R install we can find: the registry first, then the usual folders on every drive."""
    seen, out = set(), []
    for c in _registry_candidates() + _common_candidates():
        try:
            key = str(Path(c).resolve()).lower()
        except OSError:
            continue
        if key not in seen and is_d2r_folder(c):
            seen.add(key)
            out.append(str(Path(c)))
    return out


# ---------------------------------------------------------------- making the game folder

def check_game_target(d2r_dir, game_dir):
    """An error message if game_dir can't be used for the frozen copy, else None."""
    if not game_dir:
        return "Choose where to put the Darkest Diablo game folder."
    src, dst = Path(d2r_dir).resolve(), Path(game_dir).resolve()
    if src == dst or src in dst.parents:
        return "The game folder can't be inside your D2R folder. Your normal install is never changed."
    if dst in src.parents:
        return "The game folder can't contain your D2R folder."
    return None


def plan_copy(d2r_dir):
    """(relative path, size) of every file to copy, leaving out other mods' leftovers."""
    src = Path(d2r_dir)
    files = []
    for root, dirs, names in os.walk(src):
        rel_root = Path(root).relative_to(src)
        if rel_root == Path("."):
            dirs[:] = [d for d in dirs if d.lower() not in COPY_SKIP_DIRS]
        for n in names:
            if rel_root == Path(".") and n.lower() in COPY_SKIP_FILES:
                continue
            rel = rel_root / n
            try:
                files.append((rel, (src / rel).stat().st_size))
            except OSError:
                pass
    return files


def copy_game(d2r_dir, game_dir, progress=lambda done, total, name: None):
    """Copy the D2R folder into game_dir. Files already copied (same size and time) are skipped,
    so a stopped copy picks up where it left off."""
    err = check_game_target(d2r_dir, game_dir)
    if err:
        raise RuntimeError(err)
    src, dst = Path(d2r_dir), Path(game_dir)
    files = plan_copy(src)
    total = sum(s for _, s in files)
    dst.mkdir(parents=True, exist_ok=True)
    free = shutil.disk_usage(dst).free
    have = sum((dst / r).stat().st_size for r, _ in files if (dst / r).is_file())
    if total - have > free:
        raise RuntimeError(f"Not enough space: the game needs {gb(total - have)} more, {gb(free)} free.")
    done = 0
    for rel, size in files:
        s, d = src / rel, dst / rel
        progress(done, total, str(rel))
        st = s.stat()
        if not (d.is_file() and d.stat().st_size == size and abs(d.stat().st_mtime - st.st_mtime) < 2):
            d.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(s, d)
        done += size
    progress(total, total, "")


def gb(n):
    return f"{n / 1024**3:.1f} GB" if n >= 1024**3 else f"{n / 1024**2:.1f} MB" if n >= 1024**2 else f"{max(1, n // 1024)} KB"


def has_d2rloader(game_dir):
    return (Path(game_dir) / "D2RLoader.exe").is_file()


def install_d2rloader(zip_path, game_dir):
    """Extract the D2RLoader zip downloaded from Nexus Mods next to D2R.exe, whatever folder
    the zip keeps D2RLoader.exe in."""
    with zipfile.ZipFile(zip_path) as z:
        exe = [n for n in z.namelist() if PurePosixPath(n).name.lower() == "d2rloader.exe"]
        if not exe:
            raise RuntimeError("That zip has no D2RLoader.exe in it.")
        base = str(PurePosixPath(exe[0]).parent)
        base = "" if base == "." else base + "/"
        for info in z.infolist():
            if info.is_dir() or not info.filename.startswith(base):
                continue
            rel = safe_rel(info.filename[len(base):])
            out = Path(game_dir) / rel
            out.parent.mkdir(parents=True, exist_ok=True)
            with z.open(info) as f, open(out, "wb") as o:
                shutil.copyfileobj(f, o)


# ---------------------------------------------------------------- manifests (shared with tools/release.py)

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def safe_rel(name):
    """A zip or manifest path as a safe relative path (no absolute paths, no '..')."""
    p = PurePosixPath(name.replace("\\", "/"))
    if p.is_absolute() or ".." in p.parts or not p.parts or ":" in p.parts[0]:
        raise RuntimeError(f"Refusing unsafe path in update: {name}")
    return Path(*p.parts)


def version_key(v):
    return tuple(int(x) if x.isdigit() else 0 for x in str(v).lstrip("v").split("."))


# What of build/darkestdiablo is the mod, as paths or folders inside it. Everything else there
# (logs, settings the game or plugin write while you play a linked build) stays on your PC.
# When the build starts writing a new file or folder, add it here: releases and the launcher's
# main channel both ship exactly this.
SHIPPED = ("modinfo.json", f"{MOD_NAME}.mpq/", "d2rloader/plugins/",
           # The Campfire: the mod's loader settings (more stash pages) and what the plugin reads.
           "d2rloader/metadata.json", "d2rloader/config/", "d2rloader/camp_data.txt",
           "d2rloader/SaveSlots.layout.json",
           # The plugin's own drops (guaranteed set/unique items).
           "d2rloader/loot_data.txt")


def collect_build(build_dir):
    """{relative path: {size, sha256}} of the shipped files of a built mod folder
    (build/darkestdiablo). Used by tools/release.py and the main channel alike."""
    build_dir = Path(build_dir)
    files = {}
    for f in sorted(p for p in build_dir.rglob("*") if p.is_file()):
        rel = f.relative_to(build_dir).as_posix()
        if any(rel == s or (s.endswith("/") and rel.startswith(s)) for s in SHIPPED):
            files[rel] = {"size": f.stat().st_size, "sha256": sha256_file(f)}
    return files


def diff_manifests(old_files, new_files):
    """(changed or added paths, removed paths) between two manifests' "files" maps."""
    changed = sorted(p for p, f in new_files.items() if old_files.get(p, {}).get("sha256") != f["sha256"])
    removed = sorted(p for p in old_files if p not in new_files)
    return changed, removed


# ---------------------------------------------------------------- GitHub releases

class _DropAuthOnRedirect(urllib.request.HTTPRedirectHandler):
    # Release files redirect to a storage host that rejects GitHub's Authorization header.
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        new = super().redirect_request(req, fp, code, msg, headers, newurl)
        if new is not None and urllib.parse.urlparse(newurl).netloc != urllib.parse.urlparse(req.full_url).netloc:
            new.remove_header("Authorization")
        return new


_opener = urllib.request.build_opener(_DropAuthOnRedirect)


class NeedToken(Exception):
    """The releases can't be seen without a GitHub token (the repo is private)."""


def _request(url, token=None, accept="application/vnd.github+json"):
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, "Accept": accept})
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    return req


def fetch_json(url, token=None):
    try:
        with _opener.open(_request(url, token), timeout=30) as r:
            return json.loads(r.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        if e.code in (401, 403, 404):
            raise NeedToken(f"GitHub said {e.code} for {url}") from e
        raise


def download(url, dest, token=None, progress=lambda done, total: None):
    with _opener.open(_request(url, token, "application/octet-stream"), timeout=60) as r, open(dest, "wb") as f:
        total = int(r.headers.get("Content-Length") or 0)
        done = 0
        while True:
            chunk = r.read(1 << 16)
            if not chunk:
                break
            f.write(chunk)
            done += len(chunk)
            progress(done, total)


class Release:
    """The newest published release: its manifest and the files attached to it."""

    def __init__(self, data, token=None):
        self.tag = data["tag_name"]
        self.notes = data.get("body") or ""
        self.assets = {a["name"]: a for a in data.get("assets", [])}
        self.token = token
        if "manifest.json" not in self.assets:
            raise RuntimeError(f"Release {self.tag} has no manifest.json. See docs/updates.md.")
        self.manifest = json.loads(_get_bytes(self.asset_url("manifest.json"), token))
        if self.manifest.get("format", 0) > MANIFEST_FORMAT:
            raise RuntimeError("This release needs a newer launcher. Download it from the release page.")
        self.version = self.manifest["version"]

    def asset_url(self, name):
        a = self.assets[name]
        return a["url"] if self.token else a["browser_download_url"]

    def best_download(self, installed_version):
        """(asset name, size): the small delta from the installed version if there is one, else the full package."""
        delta = f"{MOD_NAME}-delta-from-{installed_version}.zip"
        name = delta if installed_version and delta in self.assets else f"{MOD_NAME}-full.zip"
        if name not in self.assets:
            raise RuntimeError(f"Release {self.tag} has no {name}.")
        return name, self.assets[name]["size"]


def _get_bytes(url, token=None):
    with _opener.open(_request(url, token, "application/octet-stream"), timeout=30) as r:
        return r.read()


def latest_release(token=None):
    try:
        data = fetch_json(f"https://api.github.com/repos/{REPO}/releases/latest", token)
    except NeedToken:
        # 404 also means "no release yet". If the repo itself can be seen, that's the reason.
        fetch_json(f"https://api.github.com/repos/{REPO}", token)
        raise RuntimeError("No Darkest Diablo release has been published yet.")
    return Release(data, token)


# ---------------------------------------------------------------- the installed mod

def mod_dir(game_dir):
    return Path(game_dir) / "Mods" / MOD_NAME


def is_dev_link(game_dir):
    """True when Mods/darkestdiablo is tools/link.cmd's link to a developer's build."""
    p = mod_dir(game_dir)
    if not p.exists():
        return False
    try:
        if hasattr(os.path, "isjunction") and os.path.isjunction(p):
            return True
        if p.is_symlink():
            return True
        # Compare with the resolved parent, so short names (RUNNER~1) or a linked parent folder
        # higher up don't count; only Mods/darkestdiablo itself being a link does.
        inside = os.path.join(os.path.realpath(p.parent), p.name)
        return os.path.normcase(os.path.realpath(p)) != os.path.normcase(inside)
    except OSError:
        return False


def read_install(game_dir):
    try:
        return json.loads((Path(game_dir) / INSTALL_RECORD).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def write_install(game_dir, manifest):
    """Record what is installed: the manifest's facts (version, channel, commit...) and its files."""
    rec = {k: v for k, v in manifest.items() if k != "format"}
    rec.setdefault("channel", "releases")
    rec["installed_at"] = time.strftime("%Y-%m-%d %H:%M:%S")
    (Path(game_dir) / INSTALL_RECORD).write_text(json.dumps(rec, indent=1), encoding="utf-8")


def place_files(source, names, target, removed):
    """Copy the named files from source into the mod folder and delete the ones a previous
    install put there that are gone now. Nothing else in the mod folder is touched, so files
    the game or the plugin write there (logs, settings) survive every update."""
    for rel in sorted(names):
        dst = target / safe_rel(rel)
        dst.parent.mkdir(parents=True, exist_ok=True)
        tmp = dst.with_name(dst.name + ".dd-new")
        shutil.copyfile(source / rel, tmp)
        os.replace(tmp, dst)
    for rel in removed:
        try:
            (target / safe_rel(rel)).unlink()
        except FileNotFoundError:
            pass


def apply_package(zip_path, game_dir, manifest, installed):
    """Put a downloaded full package or delta into the mod folder.

    Every file is checked against the release manifest before anything is replaced, so a bad
    download never leaves a half-updated mod. Raises on any mismatch.
    """
    target = mod_dir(game_dir)
    if is_dev_link(game_dir):
        raise RuntimeError("Mods/darkestdiablo is a developer link (tools/link.cmd). The launcher leaves it alone.")
    new_files = manifest["files"]
    old_files = (installed or {}).get("files", {})
    staging = Path(game_dir) / "Mods" / f".{MOD_NAME}-update"
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)
    try:
        with zipfile.ZipFile(zip_path) as z:
            meta = json.loads(z.read("delta.json")) if "delta.json" in z.namelist() else None
            for info in z.infolist():
                if info.is_dir() or info.filename == "delta.json":
                    continue
                rel = safe_rel(info.filename).as_posix()
                if rel not in new_files:
                    raise RuntimeError(f"{rel} is not in the release manifest.")
                out = staging / rel
                out.parent.mkdir(parents=True, exist_ok=True)
                with z.open(info) as f, open(out, "wb") as o:
                    shutil.copyfileobj(f, o)
        staged = {p.relative_to(staging).as_posix() for p in staging.rglob("*") if p.is_file()}
        for rel in staged:
            if sha256_file(staging / rel) != new_files[rel]["sha256"]:
                raise RuntimeError(f"{rel} did not download correctly.")

        if meta is None:
            # Full package: it must hold every file.
            missing = set(new_files) - staged
            if missing:
                raise RuntimeError(f"The package is missing {len(missing)} files.")
            removed = sorted(set(old_files) - set(new_files))
        else:
            # Delta: it holds the changed files; everything else must already be right.
            if meta.get("from") != (installed or {}).get("version"):
                raise RuntimeError("This update was made for a different installed version.")
            changed, removed = diff_manifests(old_files, new_files)
            if set(changed) != staged:
                raise RuntimeError("The update does not match the release manifest.")
            for rel, f in new_files.items():
                if rel in staged:
                    continue
                cur = target / rel
                if not cur.is_file() or cur.stat().st_size != f["size"] or sha256_file(cur) != f["sha256"]:
                    raise RuntimeError(f"Your copy of {rel} is not the expected one; a full download is needed.")
        place_files(staging, staged, target, removed)
        shutil.rmtree(staging, ignore_errors=True)
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    write_install(game_dir, manifest)


def update(game_dir, release, progress=lambda done, total, label: None):
    """Download and apply the newest release. Tries the small delta first, falls back to the full package."""
    installed = read_install(game_dir)
    if installed and not mod_dir(game_dir).exists():
        installed = None
    name, size = release.best_download(installed and installed.get("version"))
    attempts = [name] if name.endswith("-full.zip") else [name, f"{MOD_NAME}-full.zip"]
    last_error = None
    for asset in attempts:
        with tempfile.TemporaryDirectory() as tmp:
            zp = Path(tmp) / asset
            label = "Downloading update" if "delta" in asset else "Downloading Darkest Diablo"
            download(release.asset_url(asset), zp, release.token,
                     lambda d, t: progress(d, t or release.assets[asset]["size"], label))
            progress(1, 1, "Checking files")
            try:
                apply_package(zp, game_dir, release.manifest, installed)
                return
            except RuntimeError as e:
                last_error = e
                installed = None  # fall back to the full package
    raise last_error


# ---------------------------------------------------------------- playing

def battlenet_running():
    if sys.platform != "win32":
        return True
    try:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq Battle.net.exe", "/NH"], capture_output=True,
                             text=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)).stdout
        return "battle.net.exe" in out.lower()
    except OSError:
        return True


def enable_console(game_dir):
    """Turns on D2RLoader's developer console (Ctrl + ` in game) in the game folder's
    d2rloader/config/d2rloader.toml. The loader writes that file on its first start, so before
    then there is nothing to change; the next Update or Play turns it on. Returns True if the
    console is on."""
    path = Path(game_dir) / "d2rloader" / "config" / "d2rloader.toml"
    if not path.is_file():
        return False
    text = path.read_text(encoding="utf-8")
    if re.search(r"(?m)^\s*enable_console\s*=\s*true\b", text):
        return True
    new, n = re.subn(r"(?m)^(\s*enable_console\s*=\s*)false\b", r"\1true", text)
    if n == 0:
        if re.search(r"(?m)^\[d2rloader\.developer\]", text):
            new = re.sub(r"(?m)^(\[d2rloader\.developer\][^\n]*\n)", r"\1enable_console = true\n", text, count=1)
        else:
            new = text.rstrip("\n") + "\n\n[d2rloader.developer]\nenable_console = true\n"
    path.write_text(new, encoding="utf-8")
    return True


def play(game_dir):
    exe = Path(game_dir) / "D2RLoader.exe"
    if not exe.is_file():
        raise RuntimeError("D2RLoader.exe is missing from the game folder.")
    enable_console(game_dir)
    subprocess.Popen([str(exe)], cwd=str(game_dir))
