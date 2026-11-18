# Game values the plugin reads

The D2RLoader SDK has no way to ask where the hero or a town object stands. CLAUDE.md allows the
plugin to READ a few such values from the game's memory, on the frozen build only. This page lists
every value read and where it sits. Nothing here writes to the game or changes its code.

Code: `src/plugin/gamememory.h` and `gamememory.cpp`.

## Safety

- Only on D2R.exe build 95032 (`D2R_VERSION` 3.4.95032). The plugin reads the file version of
  D2R.exe once; on any other build it reads nothing and logs "reading is off".
- Reads use Windows' `ReadProcessMemory` on the game's own process. A bad address makes the read
  fail (it returns false) instead of crashing the game.
- Only readable, writable, private memory blocks are searched. Nothing is ever written.

## How the values are found

There are no fixed addresses: the game places its units somewhere new every game. The plugin
searches its memory once in town, on a background thread, a few seconds after arriving:

1. It looks for the hero's name. The name is the first thing in the hero's player data.
2. It looks for units that point to that player data. A unit keeps it 0x10 bytes from its start.
3. In the same search it collects object units of the town fire (objects.txt row 39,
   RogueBonfire) and keeps the one nearest to the hero.

The hero's unit is then remembered, so later positions are read straight from it.

## Unit layout used

Taken from the community's long-standing map of the D2R unit structure and checked in game.

| Value | Where | Size |
| --- | --- | --- |
| Unit kind (0 player, 1 monster, 2 object) | unit + 0x00 | 4 bytes |
| Class (players) or objects.txt row (objects) | unit + 0x04 | 4 bytes |
| Player data (the name comes first) | unit + 0x10 | pointer |
| Path (where the unit stands) | unit + 0x38 | pointer |
| Hero X, Y in sub-tiles | path + 0x02, path + 0x06 | 2 bytes each |
| Object X, Y in sub-tiles | path + 0x10, path + 0x14 | 4 bytes each |

Positions are world sub-tiles, the same numbers `location player` prints in the console.

## What uses them

- The traveler's portal: it opens on the far side of the fire from the hero (`PlacePortal` in
  `campfire.cpp`). If either position can't be read, it opens where the hero stands.

## Checking it in game

Type `dd-where` in the console: it prints the hero and fire positions and the last search's
report. Compare the hero's position with `location player`. `dd-where scan` searches again.
The plugin log has a "memory scan" line after each search.
