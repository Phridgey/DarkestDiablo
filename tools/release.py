"""Publish what is in build/darkestdiablo/ as a launcher update. See docs/updates.md.

Usage (from the repo folder, after building):
    python tools/release.py 0.2.0                 # make the files in build/release/0.2.0/, publish nothing
    python tools/release.py 0.2.0 --publish       # also create GitHub release v0.2.0 with them (needs gh)
    python tools/release.py 0.2.0 --publish --notes "Blood Raven's fork"

What it makes:
    manifest.json                          every file of the mod with its size and SHA-256
    darkestdiablo-full.zip                 the whole mod, for new installs
    darkestdiablo-delta-from-<old>.zip     only the files that changed since <old>, one per
                                           recent release, so players download a few KB
"""
import argparse, json, subprocess, sys, tempfile, zipfile
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


def gh(*args, capture=True):
    return subprocess.run(["gh", *args], cwd=ROOT, check=True, text=True, capture_output=capture).stdout


def earlier_manifests():
    """{version: manifest} of the newest published releases, read from GitHub."""
    try:
        tags = json.loads(gh("release", "list", "--limit", str(DELTAS_FOR), "--exclude-drafts",
                             "--exclude-pre-releases", "--json", "tagName"))
    except (OSError, subprocess.CalledProcessError) as e:
        print(f"note: could not list earlier releases ({e}); making the full package only.")
        return {}
    out = {}
    for t in tags:
        with tempfile.TemporaryDirectory() as tmp:
            try:
                gh("release", "download", t["tagName"], "-p", "manifest.json", "-D", tmp)
                m = json.loads((Path(tmp) / "manifest.json").read_text(encoding="utf-8"))
                out[m["version"]] = m
            except (subprocess.CalledProcessError, OSError, ValueError, KeyError):
                print(f"note: release {t['tagName']} has no usable manifest.json; skipping it.")
    return out


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
    ap.add_argument("--publish", action="store_true", help="create the GitHub release (needs the gh tool, signed in)")
    ap.add_argument("--notes", default="", help="what changed, shown to players in the launcher")
    a = ap.parse_args()

    version = a.version.lstrip("v")
    if not (BUILD / f"{ddcore.MOD_NAME}.mpq").is_dir():
        sys.exit("build/darkestdiablo is missing. Run tools\\build-plugin.cmd (or python tools/build.py) first.")
    if not (BUILD / "d2rloader" / "plugins" / f"{ddcore.MOD_NAME}.dll").is_file():
        sys.exit("The plugin isn't in the build. Run tools\\build-plugin.cmd first, so players get the code too.")

    files = collect()
    manifest = {"format": ddcore.MANIFEST_FORMAT, "version": version,
                "d2r_version": (ROOT / "D2R_VERSION").read_text().strip(), "files": files}
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

    if not a.publish:
        print(f"\nMade {out}. Nothing published; add --publish to create release v{version}.")
        return
    notes = a.notes or f"Darkest Diablo {version}"
    gh("release", "create", f"v{version}", *[str(p) for p in sorted(out.iterdir())],
       "--title", f"Darkest Diablo {version}", "--notes", notes, capture=False)
    print(f"Published v{version}. Launchers will offer it as an update.")


if __name__ == "__main__":
    main()
