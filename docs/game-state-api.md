# The game-state API

The plugin's one way to know what is happening in the running game: the hero, the world,
monsters, objects, items, buffs and debuffs, and what just changed. Any feature in `src/plugin/`
that needs game information uses this API instead of reading the game itself.

- Code: `src/plugin/gameapi.h` (what you call), built on `src/plugin/gamestate.h` (the live unit
  list). How the unit list is found and every address used: `docs/game-memory.md`.
- Read only. Nothing in the API writes to the game or changes its code (CLAUDE.md). It works on
  the frozen build in `D2R_VERSION` only, and returns "nothing" anywhere else.
- **Keep this page up to date.** Any change to `gameapi.h` or `gamestate.h` updates this page in
  the same pull request: new calls, a value's status (planned, mapped, checked), and lessons.

## Quick start

```cpp
#include "gameapi.h"

// Who is the hero, and how are they doing?
const Game::HeroDetails hero = Game::HeroInfo();
if (hero.unit && !hero.dead && hero.life * 4 < hero.maxLife) {
	// below a quarter of their life
}

// Any stat, with items and skills added (what the character screen shows).
const int magicFind = Game::Stat(hero.unit, Game::StatId::MagicFind).value_or(0);
const int fireResist = Game::Stat(hero.unit, Game::StatId::FireResist).value_or(0);

// Buffs and debuffs on the hero.
for (const Game::StateDetails& s : Game::States(hero.unit)) {
	// s.state = states.txt row, s.stats = what it adds, s.activeSeconds = how long it's been on
}
const bool frozen = Game::HasState(hero.unit, 1);  // states.txt row 1 = "freeze"

// Champions and uniques near the hero, nearest first.
for (const Game::MonsterDetails& m : Game::Monsters(40)) {
	if (m.champion || m.unique) { /* ... */ }
}

// The town fire (objects.txt row 39), if loaded.
const auto fires = Game::Objects(60, 39);
```

React to changes from one place, on a timer (the plugin's `Later` helper in `campfire.cpp`):

```cpp
for (const Game::Event& e : Game::Poll()) {
	switch (e.type) {
		case Game::EventType::HeroDied:     /* permanent death */ break;
		case Game::EventType::ObjectUsed:   /* e.unit.row: which object; e.after: its new mode */ break;
		case Game::EventType::HeroStateOn:  /* e.after: the states.txt row that started */ break;
		default: break;
	}
}
```

## Rules for calling it

- **Any thread, any time.** Every call reads fresh values, so the result is "now". Nothing is
  cached except where the unit list lives.
- **Nothing found is normal.** At the menus, while a level loads, or on the wrong game build,
  calls return an empty `Unit` (`if (!unit)`), an empty list, `-1`, or an empty `std::optional`.
  Always check before using a value.
- **Units are snapshots.** A `Game::Unit` holds the values read when you got it. Ask again (or
  `GameState::Refresh(unit)`) for newer ones; never keep `unit.address` for later.
- **One poller.** `Game::Poll()` compares with its previous call, so only one place may call it.
  Today that is the `dd-state watch` tick. A feature that needs events should share that tick
  (or become it), not start a second one.
- **Cost.** A call walks the loaded units (a few hundred) and takes well under a millisecond.
  Fine every 100-250 ms; avoid calling it every frame from many places.
- **Positions** are world sub-tiles, the same numbers `location player` prints. `fx`, `fy` add
  the fraction of a sub-tile. Radius arguments are in sub-tiles from the hero.
- **Rows** are table rows: monsters are `monstats.txt`, objects `objects.txt`, states
  `states.txt`, skills `skills.txt`, stats `itemstatcost.txt`, areas `levels.txt` Id.

## Status of each value

**checked** = compared with the game; **mapped** = from the community's maps of D2R (MapAssist,
D2RMH) or the classic game's layout, printed by `dd-state` for checking; **planned** = not found
yet. Use mapped values, but check them in game before a feature depends on them, then mark them
checked here.

### The hero: `Game::Hero()`, `Game::HeroInfo()`

| Value | Status |
| --- | --- |
| Unit, position (with fractions) | checked |
| Name, class | checked |
| Mode (5 standing in town; 0 dying, 17 dead) | checked (town) |
| Area (levels.txt Id) | checked (town: 1) |
| Level, life, max life | checked (0.11.54) |
| Mana, experience, gold, stash gold | mapped (same stat list as life) |

`Game::SetLocalPlayerId(id)` (from the SDK's `GameplayEvent::playerId`) makes `Hero()` exact in
multiplayer-style games; `campfire.cpp` already passes it on.

### Stats: `Game::Stat(unit, id, param = 0, withItems = true)`

Any `itemstatcost.txt` row, for the hero or a monster. `withItems = false` gives the unit's own
value without items, skills and auras. Life, mana and stamina come back whole (the game keeps them
in 256ths). Named rows are in `Game::StatId`: attributes, life/mana/stamina, level, experience,
gold, defense, damage reduction, every resistance and its maximum, gold find, magic find, faster
attack/cast/run-walk/hit recovery/block, and `VelocityPercent` (speed changes from skills and
states such as Slowed). Status: mapped. `GameState::Stats(unit, withItems)` lists them all.

### Buffs and debuffs: `Game::States(unit)`, `Game::HasState(unit, row)`

Each `StateDetails`: `state` (states.txt row), `skill` and `skillLevel` that gave it, `endFrame`
(the game frame it ends on, 25 frames a second; 0 for auras and permanent states), `stats` (what
it adds, e.g. `velocitypercent -40`), `activeSeconds` (how long the API has seen it on).
Status: mapped, from the classic game's stat list layout. Time left needs the game's current
frame, which is planned; until then use `activeSeconds` against the state's known length.

### The world: `Game::World()`

| Value | Status |
| --- | --- |
| Area | checked |
| Act, difficulty | mapped |
| Map seed, game name | planned |

### Monsters: `Game::Monsters(radius = 60, aliveOnly = true)`, `Game::Monster(unit)`

| Value | Status |
| --- | --- |
| Every loaded monster, position, monstats.txt row | checked |
| Alive or dead (mode 0 dying, 12 dead) | mapped |
| Champion, unique, super unique, minion, possessed, ghostly | mapped |
| Enchantments (monumod.txt rows), super unique row | mapped |
| Life, max life; any stat and state (same calls as the hero) | mapped |

### Objects: `Game::Objects(radius = 60, row = AnyRow)`

| Value | Status |
| --- | --- |
| Every loaded object, position, objects.txt row (39 town fire, 267 stash, 119 waypoint) | checked |
| Mode: 0 idle, 1 being used, 2 used/open (chests, shrines, doors) | mapped |

### Items: `Game::GroundItems(radius = 60)`, `Game::Item(unit)`

| Value | Status |
| --- | --- |
| Every loaded item, row, position on the ground | mapped |
| Where it is (`ItemPlace`: stored, equipped, belt, ground, cursor, socketed) | mapped |
| Quality, owner's unit number, item level, flags (identified, socketed, ethereal) | mapped |

For the local player's own items, the SDK's InventoryService is checked and preferred.

### What changed: `Game::Poll()`

| Event | `before` / `after` | Status |
| --- | --- | --- |
| `HeroDied` | mode before / after (0 or 17) | mapped |
| `HeroHit` | life before / after | mapped |
| `HeroAreaChanged` | area before / after | mapped |
| `HeroStateOn` / `HeroStateOff` | the states.txt row (after / before) | mapped |
| `MonsterDied` | mode before / after | mapped (the SDK's own monster-death event also exists) |
| `ObjectUsed` | mode before / after (chest opened, shrine taken, door opened) | mapped |
| `UnitAppeared` / `UnitGone` | a monster, object or item came into or left the loaded area | mapped |

The first call only takes a snapshot. The town fire and stash never change mode, so the fire
click still goes by distance (`docs/campfire.md`).

### Planned (needs finding in the game's code, then checking)

- **Changing values** (movement speed, buffs, debuffs, magic find): Sam asked for this
  (2026-11-19). It needs writing to game memory, which CLAUDE.md forbids until both of us agree;
  it is being built on its own branch with the rule change for Fabrizio to approve. Until then,
  change these through tables (our own states and skills, like the dodge slow in
  `src/patches/050_dodge_roll.py`) and SDK services.
- **The game's current frame** (time left on a state), **the unit under the mouse**, **open
  panels**, **map seed**, **level exits and preset objects**, **the mercenary**, **skills** (left
  and right skill, skill levels).
- **Missiles** are not in the unit list's missile block (always 0 while casting). Not needed for
  now (Sam, 2026-11-19); `dd-state tables` is the research tool if they are.

## Checking it in game

Open the console in game:

- `dd-state`: the hero, world, resistances, magic find, speeds, the nearest monsters (kind, life,
  enchantments), objects (row, mode) and items on the ground.
- `dd-state stats`: every hero stat as `id=value` (`id/param=value` for stats with a param).
- `dd-state states`: the hero's buffs and debuffs, plus raw research lines.
- `dd-state watch`: logs each `Game::Poll()` change as "State:" lines; type it again to stop.
- `dd-state tables`: research for where missiles live.

Everything is also written to the plugin log (`Mods\darkestdiablo\d2rloader\logs\darkestdiablo.log`).
When a mapped value proves right, mark it checked here; when it's wrong, fix the offset in
`gameapi.cpp` and note what the game showed.

## Adding to the API

1. Find the value: the community maps (MapAssist, D2RMH) or the classic game's structures give a
   starting point; our build may differ (stats are 16 bytes here, not 8).
2. Add it to `gameapi.h` with a comment that says its units and what "unknown" returns, and read
   it in `gameapi.cpp` with `GameState::ReadBytes` (fails safely on a bad address). Put each
   offset in a named constant at the top of `gameapi.cpp`.
3. Print it from `dd-state` so it can be checked in game.
4. List it on this page as mapped; mark it checked once compared with the game, and add any new
   address to `docs/game-memory.md`.

## How the bot and map tools compare

The D2 bot frameworks (D2BS / kolbot) give scripts `me` (the hero: position, area, life, mana,
stats, gold, mode), `getUnit(type, name or row, mode)` to find any unit, `unit.getStat(id)`,
`getState`, items with their location and quality, the room and preset data of each level,
open-panel flags and the hovered unit. The D2R map tools (MapAssist, D2RMH) read the same unit
list as we do, plus the map seed, the panel flags and the game's name. The API covers their
unit, stat, state and world parts; the rest is the planned list above.
