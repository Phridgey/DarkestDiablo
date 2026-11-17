# Darkest Diablo

A Hades-style roguelite mod for Diablo II: Resurrected. Design: `docs/design.md`.
Rules for Claude sessions: `CLAUDE.md`.

The mod has two parts:
- **Data** (tables, maps, text): built by `tools/build.py` into `build/darkestdiablo/darkestdiablo.mpq`.
- **Code** (run rules, camp, forks): a [D2RLoader](https://d2rloader.net) plugin in `src/plugin`,
  built with the official [D2RLoader Plugin SDK](https://github.com/D2RLoader/PluginSDK) (MIT).

## Playing (no repo needed)
Download **DarkestDiabloLauncher.exe** from the newest release and run it. It finds your D2R,
asks where to put the Darkest Diablo game folder (any folder, any drive), copies the game there
(your normal install is never changed), helps you add D2RLoader, and downloads the mod. After
that it shows **Update** when there is one, otherwise **Play**. You still need the Battle.net app
open to play.

**Contributors:** players get our work through GitHub releases, as small updates that hold only
the files that changed. How releases work and how to publish one: `docs/updates.md`.

## One-time setup (each of us, to develop)
You need: your own copy of D2R, Python 3, Git, Visual Studio with "Desktop development with C++".

1. **Frozen game copy and D2RLoader.** Easiest: run the launcher (`python launcher\darkestdiablo_launcher.py`
   or the .exe) and let it make the game folder wherever you like and add D2RLoader. By hand
   instead: check that the last field on line 2 of `.build.info` in your D2R folder matches
   `D2R_VERSION`, then copy the game, leaving out mod leftovers:

       robocopy "<your D2R folder>" "<your game folder>" /E /XD Mods /XF D2RHUD.dll D2RHUD_Config.txt

   and extract D2RLoader 1.3.1 or later (https://www.nexusmods.com/diablo2resurrected/mods/1119)
   next to `D2R.exe`. It warns that 3.4 is not fully tested; that is expected.
   Only ever mod this copy. Leave your normal Battle.net install alone.
2. **Your build instead of a release.** If the launcher installed a release into
   `<your game folder>\Mods\darkestdiablo`, delete that folder before step 4 so the link can
   take its place. The launcher sees the link and leaves it alone ("Developer build").
3. **Vanilla files.** Extract these from the frozen copy into `vanilla/` (CascView, or
   `tools/extract.cpp`), keeping their folders: `data/global/excel`, `data/local/lng/strings`,
   `data/global/dataversionbuild.txt`, and for the hand-made maps the room pieces
   `data/global/tiles/act1/caves` and `data/hd/env/preset/act1/caves`. Write the build number
   into `vanilla/D2R_VERSION`.
4. **Build and link.**

       tools\build-plugin.cmd
       tools\link.cmd "<your game folder>"

5. **Play.** Open the Battle.net app, then run `D2RLoader.exe` in the frozen copy. The first time,
   open the D2RLoader settings (gear button under Play), set **Default mod** to
   `darkestdiablo`, Apply, and let it restart.

## What works so far
- **Waypoints:** the plugin unlocks every waypoint when a character enters a game, so the next
  stop is always reachable ("scouted ahead"). Per-stop unlocks wait for a waypoint service in the
  SDK (D2RLoader/PluginSDK issue #9). Act I slots: 1 = Den of Evil (was Cold Plains),
  2 = Burial Grounds entry (was Stony Field). The waypoint menu comes from `actinfo.txt` in 3.x.
- **Arenas** (plugin, `Arenas` table): arriving starts the waves, wave 1 after 1 s, then one every
  10 s whatever is still alive. When the last wave has spawned and everything is dead (the boss
  included, any kill order), a Town Portal scroll goes into the inventory.
  - **Den of Evil:** one room (`src/maps/den_of_evil.json`). Fallen, zombies and shamans, then
    Corpsefire and his pack.
  - **Burial Grounds:** you arrive by waypoint in a small crypt room, prep, and take the stairs
    up. You come out at the graveyard's crypt door; the waves come at the plaza by the hanging
    tree: zombies and skeletons, then Blood Raven with zombies. Built on two unused cave levels
    (9 = arena, 10 = entry room), because outdoor levels are placed somewhere new every game.
    See `src/patches/020_burial_grounds.py`.
- **Route map** (`dd-map`): a panel listing the Act I stops. Den and Burial Grounds are
  clickable and send you there; the rest are greyed out. The layout is a mod file
  (`src/data/data/global/ui/layouts/darkestdiablo/RouteMaphd.json`), so changing it needs only a
  game restart, not a plugin rebuild.
- **Console** (Ctrl + backtick, with `enable_console = true` in `d2rloader/config/d2rloader.toml`):
  `dd` (plugin running?), `dd-tp` (TP scroll), `dd-waves` (restart the waves of the arena you
  stand in), `dd-map` (route map). Handy for testing: `location player` prints your position;
  `cheat mspawn zombie1,3 @ x,y` tests a spawn spot; `goto <level id>`; `god 1`.

`tools\set_waypoints.py` still exists for editing a closed save's waypoints by hand.

## Next up
- Town waypoint opens the route map instead of Blizzard's list: the click sends
  `PanelManager:OpenPanel:WaypointsPanelExpansion`; the plugin's UI-message listener can
  consume it and open the map.
- Draw the map as the tree from the design sketch (forks side by side, lines between nodes).
- Blood Raven's fork (Blood Raven MF / Mausoleum / Crypt), then Tristram.
- Tuning: lighter Den waves; black edges around single-room arenas (see `docs/design.md`).
- Rename the repo folder on Fabrizio's PC from `E:\Dev\mephistorghast` to `darkestdiablo`
  (blocked while a Claude session has it open), then rerun `tools\link.cmd`.

## Every change
- Data or patches changed: `python tools\build.py`
- Plugin changed: `tools\build-plugin.cmd` (also rebuilds the data)
- Ready for players: `python tools/release.py <version> --publish` (see `docs/updates.md`)

Then start `D2RLoader.exe`. The link means the game always sees the latest build.
To check the plugin is running: D2RLoader's log (`d2rloader\logs\d2rloader.log` in the game
folder) lists "Darkest Diablo ... [mod]" under plugins, and the plugin's own log
(`build\darkestdiablo\d2rloader\logs\darkestdiablo.log`) says "Darkest Diablo plugin <version> loaded."

## Later
- Move the game copy to E:. A straight copy of the D: frozen copy crashes at startup on E:
  (D2RLoader.exe, C++ exception in KERNELBASE, right after the string tables load). Next try:
  a fresh install or a fresh frozen copy made directly on E:.
