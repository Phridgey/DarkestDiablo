# Linear zone (prototype)

Sam's test of a custom zone (2026-11-20): a short, linear map, like a much shorter Path of Exile
map. The player enters on one side, fights through to the other and kills an elite at the end.
Reached through a red portal in town. Uses the custom Den of Evil's look and monsters.

## How to try it
1. In the Rogue Encampment, a red portal stands a few steps from the fire (about 1.5 s after
   arriving). Walk into it.
2. You arrive at the west end of a cave six rooms long. The game's own random packs of Den
   monsters (Fallen, Fallen Shamans, zombies, with the odd champion or unique pack) are spread
   along it.
3. Walk east. When you reach the last room, Bishibosh (a Fallen Shaman boss with his Fallen)
   comes out a little ahead of you.
4. Killing him gives a Town Portal scroll, as in the arenas.

Test commands (console): `dd-zone` goes straight to the zone; `dd-zone-portal` puts the red
portal where you stand and prints its offset from the fire (to choose a better spot);
`dd-zone-boss` spawns the boss next to you.

## How it is built
| Part | Where |
| --- | --- |
| Map: Blizzard's Act I cave rooms `cavee`, 4 x `caveew`/`caveew2`, `cavew` in a row (145 x 25 tiles) | `src/maps/linear_zone.json` |
| Level: "Act 1 - Jail 3" (Id 31) reused, with the Den's look, sound, name, monster level and monsters; random packs on (lvlprest `Populate` 1, `MonDen` 900) | `src/patches/120_linear_zone.py` |
| Red portal, travel, arrival spot, boss, scroll | `src/plugin/zone.cpp` |

## Engine limits (and the workarounds)
- **No real linked portal.** The SDK can't make a portal with a destination, and a raw
  `cheat ospawn` portal leads nowhere. The red portal is the Tristram portal's look
  (objects.txt row 189, CainPortal, a dummy the game never operates). The plugin watches the
  hero's position and sends them with `goto 31` when they step into it.
- **`cheat ospawn` places at the hero's feet.** The hero is moved to the portal spot and back
  in the same moment (`location`), as for the traveler's portal.
- **Maps are stitched from Blizzard's rooms.** No new terrain; the tunnels bend as Blizzard drew
  them. The layout is the same every game; the monsters are new every game.
- **Spawns only work near the hero.** The boss can't wait in the end room; he comes out when the
  hero gets there.
- **The zone keeps its state for the whole game.** Going back in during the same game finds it
  cleared; a new game makes it fresh.

## Numbers to tune after the first test (top of `src/plugin/zone.cpp`)
- `PortalFromFireX/Y`: where the portal stands next to the fire (guessed).
- `StartTileX/Y`: the arrival spot in the first room (guessed; the hero is only moved if `goto`
  put them outside the first room).
- `BossRoomEnter`, `BossAhead`: when and where the boss comes out.
- `DENSITY` and `MONSTERS` in the patch: how packed the corridor is, and with what.

## Not known yet (first test)
- Whether the game fills a hand-made map with its random packs. Its own preset levels
  (Tristram) are filled, so it should. If not, the plugin can spawn a pack per room instead.
- Whether the six rooms join cleanly (no walls across the openings, no black gaps).
