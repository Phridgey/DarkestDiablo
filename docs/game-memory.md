# Game values the plugin reads

The D2RLoader SDK has no way to ask where the hero or a town object stands. CLAUDE.md allows the
plugin to READ a few such values from the game's memory, on the frozen build only. This page lists
every value read and where it sits. Nothing here writes to the game or changes its code.

Code: `src/plugin/gamestate.h`/`.cpp` (the live reader) and `gamememory.h`/`.cpp` (hero, fire and
stash for the Campfire, built on it).

## Safety

- Only on D2R.exe build 95032 (`D2R_VERSION` 3.4.95032). The game runs inside D2RLoader.exe, so the
  plugin reads the file version of the D2R.exe file in the game folder (found next to the
  process or up from the plugin's own folder),
  once. Its numbers are 16 bits each, so 95032 is stored as 3.4.29496.0 (95032 - 65536). On any
  other build it reads nothing and logs "reading is off".
- Reads use Windows' `ReadProcessMemory` on the game's own process. A bad address makes the read
  fail (it returns false) instead of crashing the game.
- The game's code is read (to find the unit list), never changed. Nothing is ever written.

## The live game-state reader (`gamestate.h`)

Since 0.11.51 the plugin reads the game's state live: every player, monster, object (the town
fire, the stash chest, shrines, doors, chests...), missile, item and map tile the game has loaded,
with its kind, its row in the game's tables, its number, what it is doing and where it stands.
Any feature can ask for it at any moment (`GameState::Units`, `Nearest`, `Player`), for example
`GameState::Nearest(GameState::Kind::Object, 39, x, y)` is the town fire nearest to x, y.

It all comes from one place, the game's unit list: for each kind of unit (0 players, 1 monsters,
2 objects, 3 missiles, 4 items, 5 tiles) 128 chains, picked by the unit's number mod 128. Each
entry points at a unit, and each unit points at the next one in its chain.

### Finding the unit list

The list sits at a fixed place in the game, but that place differs between game and loader
versions, so it is looked up once per game start (a few milliseconds):

1. **From the game's own code.** The code that looks a unit up reads "list start + slot * 8",
   and that instruction holds the list's distance from the game module's start. The plugin reads
   the game's code (never changing it), collects every address such instructions use, and keeps
   the one whose contents look like a unit list: every unit in its chains is of its chain's kind,
   and a named player stands somewhere. This is how the community's map tools (MapAssist,
   D2RMH) find it; they look for the exact bytes `48 03 C7 49 8B 8C C6`, which the plugin prefers
   when several addresses fit. It works on any PC without a first slow search.
2. **The saved place.** The place found is saved in `darkestdiablo-unitlist.txt` next to the
   plugin, and the place found on our PCs (`D2RLoader.exe` + 0x2a23910, next unit at 0x150) is
   built in. Both are only tried when the code gives nothing.
3. **The old slow search** (below) still runs if both fail, and hands what it finds over.

The plugin log's "Camp:" line after each town visit says which way was used. In the console,
`dd-units` shows what the reader sees (see "Checking it in game").

## The slow search (the fallback)

Before 0.11.51 this was the only way. It searches the game's memory on a background thread:

1. It looks for the hero's name. The name is the first thing in the hero's player data.
2. It looks for units that point to that player data. A unit keeps it 0x10 bytes from its start.
3. In the same search it collects object units of the town fire (objects.txt row 39,
   RogueBonfire) and of the town's stash chest (row 267, bank).
4. It looks in the game's writable memory for the hero's address to find the unit list.

It takes about 2 seconds and reads about 2.7 GB.

## Unit layout used

Taken from the community's long-standing map of the D2R unit structure and checked in game.

| Value | Where | Size |
| --- | --- | --- |
| Unit kind (0 player, 1 monster, 2 object) | unit + 0x00 | 4 bytes |
| Class (players) or objects.txt row (objects) | unit + 0x04 | 4 bytes |
| Unit number (picks its chain in the list) | unit + 0x08 | 4 bytes |
| Player data (the name comes first) | unit + 0x10 | pointer |
| Path (where the unit stands) | unit + 0x38 | pointer |
| Hero X, Y in sub-tiles | path + 0x02, path + 0x06 | 2 bytes each |
| Object X, Y in sub-tiles | path + 0x10, path + 0x14 | 4 bytes each |
| Next unit in the same chain | unit + 0x150 (or 0x158; the search checks which) | pointer |

| Mode (what it is doing; monsters 0 dying, 12 dead) | unit + 0x0C | 4 bytes |
| Stat list (life) | unit + 0x88 | pointer |
| Room the unit is in | path + 0x20 (players, monsters), path + 0x00 (objects) | pointer |
| Level Id (levels.txt) | room + 0x18 -> + 0x90 -> + 0x1F8 | 4 bytes |
| Life, character level | stat list: the first (pointer, count) pair in its first 0x100 bytes whose stats hold life (stat 6) and maximum life (stat 7); life in 256ths, level is stat 12 | 16 bytes per stat |

Checked in town with 0.11.51: the list was found from the game's code in 141 ms (D2RLoader.exe +
0x2a23910, next at 0x150), mode (5, standing in town), level (the area: 1, Rogue Encampment) and the object
rows (39 fire, 267 stash chest, 119 waypoint) were right. Life and character level read -1:
the hero's stat list holds two (pointer, count) pairs, at + 0x30 (16 stats) and + 0xA8 (34 stats),
and each stat there is 16 bytes, not the map tools' 8: the param in bytes 0-3, the stat number in
bytes 4-7, the value in bytes 8-11 (the 0.11.53 log: stat 6, life, 0x4900 = 73). 0.11.54 reads that
layout. `dd-units` logs every stat entry raw ("Units: hero stats").

Positions are world sub-tiles, the same numbers `location player` prints in the console.

### The hero's magic find

Stat 80 (item_magicbonus) from the hero's stats with items counted (`GameState::Stats`, the
list + 0xA8 array above), read by `GameMemory::HeroMagicFind`.

## The game-state API's reads (`gameapi.cpp`)

The API (`docs/game-state-api.md`) reads more values on top of the unit layout above. Each one is
a named constant at the top of `gameapi.cpp`; all are mapped (from the map tools), not yet checked:

| Value | Where | Size |
| --- | --- | --- |
| Act, difficulty | unit + 0x20 -> act + 0x28; act + 0x78 -> + 0x830 | 4 bytes, 1 byte |
| Monster kind flags, enchantments, super unique row | unit data + 0x1A, + 0x20 (9 bytes), + 0x2A | 1, 9, 2 bytes |
| Item quality, owner's unit number, flags, item level | item data + 0x00, + 0x0C, + 0x18, + 0x3C | 4 bytes each |
| Item body slot (worn), page (stored) | item data + 0x54, + 0x55 | 1 byte each |
| Skill list: { first, left, right } skill | unit + 0x100 tried first, else the first pointer in the unit's first 0x200 bytes that holds a skill list (Attack in it, no repeats, left and right in it) | pointers |
| The skill keys, also written (throwing a potion, `GameWrite::HoldSkill`) | found by shape: 32 records of 28 bytes as in the hero's .ctl file (skill, item, hand, 4, 255, 0, 0) | 896 bytes |
| The right-button skill, also written (put back after a throw) | skill list + 0x10, in every copy of the hero | pointer |
| A skill's skills.txt row; next skill; points | skill + 0x00 -> record + 0x00; skill + 0x08; skill + 0x40 | 2 bytes; pointer; 4 bytes |

## What uses them

- Fire or stash: both open the stash window, so the plugin compares the hero's distance to the
  fire and to the stash chest; the nearer one was used (`AtFire` in `campfire.cpp`).
- Magic find: the Mausoleum's rune chance. Each wave spawns the monster copies whose loot
  tables match the hero's magic find (`StartNextWave` in `darkestdiablo_plugin.cpp`,
  `src/patches/060_mausoleum.py`). If it can't be read, the hero counts as having none.
- The traveler's portal: it opens on the far side of the fire from the hero (`PlacePortal` in
  `campfire.cpp`). If either position can't be read, it opens where the hero stands.

## Checking it in game

Type `dd-units` in the console: it says how the unit list was found, how many units of each kind
are loaded, the hero (with its level and life), the nearest town fire, and the 10 nearest
objects. `dd-units monsters`, `dd-units objects 39` (one row only), `dd-units items` etc. list
one kind, nearest first. `dd-units find` looks for the unit list again. All of it also goes to
the plugin log ("Units:" lines).

Type `dd-where` in the console: it prints the hero and fire positions, the hero's magic find
(compare it with the character screen) and the last search's report. Compare the hero's
position with `location player`. `dd-where scan` searches again. `dd-state stats` lists every
hero stat.
The plugin log has a "memory scan" line after each search.

## Mercenary full gear: the one code change (2026-11-23)

Agreed by both of us as the only exception to "never change game code". `src/plugin/mercgear.cpp`.

- What: the game's two copies of "may a mercenary wear this" (the server's, also used when a save
  loads, and the panel's) test whether an item is body armor (`mov edx,3`, item type 3). Each `3`
  becomes the number of our "merc" item type (`997_merc_gear_type.py`, "merctype" in
  `loot_data.txt`), a parent type of body armor, rings, amulets, gloves, boots and belts.
- Where: found at load by byte pattern in the game's code section (the patterns and the method
  are PD2 Resurrected's, MIT, `merc_rules.cpp`); exactly one match each, or nothing changes. The
  plugin log line "Merc gear: on (server check at 0x..., panel check at 0x..., item type N)" gives
  the addresses on our build.
- How: D2RLoader's MutationService, both bytes in one transaction, the original `ba 03 00 00 00`
  checked first.
- If it fails: a message box offers to close the game, because a save loaded without the change
  loses whatever jewelry, gloves, belt and boots its mercenary wears.
