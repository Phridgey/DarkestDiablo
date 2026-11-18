# The Campfire

The start of every session: pick a save, see the camp, pick a hero, play. This page says what
the player sees, where each part lives, what the game engine forced, and what still has to be
checked in game. The rules themselves are in `src/plugin/camp.h` and are tested without the game
(`tests/camp_tests.cpp`).

## What the player sees

1. **Save slots ("Delves").** After the title screen the hero screen opens with nobody at the
   fire, darkened, and three cards stand side by side over it, Delve 1 to 3, in the game's own
   difficulty-card frame and fonts. Each lists its living heroes as
   "level class" and shows the difficulty it plays on, large at the bottom (the hardest one
   played in that save, which the plugin notes from the game while you play). The button under it says
   Continue, or Start New on an empty Delve. A started Delve has a delete icon, which asks twice.
2. **The Campfire.** The game's hero screen (the classes around the fire) is the character
   selector. The save's living heroes stand at the fire in the gear they were last seen in, each
   with a name plate. Clicking a hero (or its plate) picks it and turns Enter red; Enter loads
   it. Create brings out the name box for a new hero; on a save with no heroes yet the name box is out from the start. Options, Retire and Exit Game are
   on the left; Change save (or Escape) lays the save slots over the fire.
3. **New heroes** stand at the same fire in the game's standard look, and are made the usual
   way (click, name, Create). Only classes that may join right now are shown:
   - a brand new save: every class. The one picked starts the camp; the rest lock.
   - later: only classes a traveler brought (see below). They start at level 1 with nothing.
   - every hero dead and nobody waiting: a traveler turns up by himself with up to 3 classes.
   **Retire** asks which hero to bury. A retired hero counts as a death.
4. **In town.** Each time a hero comes back to town from a stop there is a chance (25%) that
   a traveler arrives with up to 3 locked classes; a notice says "A traveler waits by your fire".
   The class of the last hero that fell is always one of them, once. Using the fire then opens
   Cain's portal where the hero stands (`location player`, `cheat ospawn 189`), the hooded
   traveler (a still, silent figure with Nihlathak's hooded look, `cheat mspawn ddtraveler`, 045_traveler.py) steps out, and 3 s later the hero goes to
   the Campfire. The town lands somewhere new every game, so this can only happen once the hero
   is at the fire. At the Campfire (through the fire, or Save and Exit) only the offered classes stand by the fire; pick one and press Accept:
   the others leave, the camp's heroes come back and the new class waits with the name box ready.
   "Not now" keeps the traveler waiting for later.
5. **The bonfire.** Clicking the Rogue Encampment's bonfire (from any side: it has a bigger click
   box) saves, leaves the game and lands on the Campfire scene, like Save and Exit. To the game the
   fire is a stash chest, the only use we can notice. The town's own stash chest is untouched
   (Sam, 2026-11-18): both open the stash window, and the plugin tells them apart by where the
   hero stands (read from game memory, `docs/game-memory.md`). The hero walks up to what was
   clicked, so if it stands nearer the fire than the stash chest it goes to the Campfire, and
   otherwise the stash opens. (The chest is only about 14 steps from the fire, so 0.11.42's
   "within 20 steps of the fire" sent the chest to the Campfire too.) If the chest can't be
   found, only a hero right at the pit's edge counts as the fire; if the positions can't be
   read, the stash opens.
6. **Death is final.** A fallen hero's files move to the graveyard, with everything it wore and
   carried. Its class locks again. Only the stash carries on.
7. **The stash.** The game's shared stash, with 7 more pages. Each save has its own.
8. **Heroes by the fire.** In the Rogue Encampment the save's other heroes rest around the fire,
   each in its class's own body and the armor weight (light, medium, heavy) of the body armor it
   last wore. They can't be clicked and don't talk. Exact item looks and weapons can't be shown:
   they are monster figures (`046_camp_heroes.py`) wearing the class's look file, placed by the
   plugin once the fire's position is known (`docs/game-memory.md`).

## Choices the engine forced

- **One save folder, one shared stash.** The game reads every character and one shared stash
  from a single folder. So a slot is a set of files: the live folder holds the loaded slot, the
  other slots are parked in `camp/slot<N>/files/`, and picking a slot moves files both ways.
  Settings and loot filters stay put and are shared by all slots.
- **Gear at the fire is a copy, not the save.** The hero screen draws its figures from a layout
  file: a class, a weapon model, a shield model and light, medium or heavy armor per body part.
  The plugin notes what a hero wears while it is played, and writes that into the layout of its
  own panel (`Fire`) each time the Campfire opens. So a hero shows its weapon, shield and armor
  weight as last played. Helmets, gloves, boots and item colors are not shown, and a hero that
  has not been played since this was added stands there empty-handed.
- **Loading the clicked hero.** The game only loads the hero selected in its own character list.
  When a hero is clicked, the other heroes' files wait in `camp/slot<N>/bench/`, the main menu
  loads with one hero in its list, and the plugin presses Play. The bench is emptied again
  before the camp is next looked at. The character list flashes by on the way.
- **Switching hero needs a load.** A game session holds one character. Changing hero means
  leaving the game and loading the other save, so there is a loading screen each time. "Go to
  the fire" makes that two clicks, but it can't be instant.
- **The bonfire can't call the plugin.** The plugin SDK reports UI messages, not clicks on
  world objects. The bonfire therefore opens the one town window we can add a button to, the
  stash. The plugin opens the roster whenever the stash opens in the Rogue Encampment, where
  the fire is the only stash.
- **Text on our panels is fixed.** A plugin can show and hide widgets but not change their
  text. Panels hold a ready-made label for every case and show the right ones. A hidden button
  still takes clicks, so labels are stacked but buttons never are: the traveler has one button
  per place and the plugin looks up which class is offered there. The slot screen's text
  (heroes, difficulty, button words) is written into its layout by the plugin each time it opens.
- **No "player died" event.** The plugin watches for the game's "You died" window and for a
  resurrection. A softcore hero that comes back is sent out of the game at once and buried.
  Hardcore heroes are also caught by the dead mark in their save.

## Where things live

| Path | What |
| --- | --- |
| `src/plugin/camp.h` | The rules: classes, offers, the camp file, slot switching, burying. No game code. |
| `src/plugin/campfire.cpp` | Talks to the game: UI messages, panels, game events, console commands. |
| `tools/camp_layouts.py` | Our three panels (save slots, traveler, roster), written into the mod by the build. |
| `src/patches/040_campfire.py` | Bonfire, stash button, hero screen title. |
| `d2rloader/camp_data.txt` (built) | Where each class stands at the fire and how each item looks; made by `tools/build.py`. |
| `src/d2rloader/` | The mod's D2RLoader settings: `add_shared_tabs = 7`. |

In the save folder (`Saved Games\Diablo II Resurrected\mods\darkestdiablo`):

    camp\active.txt              the loaded slot
    camp\slot<N>\camp.txt        that slot's camp: heroes, recruits, fallen classes, the traveler's offer
    camp\slot<N>\files\          that slot's saves and stash while another slot is loaded
    camp\graveyard\slot<N>\      fallen heroes
    camp\slot<N>\bench\        the other heroes' files while one hero is being loaded
    camp\trash\slot<N>-<time>\  a deleted save's files, kept in case it was a mistake
    camp\options.txt             optional: "slot_picker 0", "traveler_chance 50", "campfire_screen 0"

The camp follows the save files: each time the menu loads, new saves join, deleted saves leave,
dead ones are buried. Back up the whole folder to back up everything.

Console: `dd-camp` (who is in the camp; in town it opens the roster), `dd-slot <1-3>` (main
menu), `dd-traveler` (in town: a traveler now).

## Building

The build changes two of the game's menu layouts and reads item and object models from the game.
All of them are in `tools/vanilla_files.txt`, which the launcher and the README's setup extract.
Nothing of Blizzard's is in the repo.

## Tried in the game so far

- 0.11.52: a 40% bigger click box (objects.txt Left/Top/Width/Height, both table copies)
  changed nothing in game: in the new graphics the mouse is tested against the fire's 3D
  model. 0.11.53 adds a hidden copy of the fire's logs model, 40% bigger, to
  `hd/objects/env_wood/rogue_bonfire.json`. Sam: "much better". Hidden models do take clicks.

Worked (17 Nov 2026): the save slot screen and picking a slot; landing on the Campfire after a
slot pick and after leaving a game; making a hero there; clicking a hero's class to load it
(bench, main menu, Play); the Options, Delete and Exit buttons showing; noting what a hero wears.

Did not work, and what was changed:
- The town bonfire could not be clicked; it now has a click box (not yet tried again).
- Hiding a figure of the game's hero screen hides nothing (the 3D figure stays). The game's
  eight figures are now moved out of sight in the layout, and the plugin's panel draws every
  figure that belongs at the fire.
- Marking the bonfire clickable and giving it a click box was not enough. It is now an ordinary
  object instead of a picture-only effect, with a click box over the whole camp spot (not yet
  tried).
- Even as an ordinary object with every setting of a clickable one, the fire ignored the mouse.
  The town's fire is now a new object, `CampBonfire`: a copy of the stash chest's row that
  wears the bonfire's look (not yet tried). In the old graphics (G key) it shows as a chest.
- From the hero screen, "ToFrontEnd" goes nowhere; the Back button's "ToCharacterSelect" does.
- The game's character list showed for a moment on the way through; a black panel now covers it.
- Asking the main menu to load again while it is already up does nothing. Slots are now
  switched from the Campfire, which reads the camp itself.
- The save slot screen was see-through, so the game's character list showed behind it at
  launch. It is now solid black, fills the screen, and opens as the menu starts loading.
- Exit Game at the Campfire did nothing: the hero screen can't close the game. The plugin now
  goes to the main menu behind a black screen and presses its Exit (not yet tried).

- The chest copy wearing the bonfire's model could not be clicked either, so the game seems to
  test the mouse against the model. As a test the copy now wears the chest's own model, in
  the bonfire's flames (not yet tried). If that can be clicked, it gets a model that suits.
- With the fire's own row made a full copy of the chest's row and the chest's model in the
  flames, the chest showed but still could not be clicked. Two more tests are in (not yet
  tried): a second, ordinary stash chest a few steps from the fire, added to the four town
  maps; and the fire ring's scenery without its collision shape, in case that shape swallows
  the mouse.
- None of the bonfire tries ever reached the game: it reads a second copy of its tables,
  `global/excel/base`, which the build did not change. Once `tools/build.py` patched both copies
  (0.11.7), the chest in the fire could be used, though not reached inside the pit, and the
  changed town maps showed. Town maps only load under new names (`darkest_town*.ds1`, with the
  town's row in `lvlprest.txt` pointing at them).
- Now the fire's own row takes only the chest's "use" values and keeps its size (3 by 3), look and
  light, so it can be used from the edge of the pit; the town's stash chest was removed
  (0.11.9). From 0.11.29 the chest is back as it was, and the hero's distance to the fire decides.
- Changing save at the Campfire kept showing the first save's heroes: the game keeps a panel
  it has drawn and does not read its layout again. The Campfire and the save slot screen now
  use a fresh spare panel each time their content changes (not yet tried).
- Delete is now called Retire. A save can be deleted from the save slot screen, after two
  questions. Its files are not erased: they move to `camp\trash\slot<N>-<time>\` (not yet tried).

## Not yet tried in the game

Each point fails soft and writes to `darkestdiablo.log`. "campfire_screen 0" in
`camp\options.txt` brings the game's character list back if the Campfire screen misbehaves.

1. Does the Campfire open by itself after a slot is picked and after leaving a game?
2. Do the camp's heroes stand at the fire, and are the game's own figures for those classes
   hidden? Do the figures show weapon, shield and armor weight?
3. Does clicking a figure load that hero? (The name plate is the sure way.) Is it the right
   hero when the camp has two or more?
4. Do Options, Delete and Exit Game work there? Does Back show the save slots?
5. Is the bonfire clickable now, and does it open the stash with extra pages and the Campfire
   button? Does "Go to the fire" leave the game and land on the Campfire?
6. Death: is the hero buried after a hardcore death, and after a softcore one?
7. Does the game read a save's class and level where `camp.h` expects them?
