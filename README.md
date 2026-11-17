# Darkest Diablo

A Hades-style roguelite mod for Diablo II: Resurrected. Design: `docs/design.md`.
Rules for Claude sessions: `CLAUDE.md`.

The mod has two parts:
- **Data** (tables, maps, text): built by `tools/build.py` into `build/darkestdiablo/darkestdiablo.mpq`.
- **Code** (run rules, camp, forks): a [D2RLoader](https://d2rloader.net) plugin in `src/plugin`,
  built with the official [D2RLoader Plugin SDK](https://github.com/D2RLoader/PluginSDK) (MIT).

## One-time setup (each of us)
You need: your own copy of D2R, Python 3, Git, Visual Studio with "Desktop development with C++".

1. **Frozen game copy.** Check your D2R build: the last field on line 2 of `.build.info` in
   your D2R folder must match `D2R_VERSION`. Then copy the game, leaving out mod leftovers:

       robocopy "<your D2R folder>" "D:\Games\D2R-darkestdiablo" /E /XD Mods /XF D2RHUD.dll D2RHUD_Config.txt

   Only ever mod this copy. Leave your normal Battle.net install alone.
2. **D2RLoader.** Download D2RLoader 1.3.1 or later from
   https://www.nexusmods.com/diablo2resurrected/mods/1119 and extract it into the frozen copy
   (next to `D2R.exe`). It warns that 3.4 is not fully tested; that is expected.
3. **Vanilla files.** Extract these from the frozen copy into `vanilla/` (CascView, or
   `tools/extract.cpp`), keeping their folders: `data/global/excel`, `data/local/lng/strings`,
   `data/global/dataversionbuild.txt`, and for the hand-made maps the room pieces
   `data/global/tiles/act1/caves` and `data/hd/env/preset/act1/caves`. Write the build number
   into `vanilla/D2R_VERSION`.
4. **Build and link.**

       tools\build-plugin.cmd
       tools\link.cmd "D:\Games\D2R-darkestdiablo"

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
- **The Campfire** (`docs/campfire.md`; built, not yet tried in the game): three save slots, the
  character list staged at the campfire, recruiting on the hero screen (a new save offers every
  class, after that only classes a traveler brought), travelers in town, permanent death, and a
  bigger shared stash per save. The town bonfire opens the stash, which has a Campfire button.
  The build needs the game's menu layouts: extract `data/global/ui/layouts` into `vanilla/`
  (and the few other files `docs/campfire.md` lists under Building).
  Console: `dd-camp`, `dd-slot <1-3>`, `dd-traveler`.
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

Then start `D2RLoader.exe`. The link means the game always sees the latest build.
To check the plugin is running: D2RLoader's log (`d2rloader\logs\d2rloader.log` in the game
folder) lists "Darkest Diablo ... [mod]" under plugins, and the plugin's own log
(`build\darkestdiablo\d2rloader\logs\darkestdiablo.log`) says "Darkest Diablo plugin <version> loaded."

## Later
- Move the game copy to E:. A straight copy of the D: frozen copy crashes at startup on E:
  (D2RLoader.exe, C++ exception in KERNELBASE, right after the string tables load). Next try:
  a fresh install or a fresh frozen copy made directly on E:.
