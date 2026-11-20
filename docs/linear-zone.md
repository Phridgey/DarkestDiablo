# Tower zone (prototype)

Sam's test of a custom zone (2026-11-20). A first try, a long linear cave, was far too big.
This one is the Forgotten Tower's last floor turned into a small quest map:

1. In the Rogue Encampment, a red portal stands a few steps from the fire (about 1.5 s after
   arriving). Walk into it.
2. You arrive in the south loop of the Tower floor. Packs of Tower monsters (Fallen, ghosts,
   goatmen, archers) come out as you go. They have 80% of the Den monsters' life, damage,
   defense, hit chance and experience.
3. An elite waits in each side room (west and east), with a few minions. Each drops a Tower
   seal (1x2): the West and the East Tower Seal.
4. The north room (the Countess's) is empty except for an altar (the Tower's tome). Walk up to
   it or click it with both seals in your inventory: the seals vanish and Blood Raven comes out.
5. Killing her gives a Town Portal scroll, as in the arenas.

Seal rules:
- Put in the stash (personal or shared) or the Cube, a seal jumps back to the inventory. With no
  room, it stays there and jumps back as soon as there is.
- Seals don't outlive the game: any seal the hero didn't pick up in this game vanishes (the
  game saves items when you quit, before the plugin can act, so they go at the next game's
  start instead).

Test commands (console): `dd-zone` goes straight to the zone; `dd-zone-seals` prints where you
stand (zone cell) and which seals you picked up; `dd-zone-boss` brings out Blood Raven without
the seals; `dd-zone-portal` puts the red portal where you stand and prints its offset from the
fire.

## How it is built
| Part | Where |
| --- | --- |
| Map: Blizzard's `act1/crypt/cryptcountess2` (the Countess floor's second layout, 31 x 31 cells) without its monsters, chests and gold | `src/maps/tower_zone.json` |
| Level: "Act 1 - Jail 3" (Id 31) reused, with the Tower cellar's look and sound and the Den's monster level; random packs off. The real Tower floor (level 25) is untouched: the Countess stop uses it | `src/patches/120_linear_zone.py` |
| Monsters `dd_tower_<id>` (80% of the Den's numbers), the elites (`dd_tower_keeper_w/e` and two new superuniques), the seals `zs1`/`zs2` and their loot tables | `src/patches/120_linear_zone.py` |
| Red portal, arrival, packs, elites, altar, Blood Raven, scroll, seal rules | `src/plugin/zone.cpp` |

The elites' superunique rows and the seal codes reach the plugin as `towerelite <n> <row> <code>`
lines in `d2rloader/loot_data.txt`.

## Engine limits (and the workarounds)
- **No real linked portal.** The red portal is the Tristram portal's look (objects.txt row
  189, a dummy). The plugin sends the hero with `goto 31` when they step into it.
- **`cheat ospawn` places at the hero's feet.** The hero is moved to the spot and straight back
  (`location`) to put up the red portal and the altar.
- **No object-click event.** The altar fires when the hero reaches it, which clicking it does.
- **The stash can't refuse an item.** The plugin moves a stored seal back to the inventory.
- **Nothing can run as the hero quits.** Leftover seals vanish at the next game instead.
- **Spawns only work near the hero.** Packs, elites and Blood Raven come out as the hero nears.

## Numbers to tune after the first test (top of `src/plugin/zone.cpp`)
The spots (`Entry`, `Altar`, `RavenAt`, `EliteAt`, `Packs`) are map cells read from the map file,
not yet checked in game; `dd-zone-seals` prints the cell you stand in. `STRENGTH`, the monster
kinds and the elites' mods are at the top of the patch.
