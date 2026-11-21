# The launcher and how updates reach players

Players don't clone this repo. They run **DarkestDiabloLauncher.exe**, which sets up the game
once and then offers **Update** or **Play** every time it opens. This page is for us: what the
launcher does, where it gets the mod from, and what that means for every change we merge.

The launcher has two channels (setting `channel` in `%APPDATA%\DarkestDiablo\launcher.json`):

- **`main` (the default for now):** builds the newest commit on `main` on the player's PC.
- **`releases` (later):** downloads published releases as small deltas. See further down.

## The main channel: what it means for us

**Whatever is on `main` is what everyone's launcher builds next.** Update shows up as soon as
`main` has a new commit. So:

- Only merge to `main` what builds with `tools\build-plugin.cmd` from scratch.
- Every game file the build reads must be in `tools/vanilla_files.txt` (one piece of a path per
  line). The launcher extracts exactly those, and again whenever that file changes. A missing
  entry fails the build on every player's PC with "... is missing".
- A new file the build writes into `build/darkestdiablo` reaches players only once it is listed
  in `SHIPPED` in `launcher/ddcore.py`.
- Never rename or remove `tools/build-plugin.cmd`; the launcher runs it. It may change freely
  (new steps, tests) as long as it still ends with the built mod in `build/darkestdiablo`.
- Standard Python modules our build scripts import must be in `launcher/build_stdlib.py`
  (the launcher .exe carries its own Python). Third-party packages can't be used.

What the launcher does on Update (`launcher/ddmain.py`):

1. Asks GitHub for the newest commit on `main` and downloads that source as a zip (no Git needed).
2. Puts it in `<game folder>\darkestdiablo-src`, keeping `vanilla/` and `build/` from last time.
3. Extracts the files in `tools/vanilla_files.txt` from the player's game folder (first time, or
   when the list or the game build changed), with `tools/extract.cpp` built into the launcher.
4. Runs `tools\build-plugin.cmd`, with the launcher itself standing in as `python.exe`. The plugin
   needs Visual Studio's C++ tools; without them the launcher explains how to get them. The
   output goes to `darkestdiablo-src\build.log`, which the launcher opens if the build fails.
5. Copies only the changed `SHIPPED` files into `Mods\darkestdiablo` and deletes the ones it put
   there before that are gone now. Nothing else there is touched (logs, anything the plugin
   writes), and saves live in `Saved Games\Diablo II Resurrected\mods\darkestdiablo`, which
   the launcher never touches.
6. Records the commit in `<game folder>\darkestdiablo-install.json`.

Getting the launcher itself: the Launcher workflow builds the .exe on every push to `main` that
changes it (Actions, newest run, artifact `DarkestDiabloLauncher`). The launcher keeps itself up
to date after that, see "The launcher updating itself" below.

## The launcher updating itself

No release is needed for a launcher change: merging it into `main` is enough.
- On every push to `main` that changes the launcher, the Launcher workflow stamps the build
  number (the workflow's run number) into `launcher/build_info.py`, builds the .exe and puts it
  on the GitHub pre-release with tag `launcher`, together with `launcher.json`
  (`build`, `commit`, `sha256`, `size`). That pre-release only ever holds the newest launcher;
  its files are replaced each time. It is not a mod release: the releases channel reads
  `/releases/latest`, which skips pre-releases.
- When the launcher opens, it reads `launcher.json` (`launcher/ddself.py`). If the build number
  is higher than its own, it downloads the .exe next to itself as `<name>.new`, checks size and
  sha256, starts a small script that waits for the launcher to close, moves the new .exe over the
  old one and opens it, then closes. Any failure (offline, folder not writable, bad download)
  leaves the old launcher in place and it carries on as usual.
- A launcher built anywhere else (`tools\build-launcher.cmd`, a pull request's build) has build 0
  and never replaces itself, so a test build isn't overwritten.

## What the launcher does (both channels)

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
5. Installs the mod: on the main channel by building it (above); on the releases channel by
   downloading the newest release into `<game folder>\Mods\darkestdiablo`. Either way it writes
   `<game folder>\darkestdiablo-install.json` (what is installed and the file list).

Every run after that: looks for something newer (a new commit on `main`, or a newer release). If
there is, it shows **Update**; otherwise **Play**. Play checks that the
Battle.net app is open and starts `D2RLoader.exe` in the game folder. The first time it reminds the
player to set D2RLoader's Default mod to `darkestdiablo`.

Settings (folders, an optional GitHub token) are kept in `%APPDATA%\DarkestDiablo\launcher.json`.

**Our own setups are left alone.** If `Mods\darkestdiablo` is the link made by `tools\link.cmd`,
the launcher says "Developer build" and only offers Play. Keep developing exactly as before.

## The releases channel: what a release is made of

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

Paths are relative to `Mods\darkestdiablo`, with forward slashes. Only what `SHIPPED` in
`launcher/ddcore.py` lists is shipped (today `modinfo.json`, `darkestdiablo.mpq/`,
`d2rloader/plugins/` and the mod's own settings and data in `d2rloader/`); logs and settings the
game writes into your build stay on your PC. The main channel installs the same set.

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
| `launcher/ddmain.py` | The main channel: download `main`, extract game files, run the build, install. |
| `launcher/build_stdlib.py` | Standard modules bundled for the build scripts the .exe runs. |
| `tools/extract.cpp`, `tools/extract/`, `tools/build-extract.cmd` | The game file reader (CascLib), bundled in the launcher. |
| `tools/vanilla_files.txt` | The game files the build reads. |
| `launcher/darkestdiablo_launcher.py` | The window. Everything is drawn in code: no images, no sound (silent on purpose). |
| `tools/release.py` | Makes and publishes releases. Shares the manifest code with the launcher. |
| `launcher/ddself.py`, `launcher/build_info.py` | The launcher updating itself; its build number. |
| `tools/build-launcher.cmd` | Builds `build\launcher\DarkestDiabloLauncher.exe` with PyInstaller. |
| `tests/test_launcher.py` | Install, delta, fallback and safety tests: `python -m unittest discover tests`. |
| `.github/workflows/launcher.yml` | Runs the tests and builds the .exe on Windows. |

Run it from source with `python launcher\darkestdiablo_launcher.py` (Python 3.10+, standard
library only). If the manifest ever needs a field old launchers can't understand, raise
`MANIFEST_FORMAT` in `ddcore.py`; older launchers then tell players to download the new one.

### The repo is public

Since 17 Nov 2026 the repo is public, so the launcher needs no token. The token prompt only
appears if GitHub ever refuses the launcher (a private repo again, or a separate private one).
