# The caravan's stash

The save's stash starts small and grows over many heroes. Rules: `src/plugin/stash.h` (tested in
`tests/camp_tests.cpp`); in the game: `src/plugin/stash.cpp`; stash window: `src/patches/130_stash.py`.

## What the player sees
- **Only shared pages.** The personal page is gone. What a hero still has there, or puts there, is
  moved to the open shared pages while the stash is open; what doesn't fit stays where it is.
- **Pages.** A save starts with 1 page. 9 tiers add one each, for 50,000 / 100,000 / 175,000 /
  300,000 / 500,000 / 750,000 / 1,000,000 / 1,500,000 / 2,500,000 gold (6,875,000 in all). Gold is
  paid in parts, by any hero of the save (a hero can only carry 10,000 per level). Gold on the
  shared pages has to be withdrawn first; the hero's old personal stash gold is used up first.
- **The Caravan button** on the stash's right side opens the window with the page tiers (left) and
  the passive tree (right).
- **Points.** 1 point the first time a class finishes an act in this save (8 classes x 5 acts: 40).
  They stay when the hero dies; a later hero of the same class earns nothing for the same act. Acts
  finished before this was added count the next time that hero is played. The tree costs 28.
- **The Upgrades tab** (first node): one slot per gem, rune and potion, kept as counts. Nodes open the
  kinds (by quality, rune range, potion type), raise the limit per slot from 20 to 99, and add the gem cutter,
  a "Store bag" button and the Porter (stores gems and runes on coming back to town). Click a slot
  to take one out or to store what is in the hand; Ctrl+click a bag item stores it.

## Choices the engine forced
- **Not the game's own gem and rune tabs.** They exist only for heroes made with the Warlock
  rules: our heroes' stash file (`SharedStashSoftCoreV2.d2i`) and tables (`excel/base`) have none of it.
  Storing an item here destroys it and counts it in `stash.txt`; taking one out makes a new one.
  After a crash (not a normal exit) an item stored since the last save of the hero can come back
  in the bag while it is also counted here.
- **The personal tab** can't be taken out (the game ties tabs to their numbers), so it is moved out
  of the window and covered. Whether the stash then opens on the personal page first is open, see below.
- **Locked pages.** The plugin can't ask which page shows. It counts arrow clicks (and corrects itself
  on item clicks), and stops the right arrow at the last open page. Anything that still lands on a
  locked page is moved to an open one, so a wrong count never gives free space or loses an item.
- **Old saves.** Locked pages that already held items stay open until they are emptied.
- **Panels.** Every change draws a new layout in the next spare panel (5 for the window, 5 for the
  tab, 1 for the button). With the save slots' spares cut from 14 to 6, the plugin uses 63 of 64 panels.
- The controller and legacy-graphics stash windows still show the personal tab and no Upgrades tab
  (items put there are moved all the same).

## Not yet tried in the game
1. Does the stash open on the shared pages? If it opens on an empty page, click Shared. `dd-stash
   autotab` makes the plugin ask for the shared tab (`SelectTab:1`) instead; try it on a save that
   doesn't matter first, because the game's own message carries no text.
2. Do the Caravan button and both windows open, over the bag and the stash, and close with the stash?
3. Do changes show after more than 5 of them (spare panels used a second time)?
4. Does paying take the gold (`cheat stat gold,<n>`)? It only counts gold that is seen to be gone.
5. Taking out to the hand, storing from the hand, Ctrl+click, "Store bag", Porter, gem cutter.
6. Are personal items and items on locked pages moved?

Console: `dd-stash` (what this save has); for tests `dd-stash points <n>`, `dd-stash gold <n>`.
