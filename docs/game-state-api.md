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

// Champions and uniques the hero fights (not town NPCs, summons or the mercenary), nearest first.
for (const Game::MonsterDetails& m : Game::Enemies(40)) {
	if (m.champion || m.unique) { /* ... */ }
}

// The town fire (objects.txt row 39), if loaded; or every unused shrine nearby.
const auto fires = Game::Objects(60, 39);
for (const Game::Unit& shrine : Game::Objects(40, Game::ObjectKind::Shrine)) {
	if (!Game::Used(shrine)) { /* ... */ }
}

// The hero's skills (skills.txt rows) and what the mouse buttons hold.
const Game::SkillSet skills = Game::HeroSkills();
// skills.right, skills.left, skills.skills[i].id / .baseLevel

// The mercenary, what the hero wears and carries, and the hero's corpse after a death.
if (const auto merc = Game::Mercenary(); merc && merc->alive) { /* merc->life, merc->unit */ }
const auto belt = Game::HeroItems(Game::ItemPlace::Belt);  // potions in the belt
for (const Game::PlayerDetails& p : Game::Players()) {
	if (p.corpse) { /* the body to walk back to */ }
}
```

React to changes by subscribing to the shared event feed. The plugin looks for changes every
250 ms (one tick for everyone) and calls each subscriber on the UI thread:

```cpp
void OnGameEvent(const Game::Event& e, void* userData) {
	switch (e.type) {
		case Game::EventType::HeroDied:     /* permanent death */ break;
		case Game::EventType::ObjectUsed:   /* e.unit.row: which object; e.after: its new mode */ break;
		case Game::EventType::HeroStateOn:  /* e.after: the states.txt row that started */ break;
		case Game::EventType::MercDied:     /* the mercenary fell */ break;
		default: break;
	}
}

const int id = Game::Subscribe(OnGameEvent);  // once, e.g. when your feature loads
Game::Unsubscribe(id);                        // when it no longer wants events
```

Numbers for the run and the current stop, kept by the API from the same feed:

```cpp
const Game::CounterSet c = Game::Counters();
// c.run.kills, c.run.killsChampion, c.run.killsUnique, c.run.killsBoss, c.run.heroDeaths,
// c.run.mercDeaths, c.run.goldGained, c.run.itemsFound, c.run.levelsGained, c.run.shrinesUsed,
// c.run.chestsOpened, c.run.seconds; the same in c.stop for the area the hero is in (c.stopArea).
Game::ResetStop();  // e.g. when an arena's first wave starts, if "stop" should mean that instead
```

## Rules for calling it

- **Any thread, any time.** Every call reads fresh values, so the result is "now". Nothing is
  cached except where the unit list lives.
- **Nothing found is normal.** At the menus, while a level loads, or on the wrong game build,
  calls return an empty `Unit` (`if (!unit)`), an empty list, `-1`, or an empty `std::optional`.
  Always check before using a value.
- **Units are snapshots.** A `Game::Unit` holds the values read when you got it. Ask again (or
  `GameState::Refresh(unit)`) for newer ones; never keep `unit.address` for later.
- **One tick, many subscribers.** Only the plugin's timer (`TrackTick` in `campfire.cpp`) calls
  `Game::Tick()`. Features use `Game::Subscribe()` and `Game::Counters()`, never a timer of
  their own that looks for changes. `Game::Poll()` still works (everything since its previous
  call) for one caller; new code should subscribe instead.
- **Subscribers are quick.** They run on the game's UI thread right after the tick. Do the work
  or hand it to `Later`; never wait or sleep in one.
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
| `Game::PlayerModeName(mode)`: idle, walk, run, cast, dead...; `Game::InCombatMode(mode)` (attacking, casting, hit, blocking) | mapped (the classic game's player modes) |

### Skills: `Game::HeroSkills()`

`SkillSet`: `skills` (every skill the hero has, each with `id` = skills.txt row and `baseLevel` =
points put in it, without items), `left` and `right` (the skills on the mouse buttons), `found`
(false when the skill list wasn't found).

| Value | Status |
| --- | --- |
| The skill list, each skill's row | mapped: found by its shape (it must hold Attack, no repeats, and the left and right skills), map tools' place (hero + 0x100) tried first |
| Left and right skill | mapped (same list) |
| Points in each skill (`baseLevel`) | mapped: the map tools' place (skill + 0x40), unchecked; `dd-state skills` prints the nearby numbers to find the right one |

### Players: `Game::Players()`

Every player unit loaded, one per player: `name`, `classId`, `level`, `hero` (the local player),
`dead`, `corpse` (a dead player unit that is not the hero: the body left after a death). Status:
mapped; checking a corpse needs a death in game.

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

### Monsters: `Game::Monsters(radius = 60, aliveOnly = true)`, `Game::Monster(unit)`, `Game::Enemies(radius = 60)`, `Game::Mercenary()`

| Value | Status |
| --- | --- |
| Every loaded monster, position, monstats.txt row | checked |
| `cls` (`Game::ClassOf(row)`): `Enemy`, `Npc` (town people, guards), `Mercenary`, `Summon` (golems, skeletons, wolves, valkyrie...), `Critter` (rats, birds, cows) | from the tables (see below) |
| `Enemies()`: live monsters of class `Enemy` only | from the tables |
| `Mercenary()`: the one hired fighter loaded (single player), alive or dead | mapped (by class; the game's owner link isn't read) |
| Alive or dead (mode 0 dying, 12 dead) | mapped |
| Champion, unique, super unique, minion, possessed, ghostly | mapped |
| Enchantments (monumod.txt rows), super unique row | mapped |
| Life, max life; any stat and state (same calls as the hero) | mapped |

### Objects: `Game::Objects(radius = 60, row = AnyRow)`

| Value | Status |
| --- | --- |
| Every loaded object, position, objects.txt row (39 town fire, 267 stash, 119 waypoint) | checked |
| Mode: 0 idle, 1 being used, 2 used/open (chests, shrines, doors); `Game::Used(unit)` | mapped |
| Kind (`Game::KindOf(row)`, `Game::Objects(radius, ObjectKind)`): `Shrine`, `Chest` (also urns, caskets, bookshelves), `Waypoint`, `Portal` (town and red portals), `Door`, `Well`, `Stash`, `Fire`, `Other` | from the tables |

**Kinds from the tables.** Monster classes and object kinds come from `monstats.txt` (`npc`,
`Align`, hireling rows) and `objects.txt` (`SubClass`, `IsDoor`). `tools/gen_game_tables.py`
turns them into `build/plugin-gen/gametables.h` on every `tools\build-plugin.cmd`; nothing from
the tables is committed. Rows the mod adds past the end of the vanilla tables count as `Enemy`
and `Other`.

### Items: `Game::GroundItems(radius = 60)`, `Game::Item(unit)`, `Game::HeroItems(place)`

| Value | Status |
| --- | --- |
| Every loaded item, row, position on the ground | mapped |
| Where it is (`ItemPlace`: stored, equipped, belt, ground, cursor, socketed) | mapped |
| Quality, owner's unit number, item level, flags (`Identified()`, `Socketed()`, `Ethereal()`) | mapped |
| `bodySlot` of a worn item (1 head ... 10 gloves, 11-12 weapon swap) | mapped (item data + 0x54) |
| `page` of a stored item (0 inventory, 3 cube, 4 stash) | mapped (item data + 0x55) |
| `HeroItems()`: every item whose owner is the hero (worn, inventory, cube, stash, belt, cursor) | mapped (needs the owner number to be right) |

For the local player's own items, the SDK's InventoryService is checked and preferred.

### What changed: events (`Game::Subscribe()`, `Game::Poll()`)

| Event | `before` / `after` | Status |
| --- | --- | --- |
| `HeroDied` | mode before / after (0 or 17) | mapped |
| `HeroHit` | life before / after | mapped |
| `HeroAreaChanged` | area before / after | mapped |
| `HeroStateOn` / `HeroStateOff` | the states.txt row (after / before) | mapped |
| `HeroHealed` | life before / after | mapped |
| `HeroLevelUp` | level before / after | mapped (the SDK's own level-up event also exists) |
| `HeroGoldChanged` | gold carried before / after | mapped |
| `SkillChanged` | right mouse skill before / after (skills.txt rows) | mapped |
| `MonsterDied` | mode before / after | mapped (the SDK's own monster-death event also exists) |
| `MercDied` | mode before / after; sent instead of `MonsterDied` for the mercenary | mapped |
| `ObjectUsed` | mode before / after (chest opened, shrine taken, door opened) | mapped |
| `UnitAppeared` / `UnitGone` | a monster, object or item came into or left the loaded area | mapped |

The first tick only takes a snapshot. The town fire and stash never change mode, so the fire
click still goes by distance (`docs/campfire.md`).

### The event feed: `Game::Subscribe(callback, userData)`, `Game::Unsubscribe(id)`

Every event above goes to every subscriber, in order, once. The tick runs every 250 ms from
the moment the plugin loads (at the menus it finds no hero and does nothing). A monster that
dies and disappears within 250 ms can be missed; in practice the dying animation and the
corpse last much longer. Status: mapped (built on the events above).

### Run counters: `Game::Counters()`, `Game::ResetCounters()`, `Game::ResetStop()`

`Counters()` returns a `CounterSet`: `run` (since the run started), `stop` (since the hero
entered the current area) and `stopArea` (that area's levels.txt Id). Each is a `Tally`:

| Counter | How it is counted | Status |
| --- | --- | --- |
| `kills` | `MonsterDied` of a monster of class `Enemy` (not NPCs, summons, critters, the mercenary) | mapped |
| `killsNormal`, `killsChampion`, `killsUnique`, `killsBoss` | the dead monster's kind; boss = monstats.txt `boss` rows (act bosses, Blood Raven, Griswold, the Summoner, Radament...), which wins over unique | mapped |
| `heroDeaths`, `mercDeaths` | `HeroDied`, `MercDied` | mapped |
| `goldGained` | each rise of gold carried + stash gold (picking up, selling, quest gold). Moving gold to the stash doesn't count; spending doesn't subtract | mapped |
| `itemsFound` | items that newly belong to the hero (`HeroItems()`): picked up, bought, rewards. What the hero owns when the run starts doesn't count | mapped (needs the item owner number) |
| `levelsGained` | `HeroLevelUp` | mapped |
| `shrinesUsed`, `chestsOpened` | `ObjectUsed` from idle (mode 0) of a `Shrine` or `Chest` kind object, once per object | mapped |
| `seconds` | real time since the reset; the game's pause menu still counts | checked by the clock |

The run starts at `ResetCounters()`, which the Campfire calls when a game with a hero starts
(`GameJoined`). The stop starts again by itself whenever the hero enters another area (going to
town and back starts a new stop too), or at `ResetStop()`. Nothing is saved: the counters start
from zero with each game, so a run that spans several games needs its own saved total.

### Planned (needs finding in the game's code, then checking)

- **The game's current frame** (time left on a state), **the unit under the mouse**, **open
  panels**, **map seed**, **level exits and preset objects**, **the mercenary's owner link**
  (today the mercenary is found by its monster class), **where a stored item sits in the grid**.
- **Missiles** are not in the unit list's missile block (always 0 while casting). Not needed for
  now (Sam, 2026-11-19); `dd-state tables` is the research tool if they are.

## Changing values

Both of us agreed (2026-11-19) to let the API change values by writing game memory (CLAUDE.md).
All writes go through `src/plugin/gamewrite.h`, the only file that writes:

```cpp
#include "gamewrite.h"

const Game::Unit hero = Game::Hero();
const auto copies = GameWrite::Copies(hero);              // slow (1-2 s): once per game, keep it
GameWrite::SetStat(copies, Game::StatId::MagicFind, 300); // only if the hero has magic find now
GameWrite::SetStateStat(copies, 60, Game::StatId::VelocityPercent, -20);  // a weaker slow
GameWrite::AddStat(copies, Game::StatId::Life, Game::StatId::MaxLife, 256, 0.0);  // +1 life, never past the maximum
GameWrite::SetStateEnd(copies, 60, endFrame);              // a shorter or longer state
```

What it can and can't do (status: mapped, being tested with `dd-set`):

- **Only existing values.** A write changes a stat the unit has now. It can't give a stat the
  unit doesn't have (magic find with no magic find items, a speed bonus with no speed source),
  because that would need the game's own memory allocator. For those, give the stat through our
  own state or a hidden item first (tables), then tune its value with a write.
- **Every copy.** The game keeps more than one copy of the hero: what the screen draws (what the
  API reads) and the game rules' own copy. `Copies()` finds them all; a write to only the drawn
  copy may show on screen but change nothing, or be undone.
- **Writes don't stick.** The game recalculates totals when items, skills or states change, so a
  feature that wants a value to stay writes it again on a timer (`dd-set hold`).
- **Safety.** Writes go only to plain, writable data memory, never code, and only on the frozen
  build. A write that misses does nothing, but a write to the right place with a silly value can
  still break the game until restart; keep values in the game's normal ranges.

Test in game: `dd-set copies` (lists the hero's copies with life and position, to tell them
apart), `dd-set stat <id> <value>`, `dd-set hold <id> <value>` / `dd-set hold off`,
`dd-set state <row> end <frame>`, `dd-set state <row> stat <id> <value>`. Log lines start "Write:".

## Checking it in game

Open the console in game:

- `dd-state`: the hero (with mode name and left/right skill), world, resistances, magic find,
  speeds, the nearest monsters (class, kind, life, enchantments), objects (kind, row, mode) and
  items on the ground.
- `dd-state stats`: every hero stat as `id=value` (`id/param=value` for stats with a param).
- `dd-state states`: the hero's buffs and debuffs, plus raw research lines.
- `dd-state skills`: the hero's skills as `row/points`, the left and right skill, and the raw
  numbers of each skill (to find where the points live if `baseLevel` is wrong).
- `dd-state items`: every item the hero owns, with place, slot, page, quality and item level.
- `dd-state players`: every player (the hero, a corpse) and the mercenary.
- `dd-state counters`: the run and stop counters; `dd-state counters reset` starts them again.
- `dd-state watch`: subscribes and logs each event as "State:" lines; type it again to stop.
- `dd-state tables`: research for where missiles live.

Everything is also written to the plugin log (`Mods\darkestdiablo\d2rloader\logs\darkestdiablo.log`).
That log is rewritten every time the game starts (also when it restarts by itself), so every
`State:` and `Write:` line (dd-state, dd-state watch, dd-set) also goes to
`Mods\darkestdiablo\d2rloader\logs\darkestdiablo-state.log`, which is only ever added to: each line
has the local date and time, and each game start adds a `--- session start <version> <time>`
line. Over 5 MB at start it becomes `darkestdiablo-state.old.log`. Read results from there.
When a mapped value proves right, mark it checked here; when it's wrong, fix the offset in
`gameapi.cpp` and note what the game showed.

### One play session to check the mapped values

The results stay in `darkestdiablo-state.log`. Type `dd-state watch`
first so every event is logged, then:

| Do this | Then type | What should match |
| --- | --- | --- |
| Stand in town | `dd-state` | hero line: mana, gold, stash gold, exp as on the character screen; `town idle`; world: act 0, your difficulty; objects: `stash`, `waypoint`, `fire` kinds |
| Open the character screen | `dd-state stats` | strength, dexterity, resistances, defense, magic find as the screen shows them |
| Open the skill tree | `dd-state skills` | each skill's points (second number); left and right skill match the mouse buttons. If points are -1 or wrong, send the raw lines |
| Wear, carry, stash and belt a few items | `dd-state items` | slot of each worn item, page 0 / 3 / 4 for inventory / cube / stash, quality, ethereal, unidentified |
| Hire a mercenary, walk near it | `dd-state players` | `Mercenary:` line with its life; the hero line says `the hero` |
| Cast a buff (or get cursed / chilled) | `dd-state states` | the state appears; `State: hero state on` / `off` in the log |
| Walk into a monster area | `dd-state` | monsters say `enemy`; champions / uniques marked; NPCs in town say `npc` |
| Kill monsters, a champion, a unique | `dd-state counters` | kills by kind; `State: monster died` lines |
| Open chests, take a shrine, pick up gold and items | `dd-state counters` | chests, shrines, gold +, items found; `object used` lines |
| Get hit, drink a potion, level up | (log) | `hero hit`, `hero healed`, `hero level up` lines; levels + in counters |
| Change the right skill | (log) | `right skill changed` with the skills.txt rows |
| Die (or let the mercenary die) | `dd-state players`, `dd-state counters` | a `corpse` player line; `hero died` / `mercenary died`; deaths in counters |
| Go to another area | `dd-state counters` | the stop counters start again with the new area Id |

Mark each value that matched as checked in the tables above.

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
