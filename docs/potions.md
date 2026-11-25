# Permanent potions

Every hero has its potions from the start and keeps them. They hold charges; using one takes charges,
kills and time bring them back. Rules: `src/plugin/potions.h` (tested in `tests/camp_tests.cpp`); in the
game: `src/plugin/potions.cpp`; tables and HUD: `src/patches/131_potions.py`. Decided 2026-11-24.

## Numbers
| Potion | Holds | From | Effect |
|---|---|---|---|
| Healing | 50 | start | the game's amounts: Minor, Light at level 10, Healing 20, Greater 36, Super 54 |
| Mana | 50 | start (Light) | Light, Mana at level 20, Greater 36, Super 54 |
| Antidote, Stamina | 20 | an Act I stop each | ends poison / fills stamina, and keeps it so for 30 s |
| Thawing | 20 | an Act III stop | ends cold and freezing, and keeps them off for 30 s |
| Rejuvenation | 75% of the healing potion's | an Act IV stop | 35% life and mana at once |
| Full Rejuvenation | 50% | an Act V stop, 5% of heroes | 100% life and mana at once |
| Fire pots, gas pots | 30 | caravan tree | a use puts 5 throwing potions in the bag (not past 25 of a kind) |

- A use takes 10 charges.
- A kill gives 0.5 charge, times the game's experience multipliers: champion x3; unique and minion x5;
  and the level gap's. Supporting potions get 75% of it.
- Every potion refills 0.1% of what it holds a second.
- A belt adds 4, 8 or 12 charges to every potion, as many as the slots it used to add.
- Caravan tree: 0.2% and 0.3% a second; +25% charges for healing, for mana; +25% from kills; healing also
  uses Antidote / Stamina, mana also uses Thawing, at 50% effect (15 s) and no charge; throwing potions
  (2 points each step). Rejuvenation has both the healing and the mana passives.
- Affixes: placed once per hero on stops of the act it has not cleared yet, never an act's last stop. On
  a fork the stop may go unpicked. The route map shows the potion beside the stop. A supporting potion
  whose act an older hero has already passed is given at once.

## Choices the engine forced
- **The potions are not items.** The loader can't create, edit or move anything in the belt. So they
  are drawn over the belt's place in three spare panels, and the plugin works their effects by
  changing life, mana and stamina (`gamewrite.h`). Antidote and Thawing don't give the resistance the
  game's own do: a stat the hero doesn't have can't be added.
- **Throwing potions are made, not thrown from the slot.** The plugin has no way to launch a missile.
  Equip a stack once; the game should take new ones from the bag by itself.
- **The game's belt is off the screen**, not gone, and its size is unchanged, so saved heroes load.
  Potions an older hero left in it can't be clicked.
- Keys: "Use potion 1-4" under Darkest Diablo in the Controls menu, 1-4 by default.
- The file is `camp/slot<N>/potions/<hero>.potions.txt`; burying the hero buries it.
- The controller and legacy-graphics HUDs still show the game's belt and no slots.

## Not yet tried in the game
1. Do the slots show, in the belt's place, and redraw without flicker in a fight?
2. Do keys 1-4 work while the game also has them for its belt? The log says which key each slot got
   ("Potions: key for slot"). Clicking a slot uses it too.
3. Do healing and mana really fill, at the right pace? Log: "the hero has N copies in memory" (2 or more).
4. Do Antidote and Thawing end poison, cold and freezing?
5. The belt line: the number should be 4, 8 or 12 (the game may count the first 4 slots too).
6. Throwing potions: made, and taken from the bag when the hand is empty?
7. Does the route map show the affixes, and change after they are won?

Console: `dd-potions` (what the hero has); for tests `dd-potions fill`, `give <antidote|thawing|stamina>`,
`concoct`, `full`, `use <1-4>`.
