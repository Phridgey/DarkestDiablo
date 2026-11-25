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
| Fire pots, gas pots | 30 | caravan tree | thrown from the slot toward the cursor |

- A use takes 10 charges. Healing can't be used with full life, mana with full mana, Rejuvenation with
  both full.
- Talking to a healer (Akara, Fara, Ormus, Jamella, Malah) fills every potion.
- Each slot shows its charges and a bar with a notch per use.
- A kill gives 0.5 charge, times the game's experience multipliers: champion x3; unique and minion x5;
  and the level gap's. Supporting potions get 75% of it.
- Every potion refills 0.2% of what it holds a second.
- A belt adds 4, 8 or 12 charges to every potion, as many as the slots it used to add.
- Caravan tree: 0.4% and 0.6% a second; +25% charges for healing, for mana; +25% from kills; healing also
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
- **Throwing.** The hero gets a skill per throwing potion (`131a_throw_skills.py`), which only the plugin
  can use: it puts the skill on the right mouse button, presses that button, and puts the old skill
  back. So it flies from the hero toward the cursor, bursts on the first thing it hits (or at the end of
  its reach, not on the spot under the cursor), and kills give experience. A charge is taken only when
  the cast is seen. Not in town, not with the bag open, not with the cursor over the HUD (so by key, not
  by clicking the slot). Right-click must be the game's right-skill button.
- If the skill can't be selected, an invisible helper on the heroes' side throws at the nearest enemy
  instead (`994a_throwers.py`; its kills give no experience). `dd-potions bag` (until the game closes)
  puts 5 throwing potions in the bag instead.
- **The game's belt is off the screen**, not gone, and its size is unchanged, so saved heroes load.
  Potions an older hero left in it can't be clicked.
- Keys: "Use potion 1-4" under Darkest Diablo in the Controls menu, 1-4 by default.
- The file is `camp/slot<N>/potions/<hero>.potions.txt`; burying the hero buries it.
- The controller and legacy-graphics HUDs still show the game's belt and no slots.

## Not yet tried in the game
1. Do the slots redraw without flicker in a fight? (Seen: they show in the right place.)
2. Seen: keys 1-4 work, and the hero's 2 copies are found.
3. Do healing and mana really fill, at the right pace? Log: "the hero has N copies in memory" (2 or more).
4. Do Antidote and Thawing end poison, cold and freezing?
5. Seen: the belt line shows the right number.
6. Throwing: does it fly to the cursor, and does the old right skill come back? Log: "thrown at the
   cursor: cast seen", or why not.
7. Does the route map show the affixes, and change after they are won?

Console: `dd-potions` (what the hero has); for tests `dd-potions fill`, `give <antidote|thawing|stamina>`,
`concoct`, `full`, `bag`, `use <1-4>`.
