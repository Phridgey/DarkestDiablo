# The launcher and how updates reach players

Players don't clone this repo. They run **DarkestDiabloLauncher.exe**, which sets up the game
once and then offers **Update** or **Play** every time it opens. This page is for us: what the
launcher does, what an update is made of, and how to publish one.

## What the launcher does

First run:
1. Finds Diablo II: Resurrected (Windows' list of installed programs, then the usual folders on
   every drive). The player can point to it by hand. It checks the build in `.build.info` against
   the release's `d2r_version` (our `D2R_VERSION`) and refuses any other build.
2. Asks where the Darkest Diablo game folder goes. Any folder, any drive; nothing is assumed.
3. Copies the game there, leaving out `Mods`, `D2RHUD.dll` and `D2RHUD_Config.txt` (what the old
   `robocopy` step did). The normal install is only read, never changed. A stopped copy picks up
   where it left off.
4. D2RLoader comes from Nexus Mods, which needs an account, so the launcher can't download it.
   It opens the page and asks for the downloaded zip, then unpacks it next to `D2R.exe`.
5. Downloads the newest release into `<game folder>\Mods\darkestdiablo` and writes
   `<game folder>\darkestdiablo-install.json` (installed version and the file list).

Every run after that: looks at the newest GitHub release. If it is newer than the installed one it
shows **Update to x.y.z** with the download size; otherwise **Play**. Play checks that the
Battle.net app is open and starts `D2RLoader.exe` in the game folder. The first time it reminds the
player to set D2RLoader's Default mod to `darkestdiablo`.

Settings (folders, an optional GitHub token) are kept in `%APPDATA%\DarkestDiablo\launcher.json`.

**Our own setups are left alone.** If `Mods\darkestdiablo` is the link made by `tools\link.cmd`,
the launcher says "Developer build" and only offers Play. Keep developing exactly as before.

## What a release is made of

A release is a GitHub release tagged `v<version>` (like `v0.2.0`) with these files attached:

| File | What it is |
| --- | --- |
| `manifest.json` | Every file of the mod with its size and SHA-256. The source of truth. |
| `darkestdiablo-full.zip` | The whole mod. Used for new installs and as the fallback. |
| `darkestdiablo-delta-from-<old>.zip` | Only the files that changed since `<old>`, plus `delta.json`. One per recent release (the last 5). |
| `changelog.md` | What changed: the notes, then the commit messages since the last release. Also the release's description. |
| `DarkestDiabloLauncher.exe` | Added automatically by the Launcher workflow when the release is published. |

`manifest.json`:

```json
{
  "format": 1,
  "version": "0.2.0",
  "d2r_version": "3.4.95032",
  "files": {
    "modinfo.json": {"size": 47, "sha256": "..."},
    "darkestdiablo.mpq/data/global/excel/levels.txt": {"size": 61234, "sha256": "..."},
    "d2rloader/plugins/darkestdiablo.dll": {"size": 412160, "sha256": "..."}
  }
}
```

The manifest also has a `build` section the launcher ignores: the commit and branch it was built
from, whether there were uncommitted changes, what it was made with (Python, Plugin SDK, compiler,
D2RLoader), the checksum of every tool in `tools/` and `launcher/`, and of the tools we built (the
plugin DLL, the launcher .exe). If two of us see different games on the same version, compare
these first.

Paths are relative to `Mods\darkestdiablo`, with forward slashes. Only `modinfo.json`,
`darkestdiablo.mpq/`, `d2rloader/plugins/` and the mod's own settings and data in `d2rloader/`
(`metadata.json`, `config/`, `camp_data.txt`, `SaveSlots.layout.json`) are shipped; logs and settings the game writes into
your build stay on your PC.

`delta.json` inside a delta zip:

```json
{"format": 1, "from": "0.1.0", "to": "0.2.0", "changed": ["..."], "removed": ["..."]}
```

### Why updates stay small

A delta holds only whole files that changed, compressed. Changing a few tables or the plugin means
a download of a few KB to a few hundred KB, not the whole mod. The launcher picks
`darkestdiablo-delta-from-<installed version>.zip`; if there isn't one (the player skipped more
than 5 releases) it downloads the full package instead.

### Why updates are safe

- Every downloaded file is checked against `manifest.json` before anything is replaced. A broken
  download changes nothing.
- A delta is only applied if every other file on the player's PC already matches the manifest.
  If not (a file edited by hand, a half-finished earlier update) it falls back to the full package.
- Zip entries that try to write outside the mod folder are refused.

## Publishing an update

1. Merge to `main` and build on your PC: `tools\build-plugin.cmd` (plugin and data).
   Play it once through the link to check it works.
2. Make the release files and look at what changed:

       python tools/release.py 0.2.0

   It prints the size of the full package and of each delta. Files land in `build\release\0.2.0\`.
   It also writes `changelog.md`: your `--notes`, then every commit message since the last release.
3. Push your commit, then publish. No extra tools: it signs in to GitHub the same way `git push`
   does (or uses `GITHUB_TOKEN` if set).

       python tools/release.py 0.2.0 --publish --notes "Blood Raven's fork; lighter Den waves"

   The changelog becomes the release notes, and is attached as `changelog.md`. The first line (your
   `--notes`) is shown to players next to the Update button. The release is uploaded as a draft and
   only made public once every file is up, so players never see half a release. The workflow then
   attaches the launcher .exe to the release.

Rules:
- Versions only go up (`0.1.0`, `0.1.1`, `0.2.0`, ...). The script refuses a lower one and refuses
  to publish when nothing changed.
- Never edit or delete the files of a published release. Players' launchers use them to build
  deltas. To fix a bad release, publish a new version.
- Releases ship the built mod, which includes tables derived from Blizzard's (changed by our
  patches). Those go only into release files, never into the repo (see `CLAUDE.md`).
- If `D2R_VERSION` ever changes (both of us agreed), the next release carries the new build in
  `d2r_version` and launchers on the old build will say their game copy doesn't match.

## The launcher's code

| Path | What |
| --- | --- |
| `launcher/ddcore.py` | Finding D2R, copying the game, D2RLoader, releases, applying updates. No window code. |
| `launcher/darkestdiablo_launcher.py` | The window. Everything is drawn in code: no images, no sound (silent on purpose). |
| `tools/release.py` | Makes and publishes releases. Shares the manifest code with the launcher. |
| `tools/build-launcher.cmd` | Builds `build\launcher\DarkestDiabloLauncher.exe` with PyInstaller. |
| `tests/test_launcher.py` | Install, delta, fallback and safety tests: `python -m unittest discover tests`. |
| `.github/workflows/launcher.yml` | Runs the tests and builds the .exe on Windows. |

Run it from source with `python launcher\darkestdiablo_launcher.py` (Python 3.10+, standard
library only). If the manifest ever needs a field old launchers can't understand, raise
`MANIFEST_FORMAT` in `ddcore.py`; older launchers then tell players to download the new one.

### While the repo is private

GitHub only shows a private repo's releases to people signed in. Until the repo (or a separate
public repo just for releases, by changing `REPO` in `ddcore.py`) is public, the launcher asks for
a GitHub token with read access to the repo's contents and keeps it in its settings file.
