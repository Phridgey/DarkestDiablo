# Mephistorghast

A Hades-style roguelite mod for Diablo II: Resurrected. Design: `docs/design.md`.
Rules for Claude sessions: `CLAUDE.md`.

The mod has two parts:
- **Data** (tables, maps, text): built by `tools/build.py` into `build/mephistorghast/mephistorghast.mpq`.
- **Code** (run rules, camp, forks): a [D2RLoader](https://d2rloader.net) plugin in `src/plugin`,
  built with the official [D2RLoader Plugin SDK](https://github.com/D2RLoader/PluginSDK) (MIT).

## One-time setup (each of us)
You need: your own copy of D2R, Python 3, Git, Visual Studio with "Desktop development with C++".

1. **Frozen game copy.** Check your D2R build: the last field on line 2 of `.build.info` in
   your D2R folder must match `D2R_VERSION`. Then copy the game, leaving out mod leftovers:

       robocopy "<your D2R folder>" "D:\Games\D2R-mephistorghast" /E /XD Mods /XF D2RHUD.dll D2RHUD_Config.txt

   Only ever mod this copy. Leave your normal Battle.net install alone.
2. **D2RLoader.** Download D2RLoader 1.3.1 or later from
   https://www.nexusmods.com/diablo2resurrected/mods/1119 and extract it into the frozen copy
   (next to `D2R.exe`). It warns that 3.4 is not fully tested; that is expected.
3. **Vanilla tables.** Extract `data/global/excel`, `data/local/lng/strings` and
   `data/global/dataversionbuild.txt` from the frozen copy into `vanilla/` (CascView, or
   `tools/extract.cpp`). Write the build number into `vanilla/D2R_VERSION`.
4. **Build and link.**

       tools\build-plugin.cmd
       tools\link.cmd "D:\Games\D2R-mephistorghast"

5. **Play.** Open the Battle.net app, then run `D2RLoader.exe` in the frozen copy. The first time,
   open the D2RLoader settings (gear button under Play), set **Default mod** to
   `mephistorghast`, Apply, and let it restart.

## Every change
- Data or patches changed: `python tools\build.py`
- Plugin changed: `tools\build-plugin.cmd` (also rebuilds the data)

Then start `D2RLoader.exe`. The link means the game always sees the latest build.
To check the plugin is running: D2RLoader's log (`d2rloader\logs\d2rloader.log` in the game
folder) lists "Mephistorghast 0.1.0 ... [mod]" under plugins, and the plugin's own log
(`build\mephistorghast\d2rloader\logs\mephistorghast.log`) says "Mephistorghast plugin loaded."
