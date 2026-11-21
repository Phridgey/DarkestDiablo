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
  The game keeps a second copy of its tables in `global/excel/base` (rules without the Warlock
  expansion), so `tools/build.py` runs every patch on that copy too; write patches that work on both.
- `src/plugin/` — the code layer: a D2RLoader plugin built with the D2RLoader Plugin SDK 0.3.0
  (fork menu via waypoints, run and camp state, item rolls). Use SDK services; do not patch
  game memory by hand, and never try to get around Blizzard's protection (`D2R_loader.dll`).
  One exception, agreed by both of us: the plugin may READ a few game values the SDK doesn't
  offer (first: the hero's position, for spawns placed relative to the town fire), from the
  frozen build in `D2R_VERSION` only. The SDK has no read service, so reads use Windows'
  `ReadProcessMemory` on our own process (a bad address fails the read instead of crashing).
  Writes (agreed by both of us, 2026-11-19):
  the plugin may change existing values in game memory (stats, buff/debuff values and end times)
  through `src/plugin/gamewrite.h` only, the one file that writes, on the frozen build only, into
  plain data memory (never code). Never change game code, and never write anywhere else. Each value read and its address go in
  `docs/game-memory.md`, and the plugin turns the feature off if the game build differs.
- Game information (hero, monsters, objects, items, stats, buffs/debuffs, events) comes from the
  game-state API, `src/plugin/gameapi.h`; its guide is `docs/game-state-api.md`. Use it instead of
  new reads of your own, and update that page in the same pull request as any API change.
- `src/patches` can also change the game's menu layouts with `t.layout("<name>")` (parsed JSON).
- `tools/build-plugin.cmd` — builds the plugin and places it in the mod (Windows, needs Visual Studio).
- `tools/build.py` — builds `build/darkestdiablo/darkestdiablo.mpq/`.
- `tools/link.cmd` — run once on Windows: links the build into the frozen game's `Mods` folder.
- `launcher/` — the players' launcher (finds D2R, makes the game folder, Update / Play). For now it
  builds the newest `main` on the player's PC with `tools\build-plugin.cmd` (`launcher/ddmain.py`).
- `tools/vanilla_files.txt` — every game file the build reads. The launcher extracts exactly these.
  A change that reads a new game file must add it here, or players' builds fail.
- `tools/release.py` — publishes the build as a GitHub release the launcher can update from.
  Read `docs/updates.md` before changing what the build outputs, where files go, or the release format.

## Workflow
1. Change patches, data or the plugin. 2. `python tools/build.py` (data) or
   `tools\build-plugin.cmd` (plugin + data). 3. With the Battle.net app open, run `D2RLoader.exe`
   in the frozen copy; its default mod is `darkestdiablo`. Its log is `d2rloader/logs/d2rloader.log`.
4. One branch per feature, merged by pull request.
5. `main` is what players' launchers build, so only merge what builds with
   `tools\build-plugin.cmd` from a fresh `vanilla/` extracted with `tools/vanilla_files.txt`.
   A new file in `build/darkestdiablo` reaches players only once it is in `SHIPPED`
   (`launcher/ddcore.py`).
6. To ship releases (later): `python tools/release.py <version> --publish` from an up-to-date `main`
   build. Updates are deltas of whole changed files; never edit or delete a published release.

## Working together (both people's Claude sessions)
Players and both of us get the mod built from `main`, so content only counts once it is there.
- Start every branch from the newest `main`, and merge `main` into it again before the pull request.
- Open the pull request as soon as the branch works in your game. A branch that is only pushed
  is in nobody else's game and drifts from `main` (the town waypoint route map sat on its own
  branch while `main` got the Campfire, so our two games differed under the same version).
- The mod version is `Version` in `src/plugin/campfire.h`, the only place it lives. Raise it in
  every pull request that changes the game, so one version number always means one game. Never
  set it lower than what is on `main`.
- To check two games match, compare the plugin log line "Darkest Diablo plugin <version> loaded"
  and, for releases, the `build` section of `manifest.json` (commit, tools, checksums).

## Paths
- Never hardcode drive letters or game folders. Everyone's game folder is wherever they chose
  in the launcher; tools take it as an argument.

## Writing style
Plain, jargon-free language in docs and commit messages.

## Lessons learned (map and plugin work)
- Close the game before `tools\build-plugin.cmd`: the running game locks the plugin DLL.
- When a patch stops changing a table (or is shelved), the build stops making that file, but a
  file copied in earlier stays in `Mods/darkestdiablo` and the game keeps loading it. The
  launcher deletes files its last install placed that the new build no longer makes; a manual
  copy (robocopy) does not, so delete those files by hand (Dodge's skills, skilldesc, charstats
  and strings/skills.json, in `excel` and `excel/base`, on 2026-11-19).
- Outdoor levels (DrlgType 3, e.g. the Graveyard) are placed by the outdoor map builder somewhere
  new every game, so spawn coordinates can't be known. Build arenas on unused cave levels
  (fixed `OffsetX/Y`, own `Layer`) and copy the look from the real level's row.
- World sub-tile = (level OffsetX/Y + map tile) * 5 + 2. `location player` prints yours.
- Many tiles refuse monster spawns ("Monster not created") even where the map looks open, and
  the game only spawns near the player. Test every spawn spot in game with
  `cheat mspawn <id>,<n> @ x,y` before putting it in the plugin.
- Spawning right on arrival fails while the map loads; wait about 1 s.
- Stairs must use Blizzard's pairing: the graveyard's crypt door pairs with crypt stairs
  (lvlwarp 6 and 8). Cave stairs there crashed the game. A cave mouth (lvlwarp 2) on the arena
  side with crypt stairs (8) on the entry side works too.
- A hand-made map with exits needs `Scan = 1` in its lvlprest row, or the game never registers
  its warp tiles (BC_VERIFY ptWarp in the console, and the stairs do nothing). Templates copied
  from maps without exits (the Tree of Inifuss) have 0.
- Plugin panel buttons: use the ModalButton style (pressed/hovered/disabled frames, `textString`).
  The game drops button messages with targets it doesn't know, so send a real command
  (`PanelManager:OpenPanel:darkestdiablo/travel-<level>`) and catch it in the UI-message listener.
- Crowded arenas sometimes refuse a spawn ("Monster not created") and the plugin can't see it, so
  an arena clears with up to `MissingSpawnSlack` monsters unaccounted for.
- `createItem` refuses quest items (Book of Skill: `Unsupported`). Give those through a monster's
  treasure class instead.
- Console typing through computer use: the clipboard paste drops newlines; press Return after
  each line. `cheat killmonster` kills one monster next to the player (handy to test clears).
- `ExecuteConsoleCommand` returns true when the command was accepted, not when it worked; check
  the in-game console for errors.
- A new monster row (a copy with its own Id) is invisible until `data/hd/character/monsters.json`
  maps its Id to a model (`t.data("hd/character/monsters.json")[new] = models[old]`).
- A monster drops at most 6 items per death. With negative Picks the entries drop in order, so a
  guaranteed drop must be listed first; Corpsefire's book and Treehead's bonus, listed last, never
  dropped. Quest items (Book of Skill: `quest`, `questdiffcheck` in misc.txt) only drop while their
  quest is open; clear those fields to drop them anywhere.
- Copying a whole levels.txt row also copies `Quest`. With Quest = 1 the game counts the level's
  monsters for a quest, and an arena (which starts empty) completes it after a few kills.
- Creating a Unique with `createItem` and `RandomQualityRecord` crashed the game right after it
  succeeded. Give guaranteed set/unique items through a loot table instead.
- Cave walls are floor tiles (walls hide in the tile sheets' walk flags, `.dt1`), so a room's
  open space can't be read from the `.ds1` alone. Act I cave rooms are all 24x24 with at most an
  8x8 open square; the Pit's wide areas are open rooms joined edge to edge.
- A plugin can show, hide and find widgets (WidgetService) but not change their text. For text
  that varies, make one widget per case and show the right one (`tools/camp_layouts.py`).
- A plugin panel opened for the first time can't be found (`findPanel`) until the game has built
  it, so hiding widgets right after `openPanel` does nothing and every widget shows. Set them
  again a few times over the next second (`OpenSlotsPanel` in `campfire.cpp`).
- The plugin hears the menus too: `FrontEndNavigation:ToFrontEnd` comes before the main menu
  loads, `CharacterCreate:Select:<Class>` and `CharacterCreate:Create` on the hero screen.
