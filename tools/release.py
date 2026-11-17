"""Publish what is in build/darkestdiablo/ as a launcher update. See docs/updates.md.

Usage (from the repo folder, after building):
    python tools/release.py 0.2.0                 # make the files in build/release/0.2.0/, publish nothing
    python tools/release.py 0.2.0 --publish       # also create GitHub release v0.2.0 with them
                                                  # (signs in like `git push`; no extra tools)
    python tools/release.py 0.2.0 --publish --notes "Blood Raven's fork"

What it makes:
    manifest.json                          every file of the mod with its size and SHA-256
    darkestdiablo-full.zip                 the whole mod, for new installs
    darkestdiablo-delta-from-<old>.zip     only the files that changed since <old>, one per
                                           recent release, so players download a few KB
    changelog.md                           the release notes: --notes, then the commits since the last release
"""
import argparse, json, os, subprocess, sys, urllib.error, urllib.parse, zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "launcher"))
import ddcore  # noqa: E402

BUILD = ROOT / "build" / ddcore.MOD_NAME
OUT = ROOT / "build" / "release"
# Only these parts of the build are shipped. Logs and settings the game writes there stay local.
SHIP = ["modinfo.json", f"{ddcore.MOD_NAME}.mpq", "d2rloader/plugins"]
DELTAS_FOR = 5  # make deltas from this many earlier releases; older installs get the full package


def collect():
    files = {}
    for part in SHIP:
        p = BUILD / part
        paths = [p] if p.is_file() else sorted(x for x in p.rglob("*") if x.is_file()) if p.is_dir() else []
        for f in paths:
            rel = f.relative_to(BUILD).as_posix()
            files[rel] = {"size": f.stat().st_size, "sha256": ddcore.sha256_file(f)}
    return files


def _git(*args):
    try:
        return subprocess.run(["git", *args], cwd=ROOT, check=True, text=True, capture_output=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def _cmake_value(path, name):
    try:
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            line = line.strip()
            if line.startswith(f"set({name} ") or line.startswith(f"{name}:"):
                return line.split('"')[1] if '"' in line else line.split("=", 1)[1]
    except OSError:
        pass
    return None


def build_info():
    """How this build was made: the commit, the toolchain, and every tool with its checksum.

    Two builds of the same version should match file for file; if they don't, compare these.
    The launcher ignores this section (it only reads format, version, d2r_version and files).
    """
    obj = ROOT / "build" / "plugin-obj"
    compiler = next((v for f in sorted(obj.glob("CMakeFiles/*/CMakeCXXCompiler.cmake"))
                     if (v := _cmake_value(f, "CMAKE_CXX_COMPILER_VERSION"))), None)
    cmake_lists = (ROOT / "src" / "plugin" / "CMakeLists.txt").read_text(encoding="utf-8")
    sdk = next((l.split()[1] for l in cmake_lists.splitlines() if l.strip().startswith("GIT_TAG")), None)
    # Every tool kept in Git (scripts we wrote) and every tool we built from them.
    tools = {}
    for rel in _git("ls-files", "tools", "launcher", "src/plugin/CMakeLists.txt").splitlines():
        tools[rel] = ddcore.sha256_file(ROOT / rel)
    built = {}
    for f in sorted((ROOT / "build" / "launcher").glob("*.exe")) + [ROOT / "build" / "plugin" / f"{ddcore.MOD_NAME}.dll"]:
        if f.is_file():
            built[f.relative_to(ROOT).as_posix()] = {"size": f.stat().st_size, "sha256": ddcore.sha256_file(f)}
    return {
        "commit": _git("rev-parse", "HEAD"),
        "branch": _git("rev-parse", "--abbrev-ref", "HEAD"),
        "uncommitted_changes": bool(_git("status", "--porcelain", "--untracked-files=no")),
        "made_with": {
            "python": sys.version.split()[0],
            "plugin_sdk": sdk,
            "compiler": f"MSVC {compiler}" if compiler else None,
            "cmake_generator": _cmake_value(obj / "CMakeCache.txt", "CMAKE_GENERATOR"),
            "d2rloader": "1.3.1 or later",
        },
        "tools": tools,
        "built_tools": built,
    }


_token = []


def github_token():
    """A GitHub token: GITHUB_TOKEN if set, otherwise the sign-in `git push` already uses."""
    if not _token:
        token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
        if not token:
            try:
                r = subprocess.run(["git", "credential", "fill"], cwd=ROOT, input="protocol=https\nhost=github.com\n\n",
                                   text=True, capture_output=True, check=True, timeout=300,
                                   env={**os.environ, "GIT_TERMINAL_PROMPT": "0"})
                token = next((l[len("password="):] for l in r.stdout.splitlines() if l.startswith("password=")), None)
            except (OSError, subprocess.SubprocessError):
                token = None
        _token.append(token)
    return _token[0]


def api(method, url, body=None, content_type="application/json"):
    """One GitHub API call, signed in. `url` is a full URL or a path under the repo."""
    if not url.startswith("https://"):
        url = f"https://api.github.com/repos/{ddcore.REPO}{url}"
    req = ddcore._request(url, github_token())
    req.method = method
    if body is not None:
        req.data = body if isinstance(body, bytes) else json.dumps(body).encode("utf-8")
        req.add_header("Content-Type", content_type)
    with ddcore._opener.open(req, timeout=300) as r:
        raw = r.read()
    return json.loads(raw) if raw else None


def published_releases():
    """The newest published releases (no drafts), newest first."""
    rels = api("GET", "/releases?per_page=30")
    return [r for r in rels if not r.get("draft") and not r.get("prerelease")]


def earlier_manifests():
    """{version: manifest} of the newest published releases, read from GitHub."""
    try:
        rels = published_releases()[:DELTAS_FOR]
    except (OSError, ValueError) as e:
        print(f"note: could not list earlier releases ({e}); making the full package only.")
        return {}
    out = {}
    for r in rels:
        asset = next((a for a in r.get("assets", []) if a["name"] == "manifest.json"), None)
        try:
            m = json.loads(ddcore._get_bytes(asset["url"], github_token()))
            out[m["version"]] = m
        except (TypeError, OSError, ValueError, KeyError):
            print(f"note: release {r['tag_name']} has no usable manifest.json; skipping it.")
    return out


def changelog(version, since, notes):
    """The release notes: your --notes first (players see the first line), then every commit since `since`."""
    span = [f"v{since}..HEAD"] if since and _git("rev-parse", "-q", "--verify", f"refs/tags/v{since}") else ["-30"]
    commits = _git("log", "--no-merges", "--format=- %s", *span)
    lines = [notes or f"Darkest Diablo {version}", ""]
    if commits:
        lines += [f"Changes since {since}:" if since else "Recent changes:", commits, ""]
    return "\n".join(lines)


def publish(version, out, notes):
    """Create GitHub release v<version> with every file in `out`. Made as a draft first, so players
    never see a half-uploaded release."""
    if not github_token():
        sys.exit("Can't sign in to GitHub. Run `git push` once (it saves your sign-in) or set GITHUB_TOKEN.")
    newest = next(iter(published_releases()), None)
    if newest and ddcore.version_key(version) <= ddcore.version_key(newest["tag_name"].lstrip("v")):
        sys.exit(f"{version} is not higher than the last release ({newest['tag_name']}).")
    commit = _git("rev-parse", "HEAD")
    try:
        rel = api("POST", "/releases", {"tag_name": f"v{version}", "target_commitish": commit,
                                        "name": f"Darkest Diablo {version}", "body": notes, "draft": True})
    except urllib.error.HTTPError as e:
        sys.exit(f"GitHub refused the release ({e.code}: {e.read().decode('utf-8', 'replace')[:300]}). "
                 "Is your commit pushed, and is the version new?")
    upload = rel["upload_url"].split("{")[0]
    for p in sorted(out.iterdir()):
        api("POST", f"{upload}?name={urllib.parse.quote(p.name)}", p.read_bytes(), "application/octet-stream")
        print(f"uploaded: {p.name}")
    api("PATCH", f"/releases/{rel['id']}", {"draft": False})


def write_zip(path, names, extra=None):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for rel in names:
            z.write(BUILD / rel, rel)
        if extra:
            for name, data in extra.items():
                z.writestr(name, data)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("version", help="new version, like 0.2.0 (higher than the last release)")
    ap.add_argument("--publish", action="store_true", help="create the GitHub release (signs in like git push)")
    ap.add_argument("--notes", default="", help="what changed, shown to players in the launcher")
    a = ap.parse_args()

    version = a.version.lstrip("v")
    if not (BUILD / f"{ddcore.MOD_NAME}.mpq").is_dir():
        sys.exit("build/darkestdiablo is missing. Run tools\\build-plugin.cmd (or python tools/build.py) first.")
    if not (BUILD / "d2rloader" / "plugins" / f"{ddcore.MOD_NAME}.dll").is_file():
        sys.exit("The plugin isn't in the build. Run tools\\build-plugin.cmd first, so players get the code too.")

    files = collect()
    manifest = {"format": ddcore.MANIFEST_FORMAT, "version": version,
                "d2r_version": (ROOT / "D2R_VERSION").read_text().strip(), "files": files,
                "build": build_info()}
    if manifest["build"]["uncommitted_changes"]:
        print("note: the repo has uncommitted changes; this build can't be remade from a commit.")
    earlier = earlier_manifests()
    if earlier:
        newest = max(earlier, key=ddcore.version_key)
        if ddcore.version_key(version) <= ddcore.version_key(newest):
            sys.exit(f"{version} is not higher than the last release ({newest}).")
        if earlier[newest]["files"] == files:
            sys.exit(f"Nothing changed since {newest}; no release needed.")

    out = OUT / version
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob("*"):
        old.unlink()
    (out / "manifest.json").write_text(json.dumps(manifest, indent=1), encoding="utf-8")
    write_zip(out / f"{ddcore.MOD_NAME}-full.zip", sorted(files))
    print(f"full package: {ddcore.gb((out / f'{ddcore.MOD_NAME}-full.zip').stat().st_size)}")

    for old_version, old in sorted(earlier.items(), key=lambda kv: ddcore.version_key(kv[0])):
        changed, removed = ddcore.diff_manifests(old["files"], files)
        meta = json.dumps({"format": ddcore.MANIFEST_FORMAT, "from": old_version, "to": version,
                           "changed": changed, "removed": removed}, indent=1)
        z = out / f"{ddcore.MOD_NAME}-delta-from-{old_version}.zip"
        write_zip(z, changed, {"delta.json": meta})
        print(f"delta from {old_version}: {len(changed)} changed, {len(removed)} removed, {ddcore.gb(z.stat().st_size)}")

    notes = changelog(version, max(earlier, key=ddcore.version_key) if earlier else None, a.notes)
    (out / "changelog.md").write_text(notes, encoding="utf-8")
    print(f"changelog: {out / 'changelog.md'}")

    if not a.publish:
        print(f"\nMade {out}. Nothing published; add --publish to create release v{version}.")
        return
    publish(version, out, notes)
    print(f"Published v{version}. Launchers will offer it as an update.")


if __name__ == "__main__":
    main()
