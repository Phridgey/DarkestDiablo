# Darkest Diablo

A Hades-style roguelite mod for Diablo II: Resurrected. Design: `docs/design.md`.
Rules for Claude sessions: `CLAUDE.md`.

The mod has two parts:
- **Data** (tables, maps, text): built by `tools/build.py` into `build/darkestdiablo/darkestdiablo.mpq`.
- **Code** (run rules, camp, forks): a [D2RLoader](https://d2rloader.net) plugin in `src/plugin`,
  built with the official [D2RLoader Plugin SDK](https://github.com/D2RLoader/PluginSDK) (MIT).

## Playing (no repo needed)
Get **DarkestDiabloLauncher.exe** (from the newest run of the Launcher workflow under Actions,
until there are releases) and run it. It finds your D2R, asks where to put the Darkest Diablo game
folder (any folder, any drive), copies the game there (your normal install is never changed) and
helps you add D2RLoader. Then it builds the newest `main` on your PC and installs it. After that
it shows **Update** when `main` has new changes, otherwise **Play**. You still need the Battle.net
app open to play, and for now Visual Studio's C++ tools (the plugin is built on your PC).

**Contributors:** whatever is on `main` is what the launcher builds, with the same
`tools\build-plugin.cmd` you use. So `main` must always build, and every game file the build reads
must be listed in `tools/vanilla_files.txt`. Later, players get releases instead: small updates
that hold only the files that changed. Both are explained in `docs/updates.md`.

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
3. **Vanilla files.** `tools/vanilla_files.txt` lists the game files the build reads. Build the
   reader once with `tools\build-extract.cmd` (fetches CascLib), then extract them from the
   frozen copy and write its build number:

       build\extract\extract.exe "<your game folder>" vanilla @tools\vanilla_files.txt
       echo 3.4.95032> vanilla\D2R_VERSION

   (The launcher does this by itself in `<game folder>\darkestdiablo-src`.)
4. **Build and link.**

       tools\build-plugin.cmd
       tools\link.cmd "<your game folder>"

5. **Play.** Open the Battle.net app, then run `D2RLoader.exe` in the frozen copy. The first time,
   open the D2RLoader settings (gear button under Play), set **Default mod** to
   `darkestdiablo`, Apply, and let it restart.

## What works so far
- **Waypoints:** the plugin unlocks every waypoint when a character enters a game, so the next
  stop is always reachable ("scouted ahead"). Per-stop unlocks wait for a waypoint service in the
  SDK (D2RLoader/PluginSDK issue #9). Act I slots: 1 = Den of Evil arena (was Cold Plains),
  2 = Burial Grounds entry (was Stony Field), 3 = Tree of Inifuss entry (was Dark Wood). The waypoint menu comes from `actinfo.txt` in 3.x.
- **Arenas** (plugin, `Arenas` table): arriving starts the waves, wave 1 after 1 s, then one every
  10 s whatever is still alive. When the last wave has spawned and everything is dead (every
  boss included, any kill order), a Town Portal scroll goes into the inventory. Entering a stop
  the hero may not enter (cleared, e.g. back through that portal) starts no waves.
  - **Den of Evil:** a wide cave of 2x2 open cave rooms (`src/maps/den_of_evil.json`) on a spare
    cave level (13), not the real Den, so the Den quest (and Akara's free respec) never happens.
    The fight is in the bottom-left hall: Fallen from both ends, zombies from the north, shamans
    close by, then Corpsefire and his pack. Corpsefire always drops a Book of Skill (the quest's
    skill point); the arena waits for him to die.
  - **Burial Grounds:** you arrive by waypoint in a small crypt room, prep, and take the stairs
    up. You come out at the graveyard's crypt door; the waves come at the plaza by the hanging
    tree: zombies and skeletons, then Blood Raven with zombies. Built on two unused cave levels
    (9 = arena, 10 = entry room), because outdoor levels are placed somewhere new every game.
    See `src/patches/020_burial_grounds.py`.
  - **Tree of Inifuss** (fork 1 MF run): same setup (11 = arena, 12 = entry room). The stairs
    come out of a cave mouth beside the tree. Eight waves: Fallen and shamans, then Rakanishu,
    then lancers, then brutes and quill rats, then Treehead Woodfist. Won only when both
    Rakanishu and Treehead are dead. Every drop is 1.4 times as likely to be magic or better (on
    top of the player's magic find), and Treehead always drops one extra set or unique.
    See `src/patches/030_inifuss.py` and `035_inifuss_loot.py`.
  - **Mausoleum** (fork 1 crafting hunt): a round crypt hall on level 14, made with
    compose_map's `circle` (Blizzard has no round crypt room) and ringed with crypt pillars, bone
    piles and statues. Waypoint at the west edge. Six waves of undead that drop only runes, gems,
    potions and gold. See `src/maps/mausoleum.json` and `src/patches/060_mausoleum.py`.
- **No keys** drop anywhere (`src/patches/005_no_keys.py`).
- **Bigger inventory:** every hero's bag is 10x5 instead of 10x4 (`src/patches/055_inventory_row.py`).
  The panel's background gets its extra row at build time from the game's own picture.
- **Route map**: every waypoint opens it instead of Blizzard's list (outside town it has
  "Return to town"); `dd-map` opens it anywhere. It draws Act I as a tree, and each hero unlocks
  it in order (`src/plugin/route.h`): Den, Blood Raven, fork 1 (2 picks, the same one twice
  allowed), Tristram, fork 2, Andariel. Only the current tier is white and clickable. Progress is
  kept per hero in the save folder (`camp/slot<N>/progress/<hero>.txt`). The layout exists twice: built into
  the plugin, which is the copy the game uses, and as a mod file
  (`src/data/data/global/ui/layouts/darkestdiablo/RouteMaphd.json`). Change both and rebuild the
  plugin; `tests/test_route_map.py` fails when they differ.
- **The Campfire** (`docs/campfire.md`; built, not yet tried in the game): three save slots, the
  character list staged at the campfire, recruiting on the hero screen (a new save offers every
  class, after that only classes a traveler brought), travelers in town, permanent death, and a
  bigger shared stash per save. Using the town bonfire goes to the Campfire.
  The build needs the game's menu layouts and a few model files (all in `tools/vanilla_files.txt`).
  Console: `dd-camp`, `dd-slot <1-3>`, `dd-traveler`.
- **Dodge** is shelved (not in the game): its code is kept in `shelved/dodge/`, see `docs/dodge-roll.md`.
- **Console** (Ctrl + backtick; the launcher sets `enable_console = true` in `d2rloader/config/d2rloader.toml`
  on every Update and Play, so after the game's first start it is on; type `debug_mode` for cheats):
  `dd` (plugin running?), `dd-tp` (TP scroll), `dd-waves` (restart the waves of the arena you
  stand in), `dd-map` (route map). Handy for testing: `location player` prints your position;
  `cheat mspawn zombie1,3 @ x,y` tests a spawn spot; `goto <level id>`; `god 1`.

`tools\set_waypoints.py` still exists for editing a closed save's waypoints by hand.

## Next up
- Draw the map as the tree from the design sketch (forks side by side, lines between nodes).
- Blood Raven's fork (Blood Raven MF / Mausoleum / Crypt), then Tristram.
- Tuning: lighter Den waves; black edges around single-room arenas (see `docs/design.md`).
- Tree of Inifuss: play a full run to the TP scroll (waves and stairs tested; clearing not yet).

## Every change
- Data or patches changed: `python tools\build.py`
- Plugin changed: `tools\build-plugin.cmd` (also rebuilds the data)
- Ready for players: `python tools/release.py <version> --publish` (see `docs/updates.md`)

Then start `D2RLoader.exe`. The link means the game always sees the latest build.
To check the plugin is running: D2RLoader's log (`d2rloader\logs\d2rloader.log` in the game
folder) lists "Darkest Diablo ... [mod]" under plugins, and the plugin's own log
(`build\darkestdiablo\d2rloader\logs\darkestdiablo.log`) says "Darkest Diablo plugin <version> loaded."

## Known problem: keep the game copy on the same drive as your normal install
A game copy on another drive than your normal D2R install closes by itself at startup, with no
crash report (Fabrizio's PC: install on D:, copies on E:). Tried on 2026-11-17: a straight copy
of the working D: copy, and a fresh copy made by the launcher. It stays the same with the working
copy's game data, D2RLoader files and settings, and even the same mod through the same link; only
the drive differs. Battle.net's `.product.db` in every copy names the normal install's folder, so
Blizzard's own code may refuse to run elsewhere. Working around that would mean getting around
Blizzard's protection, which we never do (`CLAUDE.md`). So in the launcher, put the game folder
on the drive where D2R is installed.
