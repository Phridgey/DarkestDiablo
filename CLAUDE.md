# Mephistorghast — rules for Claude sessions

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
- `src/dll/` — the code layer (fork menu via waypoints, run and camp state, item rolls).
- `tools/build.py` — builds `build/mephistorghast/mephistorghast.mpq/`.
- `tools/link.cmd` — run once on Windows: links the build into the frozen game's `Mods` folder.

## Workflow
1. Change patches or data. 2. `python tools/build.py`. 3. Launch the Mephistorghast shortcut
   (`D2R.exe -mod mephistorghast -txt`, Battle.net app open, press Esc at the connect screen).
4. One branch per feature, merged by pull request.

## Writing style
Plain, jargon-free language in docs and commit messages.
