# Mephistorghast

A Hades-style roguelite mod for Diablo II: Resurrected. Design: `docs/design.md`.
Rules for Claude sessions: `CLAUDE.md`.

## One-time setup (each of us)
1. **Frozen game copy.** Check your D2R build: the last field on line 2 of `.build.info` in
   your D2R folder must match `D2R_VERSION`. Then copy the game, leaving out mod leftovers:

       robocopy "<your D2R folder>" "D:\Games\D2R-mephistorghast" /E /XD Mods /XF D2RHUD.dll D2RHUD_Config.txt

2. **Vanilla tables.** Extract `data/global/excel` and `data/local/lng/strings` from the
   frozen copy into `vanilla/` (CascView, or `tools/extract.cpp`). Also extract
   `data/global/dataversionbuild.txt`. Write the build number into `vanilla/D2R_VERSION`.
3. **Build and link.** Needs Python 3.

       python tools\build.py
       tools\link.cmd "D:\Games\D2R-mephistorghast"

4. **Shortcut.** Desktop shortcut to the frozen copy's `D2R.exe`, Target ending in
   `-mod mephistorghast -txt`. Open the Battle.net app first, then press Esc at the
   connect screen to reach offline characters.

## Every change
Edit `src/`, run `python tools\build.py`, launch the shortcut. The link means the game
always sees the latest build.
