# The game-state API

What the mod can know about the running game, at any moment, from any thread: the hero, the
world, monsters, objects, items, and what just changed. Code: `src/plugin/gameapi.h` (the API)
on top of `src/plugin/gamestate.h` (the live unit list, see `docs/game-memory.md` for how it is
found). Read only and frozen build only, like every game read (CLAUDE.md).

The D2RLoader SDK already gives some of this (events for joining, leaving, act, level, level-up,
quests and monster deaths; the local player's items). The API fills the rest. Use the SDK where
it has the answer; use the API for everything else.

## What it offers

Status: **checked** = compared with the game; **mapped** = from the community's maps of D2R
(MapAssist, D2RMH), printed by `dd-state` for checking; **planned** = needs finding first.

### The hero (`Game::Hero()`, `Game::HeroInfo()`)

| Value | Status |
| --- | --- |
| Unit, position (sub-tiles, with fractions) | checked |
| Name, class | checked |
| Mode (5 standing in town; 0 dying, 17 dead) | checked (town) |
| Area (levels.txt Id) | checked (town: 1) |
| Level, life, max life | checked (0.11.54) |
| Mana, experience, gold, stash gold | mapped (same stat list as life) |
| Any stat: `Game::Stat(unit, id)`, with or without items | mapped; ids in `Game::StatId` |
| Resistances, magic find, faster cast/attack/run/hit recovery | mapped |

### The world (`Game::World()`)

| Value | Status |
| --- | --- |
| Area | checked |
| Act, difficulty | mapped |
| Map seed, game name | planned |

### Monsters (`Game::Monsters()`, `Game::Monster(unit)`)

| Value | Status |
| --- | --- |
| Every loaded monster, position, monstats.txt row | checked |
| Alive or dead (mode 0 dying, 12 dead) | mapped |
| Champion, unique, super unique, minion, possessed, ghostly | mapped |
| Enchantments (monumod.txt rows), super unique row | mapped |
| Life, max life | mapped (same stats as the hero's) |

### Objects (`Game::Objects()`)

| Value | Status |
| --- | --- |
| Every loaded object, position, objects.txt row (39 town fire, 267 stash, 119 waypoint) | checked |
| Mode: 0 idle, 1 being used, 2 used/open (chests, shrines, doors) | mapped |

### Items (`Game::GroundItems()`, `Game::Item(unit)`)

| Value | Status |
| --- | --- |
| Every loaded item, row, position on the ground | mapped |
| Where it is (stored, equipped, belt, ground, cursor, socketed): item mode | mapped |
| Quality, owner's unit number, item level, flags (identified, socketed, ethereal) | mapped |

### Missiles

The list's missile block was always empty in game (0.11.63 log: 0 missiles while casting), so
missiles live somewhere else. `dd-state tables` lists every unit-list-shaped place in the game's
code with its unit counts, to find them. **planned**

### What changed (`Game::Poll()`)

Call it regularly from one place (every 100-250 ms). It compares with the last call and reports:

| Event | How it is seen | Status |
| --- | --- | --- |
| Hero died | hero's mode turns to 0 or 17 | mapped |
| Hero hit | hero's life went down | mapped |
| Hero changed area | area Id differs | mapped |
| Monster died | monster's mode turns to 0 or 12 | mapped (the SDK's own monster-death event also exists) |
| Object used | object's mode changes (chest opened, shrine taken, door opened) | mapped |
| Unit appeared / gone | a monster, object or item came into or left the loaded area | mapped |

"Hero died" is the event permanent death needs; the SDK has none. "Object used" may tell when
the hero uses a chest or shrine; the town fire and stash never change mode, so the fire still
goes by distance (`docs/campfire.md`).

### Planned (needs finding in the game's code, then checking)

- **The unit under the mouse.** The map tools read the game's "hovered unit". With it the fire
  click needs no distance check, and any object could be made clickable for the mod.
- **Open panels.** Inventory, stash, waypoint, character screen open or closed (MapAssist reads
  a flag block).
- **Missiles** (above), **map seed**, **level exits and preset objects** (where the stairs and
  waypoint are before walking there), **the mercenary** (a monster unit owned by the hero),
  **skills** (the hero's left and right skill, skill levels), **states** (curses, auras).

## How the bot and map tools compare

The D2 bot frameworks (D2BS / kolbot) give scripts `me` (the hero: position, area, life, mana,
stats, gold, mode), `getUnit(type, name or row, mode)` to find any unit, `unit.getStat(id)`,
`getState`, items with their location and quality, the room and preset data of each level,
open-panel flags and the hovered unit. The D2R map tools (MapAssist, D2RMH) read the same unit
list as we do, plus the map seed, the panel flags and the game's name. The API covers their
unit, stat and world parts; the rest is the planned list above.

## Checking it in game

`dd-state` prints the hero, the world, the nearest monsters (with kind, life, enchantments),
objects (row, mode) and items on the ground (row, quality, item level). `dd-state stats` prints
every hero stat. `dd-state watch` logs each change from `Game::Poll()` ("State:" lines) until
typed again. `dd-state tables` is the missile research. All of it also goes to the plugin log.
