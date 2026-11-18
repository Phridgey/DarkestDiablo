# Game values the plugin reads

The D2RLoader SDK has no way to ask where the hero or a town object stands. CLAUDE.md allows the
plugin to READ a few such values from the game's memory, on the frozen build only. This page lists
every value read and where it sits. Nothing here writes to the game or changes its code.

Code: `src/plugin/gamememory.h` and `gamememory.cpp`.

## Safety

- Only on D2R.exe build 95032 (`D2R_VERSION` 3.4.95032). The game runs inside D2RLoader.exe, so the
  plugin reads the file version of the D2R.exe file in the game folder (found next to the
  process or up from the plugin's own folder),
  once. Its numbers are 16 bits each, so 95032 is stored as 3.4.29496.0 (95032 - 65536). On any
  other build it reads nothing and logs "reading is off".
- Reads use Windows' `ReadProcessMemory` on the game's own process. A bad address makes the read
  fail (it returns false) instead of crashing the game.
- Only readable, writable, private memory blocks are searched. Nothing is ever written.

## How the values are found

There are no fixed addresses: the game places its units somewhere new every game. The plugin
searches its memory once in town, on a background thread, a few seconds after arriving:

1. It looks for the hero's name. The name is the first thing in the hero's player data.
2. It looks for units that point to that player data. A unit keeps it 0x10 bytes from its start.
3. In the same search it collects object units of the town fire (objects.txt row 39,
   RogueBonfire) and keeps the one nearest to the hero, and object units of the town's stash
   chest (row 267, bank), keeping the one nearest to the fire.

The hero's unit is then remembered, so later positions are read straight from it.

## The unit list (the quick way)

The game keeps every unit in a fixed list inside one of its modules: for each kind of unit
(0 players, 1 monsters, 2 objects, ...) 128 chains, picked by the unit's number mod 128. Each
entry points at a unit, and each unit points at the next one in its chain.

The list's place is found once, after the slow search has found the hero: the plugin looks in
the game modules' writable memory for the hero's address, works back from the hero's chain to
the start of the list, and keeps it only if the object chains there hold object units. It saves
the module name and the distance from the module's start in `darkestdiablo-unitlist.txt` next
to the plugin. From then on each search reads the list directly (a few milliseconds): the hero
is the player unit whose player data starts with the hero's name, the fire is the nearest
object unit of row 39 and the stash chest the row 267 unit nearest the fire. If the saved place stops working, the slow search runs and finds it again.

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

Positions are world sub-tiles, the same numbers `location player` prints in the console.

## What uses them

- Fire or stash: both open the stash window, so the plugin compares the hero's distance to the
  fire and to the stash chest; the nearer one was used (`AtFire` in `campfire.cpp`).
- The traveler's portal: it opens on the far side of the fire from the hero (`PlacePortal` in
  `campfire.cpp`). If either position can't be read, it opens where the hero stands.

## Checking it in game

Type `dd-where` in the console: it prints the hero and fire positions and the last search's
report. Compare the hero's position with `location player`. `dd-where scan` searches again.
The plugin log has a "memory scan" line after each search.
