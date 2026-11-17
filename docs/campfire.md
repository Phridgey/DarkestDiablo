# The Campfire

The start of every session: pick a save, see the camp, pick a hero, play. This page says what
the player sees, where each part lives, what the game engine forced, and what still has to be
checked in game. The rules themselves are in `src/plugin/camp.h` and are tested without the game
(`tests/camp_tests.cpp`).

## What the player sees

1. **Save slots.** After the title screen a window asks for Save 1, 2 or 3, each with a line
   saying who is by its fire.
2. **The Campfire.** The game's character list, staged at the campfire: the save's living heroes
   are listed, and the selected one stands by the fire in the gear it really wears. Play enters
   the game with that hero.
3. **Recruit.** Opens the game's hero screen (the eight classes around the fire), showing only
   the classes that may join right now:
   - a brand new save: every class. The one picked starts the camp; the rest lock.
   - later: only classes a traveler brought (see below). They start at level 1 with nothing.
   - every hero dead and nobody waiting: a traveler turns up by himself with up to 3 classes.
4. **In town.** Each time a hero comes back to town from a stop there is a chance (25%) that
   "A traveler joins your camp" and offers up to 3 locked classes. The class of the last hero
   that fell is always one of them, once. The picked class waits at the Campfire as a recruit.
5. **The bonfire.** Clicking the Rogue Encampment's bonfire opens the camp stash. The stash
   window has a Campfire button: it shows who is by the fire and has "Go to the fire", which
   saves, leaves the game and lands on the Campfire to pick another hero.
6. **Death is final.** A fallen hero's files move to the graveyard, with everything it wore and
   carried. Its class locks again. Only the stash carries on.
7. **The stash.** The game's shared stash, with 7 more pages. Each save has its own.

## Choices the engine forced

- **One save folder, one shared stash.** The game reads every character and one shared stash
  from a single folder. So a slot is a set of files: the live folder holds the loaded slot, the
  other slots are parked in `camp/slot<N>/files/`, and picking a slot moves files both ways.
  Settings and loot filters stay put and are shared by all slots.
- **One hero in real gear at a time.** The hero screen (eight classes around the fire) draws
  fixed models from its layout file; the game gives a plugin no way to dress them from a save.
  The character list is the only screen that draws a save's real gear, and it draws the selected
  character only. So the Campfire is the character list moved to the campfire, not the hero
  screen with eight live heroes.
- **Switching hero needs a load.** A game session holds one character. Changing hero means
  leaving the game and loading the other save, so there is a loading screen each time. "Go to
  the fire" makes that two clicks, but it can't be instant.
- **The bonfire can't call the plugin.** The plugin SDK reports UI messages, not clicks on
  world objects. The bonfire therefore opens the one town window we can add a button to, the
  stash, and the Campfire button is there. That also means the stash chest shows the button.
- **Text on our panels is fixed.** A plugin can show and hide widgets but not change their
  text. The traveler and roster panels hold a ready-made button for every case and show the
  right ones. The roster shows classes, not hero names and levels. The slot screen's summary
  lines are made once at launch.
- **No "player died" event.** The plugin watches for the game's "You died" window and for a
  resurrection. A softcore hero that comes back is sent out of the game at once and buried.
  Hardcore heroes are also caught by the dead mark in their save.

## Where things live

| Path | What |
| --- | --- |
| `src/plugin/camp.h` | The rules: classes, offers, the camp file, slot switching, burying. No game code. |
| `src/plugin/campfire.cpp` | Talks to the game: UI messages, panels, game events, console commands. |
| `tools/camp_layouts.py` | Our three panels (save slots, traveler, roster), written into the mod by the build. |
| `src/patches/030_campfire.py` | Bonfire, stash button, the character list staged at the campfire, hero screen title. |
| `src/d2rloader/` | The mod's D2RLoader settings: `add_shared_tabs = 7`. |

In the save folder (`Saved Games\Diablo II Resurrected\mods\darkestdiablo`):

    camp\active.txt              the loaded slot
    camp\slot<N>\camp.txt        that slot's camp: heroes, recruits, fallen classes, the traveler's offer
    camp\slot<N>\files\          that slot's saves and stash while another slot is loaded
    camp\graveyard\slot<N>\      fallen heroes
    camp\options.txt             optional: "slot_picker 0", "traveler_chance 50"

The camp follows the save files: each time the menu loads, new saves join, deleted saves leave,
dead ones are buried. Back up the whole folder to back up everything.

Console: `dd-camp` (who is in the camp; in town it opens the roster), `dd-slot <1-3>` (main
menu), `dd-traveler` (in town: a traveler now).

## Building

The build changes four of the game's menu layouts, so `vanilla/` needs them: extract
`data/global/ui/layouts` the same way as the tables. Nothing of Blizzard's is in the repo.

## Not yet tried in the game

Everything here builds and the rules are tested, but none of it has run in the game yet. Each
point fails soft and writes to `darkestdiablo.log`:

1. Does the slot window open over the main menu, and does the menu load the new slot's heroes
   after a switch? If the menu doesn't reload, the window says to restart the game.
2. Does the character list look right at the campfire (camera, where the hero stands)?
3. Are the classes that can't join hidden on the hero screen? Their clicks are refused either way.
4. Is the bonfire clickable, and does it open the stash?
5. Do the extra stash pages show up? (`add_shared_tabs`, and whether `metadata.json` is needed.)
6. Does "Go to the fire" leave the game?
7. Death: is the hero buried after a hardcore death, and after a softcore one?
8. Does the game read a save's class and level where `camp.h` expects them? If not, the camp
   still works from what the plugin saw; only levels shown on the slot screen would be off.
