# Darkest Diablo — design

A Hades-style roguelite built in Diablo II: Resurrected. Decisions so far (16 Nov 2026).
The shared whiteboard has the same plan as a picture; this file is the reference.

## The run
- One run spans all five acts. Each act's town is that act's own town:
  Rogue Encampment, Lut Gholein, Kurast Docks, Pandemonium Fortress, Harrogath.
- No open world. Each stop is a short tunnel the player can't get lost in, or an arena
  room with waves of enemies, on that act's tilesets.
- Stops follow the game's own quest order for the act.
- The player reaches the next stop by waypoint (the waypoint has been "scouted ahead").
  The waypoint menu doubles as the fork menu: only the fork's options are unlocked.
- The last kill on a map opens a town portal. The player returns to town to manage loot
  and comes back through the portal.
- At a fork, the player does 2 runs before moving on. The same option can be picked twice.
- Every stop gives +25% magic find the first time it is picked.

## Stop types
| Type | Drops | Magic find | Notes |
|---|---|---|---|
| Quest | Quest rewards | — | Uses the act's own quests |
| MF run | One guaranteed set or unique of the area's level, plus kill drops | D2R rules; bosses and unique monsters feel it more | |
| Crafting hunt | No magic items; runes, gems, crafting materials | Raises the rune chance instead | |
| XP grind | Normal drops | Normal | +33% experience |

Magic find also raises socket count, superior ("high quality") chance and ethereal chance
(MF runs and XP grinds).

## Act I
1. **Den of Evil** — forced quest. Akara gives a skill point back in town.
2. **Blood Raven** — forced quest. Unlocks the mercenary.
3. Fork (2 picks): **Blood Raven again** (MF) · **Mausoleum** (crafting hunt) · **Crypt** (XP grind)
4. **Tristram** — forced. Quest + MF: rescue Cain, unlock identify.
5. Fork (2 picks): **Pit** (MF) · **Countess** (crafting hunt, +2 bonus runes) ·
   **Horadric Malus** (XP grind; finishing earns an imbue)
6. **Andariel** — act boss. Open question: forced final stop?

## The camp (between characters)
- The camp is the run: the circle of characters by the fire, plus the stash.
- Hardcore rules: death is final for the character, and everything it wears and carries is lost.
  The camp carries on.
- Recruiting: one of each class in the campfire circle. The first character can be any class;
  each cleared stop offers 3 random classes to recruit, like Darkest Dungeon's stagecoach.
- The next character to play is picked from the campfire circle, with the stash in view to plan.
- Stashed items keep their level and attribute requirements. That is the balance.
- The stash starts small. Optional challenges (e.g. "bonus for spears", "can't use shields")
  unlock more tabs and stash features.
- **Atlas:** kiss and curse bonuses that keep less-progressed characters relevant. Points come
  from reaching a new furthest point; they can be re-allocated freely; profiles can be saved
  (pushing vs farming). No out-of-run skill tree.
- **Blaze of glory:** for a character that won't go further. One boss fight, no fleeing, uber
  difficulty for its level, huge rewards. Win: the character retires and brings its whole
  inventory home, boss loot included. When it can be tried: not decided.

## Technical approach
- Data edits (tables, maps) for everything they can do.
- A D2RLoader plugin (our code, built with the D2RLoader Plugin SDK) for what data can't: fork/waypoint control, run and camp state, first-pick bonuses,
  magic find effects on sockets/superior/ethereal and runes, death and retirement rules,
  class limit, challenges, Atlas.
- The plugin keeps its own camp file in the mod's save folder
  (`Saved Games\Diablo II Resurrected\mods\darkestdiablo`). Back it up with the saves.

## Open questions
- Is Andariel (and each act boss) a forced final stop?
- Examples of kiss and curse bonuses.
- When can a character attempt a blaze of glory?
- Does a recruit start at level 1? Do the 3 offers leave out classes already in the circle?

## Arena stops (decided 2026-11-17, updated)
- Waves come on a timer: wave 1 shortly after arrival, then one every 10 s, whatever is still
  alive. The stop is cleared when the last wave has spawned and everything is dead, the boss
  included, in any kill order (players may go for the boss first).
- The fight happens where the player can see it (e.g. Burial Grounds: the plaza by the hanging
  tree), never in a far corner.
- The player needs time to buff and prep: where possible, arrive in an entry room and walk
  through a gate or stairs into the arena; that starts the waves. No short timers on arrival.
- Burial Grounds packs: +50% over the first version.
- The player arrives by waypoint into one room, and the fight stays in that room.
- Example, Den of Evil: 5 waves of about 10 monsters, then a final wave with Corpsefire.
- The last kill drops a Town Portal scroll. That is the way home.
- The player can bail out of a stop early (too spooky); the goal is never being stranded.

## Tuning notes (later)
- Den arena: the room's open edge shows black nothingness; cap it with walls or a closing room.
- Den arena: the wave feel is right, but too dense for the first quest, when the player has no
  area damage yet. Lighter early waves, heavier later in the run.

## Route map (the waypoint "map")
- The town waypoint opens a map of the act's route (see the sketch: Den, Burial, fork x2,
  Tristram, fork x2, Catacombs). The player picks the next stop there.
- First version: a list panel, opened by the town waypoint (or `dd-map`). Waypoints outside
  town keep Blizzard's list, so the player is never stranded. Next: draw it as a tree.

## Fork 2 MF run: Rakanishu and Treehead (decided 2026-11-17)
- Replaces the Pit as the MF option at the fork after Tristram (Pit / Countess / Malus becomes
  Rakanishu+Treehead / Countess / Malus).
- Place: Treehead's usual spot in the Dark Wood, the Tree of Inifuss (Blizzard's `funktree`
  map piece), on a custom map far smaller than the Dark Wood so players can't wander off.
- Fight: waves of Fallen and Fallen Shamans, then Rakanishu (superunique row 3) with his pack;
  then waves of Dark Spearwomen ("lancers", cr_lancer1), then Brutes (brute2) and Quill Rats
  (quillrat1), then Treehead Woodfist (superunique row 4).
- Open question: how the player gets prep time before the waves (Burial uses an entry room with
  stairs; the forest has no door to pair stairs with).
