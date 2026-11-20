# Act II plan (decided 2026-11-20, not built yet)

Act II under the flattening plan (`docs/flattening.md`): about levels 18 to 36, highest rune Shael,
camp-wide progress, re-runs until a hero is more than 2 levels over a stop, forks with no pick
limit. Every map is a copy with no stairs (reached by the route map, left by Town Portal);
"shortened" means a smaller map composed from the level's own rooms, as the Act I arenas are.

| Tier | Stop | Kind | Target level | Cleared when |
|---|---|---|---|---|
| 1 | Halls of the Dead 3 | forced | 19-20 | the Horadric Cube is picked up |
| 2 | Sewers 3, heavily shortened, with Radament | fork: XP | 22-24 | Radament's quest item (the Book of Skill) is picked up |
| 2 | Stony Tomb | fork: crafting | 22-24 | (crafting hunt: as the Act I hunts) |
| 3 | Maggot Lair 3, shortened | forced | 26-28 | the Staff of Kings is picked up |
| 3 | Claw Viper Temple 2 | forced | 26-28 | the Amulet of the Viper is picked up |
| 4 | Arcane Sanctuary, one arm only | fork: crafting | 30-32 | the Summoner dies |
| 4 | Ancient Tunnels, shortened | fork: MF | 30-32 | (MF run: as the Act I MF runs) |
| 4 | Canyon of the Magi | fork: XP | 30-32 | (XP grind) |
| 5 | Duriel | forced | 35-36 | Duriel dies |

- **Tier 2 has no MF option**: Ancient Tunnels is only the later (tier 4) MF run.
- **Tier 3 is two forced stops on one row.** They can be done in either order and by different
  heroes; once both have been cleared by heroes of the camp, tier 4 opens.
- **Quest stops end on picking up the quest item** (Cube, Staff of Kings, Amulet of the Viper,
  Radament's book), not on a boss kill.
- **The Horadric Staff:** the Cube (tier 1), the Staff of Kings and the Amulet (tier 3) let the
  player make it in the Cube, as in the game, before Duriel.
- **Duriel:** the hero arrives straight in the staging area of the true Tal Rasha's Tomb (the room
  with the orifice), without running through the rest of the tomb. Inserting the Horadric Staff
  plays the game's own event (the wall breaks open), leading to Duriel. Only the tomb before that
  room is cut. To check while building: how the game picks the true tomb (it is random per game
  in the base game) and what the event needs, so our copy of that room plays it.
- **XP grinds** (Sewers 3 with Radament, Canyon of the Magi) follow the rule for every XP grind:
  +40% experience and every gold pile worth 3 times as much (as the Crypt and the Malus,
  `src/patches/115_xp_gold.py`).
- **Radament** drops his usual Book of Skill, worth 3 skill points instead of 1 (quest rewards
  given once per difficulty are tripled). One book, not three: the game's book only works while
  its quest is open, so a second and third copy would do nothing. Reading it completes the quest
  as usual (1 point) and the plugin adds the other 2.
- **Arcane Sanctuary** is cut down to one of the four arms of its cross: a straight path from the
  hero's arrival to the Summoner's platform at the far end. Monsters only spawn ahead of the hero,
  never behind. The Summoner always waits at the end; his death clears the stop.
