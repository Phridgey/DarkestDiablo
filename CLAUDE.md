# Darkest Diablo — rules for Claude sessions

A roguelite mod for Diablo II: Resurrected, built by two people. Read `docs/design.md` before design work.

## Game version
- Target build: see `D2R_VERSION` (3.4.95032). Both of us play a frozen copy of the game that Battle.net never updates.
- Never change the target build without both people agreeing. The DLL depends on it.

## Never
- Never commit Blizzard files. `vanilla/` and `build/` are gitignored.
- Never hand-edit a full copy of a vanilla table into the repo. Change tables with patch scripts in `src/patches/`.
- Never touch the normal Battle.net install of D2R. Work only with the frozen copy.
- Never take the mod online.

## How the repo works
- `vanilla/` — tables extracted from your own game copy (`tools/extract.cpp` or CascView).
- `src/data/` — new files copied into the mod as they are (same folder layout as the game's data).
- `src/patches/*.py` — scripts that change vanilla tables. Each defines `patch(t)`; run in name order.
- `src/plugin/` — the code layer: a D2RLoader plugin built with the D2RLoader Plugin SDK 0.3.0
  (fork menu via waypoints, run and camp state, item rolls). Use SDK services; do not patch
  game memory by hand, and never try to get around Blizzard's protection (`D2R_loader.dll`).
- `tools/build-plugin.cmd` — builds the plugin and places it in the mod (Windows, needs Visual Studio).
- `tools/build.py` — builds `build/darkestdiablo/darkestdiablo.mpq/`.
- `tools/link.cmd` — run once on Windows: links the build into the frozen game's `Mods` folder.

## Workflow
1. Change patches, data or the plugin. 2. `python tools/build.py` (data) or
   `tools\build-plugin.cmd` (plugin + data). 3. With the Battle.net app open, run `D2RLoader.exe`
   in the frozen copy; its default mod is `darkestdiablo`. Its log is `d2rloader/logs/d2rloader.log`.
4. One branch per feature, merged by pull request.

## Writing style
Plain, jargon-free language in docs and commit messages.
