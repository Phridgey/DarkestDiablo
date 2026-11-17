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
- `src/patches` can also change the game's menu layouts with `t.layout("<name>")` (parsed JSON).
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

## Lessons learned (map and plugin work)
- Close the game before `tools\build-plugin.cmd`: the running game locks the plugin DLL.
- Outdoor levels (DrlgType 3, e.g. the Graveyard) are placed by the outdoor map builder somewhere
  new every game, so spawn coordinates can't be known. Build arenas on unused cave levels
  (fixed `OffsetX/Y`, own `Layer`) and copy the look from the real level's row.
- World sub-tile = (level OffsetX/Y + map tile) * 5 + 2. `location player` prints yours.
- Many tiles refuse monster spawns ("Monster not created") even where the map looks open, and
  the game only spawns near the player. Test every spawn spot in game with
  `cheat mspawn <id>,<n> @ x,y` before putting it in the plugin.
- Spawning right on arrival fails while the map loads; wait about 1 s.
- Stairs must use Blizzard's pairing: the graveyard's crypt door pairs with crypt stairs
  (lvlwarp 6 and 8). Cave stairs there crashed the game.
- Plugin panel buttons: use the ModalButton style (pressed/hovered/disabled frames, `textString`).
  The game drops button messages with targets it doesn't know, so send a real command
  (`PanelManager:OpenPanel:darkestdiablo/travel-<level>`) and catch it in the UI-message listener.
- `ExecuteConsoleCommand` returns true when the command was accepted, not when it worked; check
  the in-game console for errors.
- A plugin can show, hide and find widgets (WidgetService) but not change their text. For text
  that varies, make one widget per case and show the right one (`tools/camp_layouts.py`).
- The plugin hears the menus too: `FrontEndNavigation:ToFrontEnd` comes before the main menu
  loads, `CharacterCreate:Select:<Class>` and `CharacterCreate:Create` on the hero screen.
