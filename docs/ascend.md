# Ascend

A hero's last fight. Rules decided 2026-11-24. Code: `src/plugin/ascension.cpp` (the fight),
`legacy.h` (the rules of the items' way home), `ascend.h` (save files),
`src/patches/990_ascension.py` (arenas, bosses, loot), `src/maps/ascend*.json`.

## What the player sees
- **Ascend**, red, under Enter at the Campfire while a hero is picked, at any level. A question
  explains it, warns a hero below the act's advised level, and says a mercenary may come but its
  gear does not come home.
- The hero goes to the boss room of the act its route is in, with only the Ascended boss there:
  40% more life, 35% more damage, 10% more movement, 50% more attack speed, 35% more cast speed,
  4 random elite affixes, tinted. Never immune to anything: no fire, lightning or cold enchanted, and
  the build stops if any affixes could add up to an immunity.
- **No way out.** Dying, quitting or crashing before the boss is dead is a death. A Town Portal
  leads straight back.
- The boss drops 3 sets or uniques and 3 of its own. Around 22 more (rare gear, rare rings, amulets and
  jewels, magic charms, affixes of the next act) burst out around the hero a moment later.
- A portal opens in the middle. Walking up to it asks "Ascend?". Not yet: carry on looting. Yes, or
  leaving the game in any way: the hero ascends.
- The class locks and comes back with the traveler, as after a death. What the hero wore and carried
  (bag, belt, Cube's contents, stash) waits on a stash page under its name. Not the Cube, not the
  mercenary's gear. The page can only be emptied; when empty it goes away.
- The old Retire is now **Dismiss**.

## Choices the engine forced
- The SDK can't move worn or belt items, and the shared stash is read once a session, so items travel
  in the save of the next hero loaded, in its hidden personal stash, and move to the page a second
  after it comes in. The ascended hero's save is kept in `camp/slot<N>/ascended/`, and the carrier's
  save from before in `<hero>.before.d2s`. Anything not understood to the last byte is refused.
- The page is one of 4 extra shared pages (`add_shared_tabs = 11`). More than one page of items, or
  more than 4 heroes, waits for a free page.
- A superunique takes 3 affixes: the 4th is part of the boss's numbers and shown in its name.
- A monster drops 6 items at most. The rest comes from 4 helpless creatures spawned and killed at once.
  Making the items with the SDK's `createItem` on the ground crashed the game (2026-11-24, client
  item packet, Spirit Mask suspected).

## Seen in the game (2026-11-24)
Act II: the arena loads, the boss comes out with its affixes, its death is noticed and the win recorded.

## Not yet tried in the game
1. Does each arena load, and is the hero's, boss's and portal's spot open floor?
2. Does the boss come out, tinted, with 3 affixes? The log says its life before and after.
3. Attack and cast speed: the log says how many values were written. Which of the two rates casts
   follow isn't known; if casts follow the attack rate they get +50% too.
4. Are the spoils killed at once (else they can be smashed), and do they drop their loot?
5. Does the game load a save with added items, and does the page fill? Are the arrows right?
6. Does the game write the hero's save before the Campfire reads it?
7. Does the game allow 14 shared pages?
