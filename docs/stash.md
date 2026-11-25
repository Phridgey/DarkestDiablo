# The caravan's stash

The save's stash starts small and grows over many heroes. Rules: `src/plugin/stash.h` (tested in
`tests/camp_tests.cpp`); in the game: `src/plugin/stash.cpp`; stash window: `src/patches/130_stash.py`.

## What the player sees
- **Only shared pages.** The personal page is gone. What a hero still has there, or puts there, is
  moved to the open shared pages while the stash is open; what doesn't fit stays where it is.
- **Pages.** A save starts with 1 page. 9 tiers add one each, for 50,000 / 100,000 / 175,000 /
  300,000 / 500,000 / 750,000 / 1,000,000 / 1,500,000 / 2,500,000 gold (6,875,000 in all). Gold is
  paid in parts, by any hero of the save (a hero can only carry 10,000 per level): what the hero
  carries, then its old personal stash gold, then the gold in the stash.
- **The Journal** (button under Caravan; click again or Minimize to fold it) is a small window beside the
  stash. Clicking a rune in the bag, on a shared page or in the Upgrades tab lists its
  runewords by level; pointing at a name shows what it does. Alt+click looks a rune up without picking it up;
  in the Upgrades tab the first click on a rune looks it up and the next takes it out.
  Its data comes from the game's tables (`tools/gen_journal.py`); the window is `src/plugin/journal.h`.
- **The Caravan button** on the stash's right edge puts the caravan window in the stash's place: page
  tiers on its left, passive tree beside them. Its **Stash** button goes back; the X closes both.
- **Points.** 1 point the first time a class finishes an act in this save (8 classes x 5 acts: 40).
  They stay when the hero dies; a later hero of the same class earns nothing for the same act. Acts
  finished before this was added count the next time that hero is played. The tree costs 40. Part of it is for the heroes'
  potions (`docs/potions.md`). Points spent on the old potion shelves come back; potions on them are gone.
- **The Upgrades tab** (first node): one slot per gem and rune, kept as counts. Nodes open the
  kinds (by quality, rune range), raise the limit per slot from 20 to 99, and add the gem cutter,
  a "Store bag" button and the Porter (stores gems and runes on coming back to town). Click a slot
  to take one out or to store what is in the hand; Ctrl+click a bag item stores it, from any page of the stash.

- **The safe.** Heroes with the Warlock rules have three tabs of the game's own (Gems, Materials, Runes).
  They are off (no tabs, their slots outside the window). What they hold goes to the safe, a tab after Upgrades that
  shows only what is in it. Click to take one out; nothing can be put in; the tab is gone once it is
  empty. It is one safe for all saves (the game has one such stash for all of them): `camp/safe.txt`.
  Ctrl+click on a gem or rune, which the game would send to the hidden tabs, stores it in the Upgrades
  tab, whichever page shows (to that page if its slot is locked or full). The moving is done in the stash
  file while the game starts (`modernstash.h`), after copying it to `camp/stash-backup-<time>-safe`; anything
  that gets into the old tabs during a game goes to the safe at the next start.

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
- **Panels.** Every change draws a new layout in the next spare panel (3 for the window, 3 for the
  tab, 1 for the button). With the save slots' spares cut from 14 to 6, the plugin uses all 64 panels.
- The controller and legacy-graphics stash windows still show the personal tab and no Upgrades tab
  (items put there are moved all the same).

## Not yet tried in the game
1. Does the stash open on the shared pages? If it opens on an empty page, click Shared. `dd-stash
   autotab` makes the plugin ask for the shared tab (`SelectTab:1`) instead; try it on a save that
   doesn't matter first, because the game's own message carries no text.
2. Do the Caravan button and both windows open, over the stash, and close with the stash?
3. Do changes show after more than 5 of them (spare panels used a second time)?
4. Does paying take the gold (`cheat stat gold,<n>`)? It only counts gold that is seen to be gone.
5. Taking out to the hand, storing from the hand, Ctrl+click, "Store bag", Porter, gem cutter.
6. Are personal items and items on locked pages moved?

Console: `dd-stash` (what this save has); for tests `dd-stash points <n>`, `dd-stash gold <n>`.

## The second stash file: heroes with the Warlock rules (0.28.3)
DLC owners' heroes may have the Warlock rules, whatever their class (byte 0xF8 of the save: 2 Lord of
Destruction, 3 Reign of the Warlock). The game gives those a stash file of their own,
`ModernSharedStashSoftCoreV2.d2i`: 5 pages of its own instead of 3, then a section for the Gems, Materials
and Runes tabs (type 1) and one for the Chronicle (type 2). Before 0.28.3 it was never joined, was moved
about with the saves while the game kept the first one in memory, and so pages 11-20 and 21-30 didn't
hold what the plugin thought. Now (`modernstash.h`) it is laid out like the other: the same pages for each
save, the same 4 for ascended heroes, two unused pages at the end; joined once while the game starts
(old files in `camp/stash-backup-<time>-modern`, marker `camp/stash-joined-modern.txt`), kept in place when
saves change, emptied with a deleted save (`camp/wipe-modern.txt`). Its gold goes to each save's purse.
- Pages that lie right stay. Pages of a parked save's file go to that save's pages, or an empty one of them.
- **Still two stashes.** Heroes with and without the Warlock rules of one save don't share items: the
  game loads one file or the other. The purse, the Upgrades tab, the safe and the page tiers are shared.
- `split_stash.py` doesn't touch this file.
- Checked against a real file of the DLC (from the D2SSharp project's tests; not in this repo), both the
  layout and the stack sizes. Not yet tried in the game with a DLC hero. `dd-stash old` logs which rules
  the hero has and every shared item's page.
- Sources: github.com/ResurrectedTrader/D2SSharp (file format), theamazonbasin.com/wiki (5 pages, separate stash).

## One stash file for all saves (0.22.1)
The game reads `SharedStashSoftCoreV2.d2i` once, **when it starts** (not at the first game, as thought
before: on 2026-11-24 a save opened right after starting got the other save's stash written over its
own), and keeps it until it closes. So the file is never swapped. Save 1 has pages
1-10, save 2 11-20, save 3 21-30; ascended heroes' pages follow, 4 a save (31-42) (`src/plugin/stashfile.h`; the file is just its pages one after
another). Changing saves needs no restart.
- **Joining** happens once, while the game starts. The old files are kept in `camp/stash-backup-<time>`.
  A file it doesn't know, or items past page 14, and nothing is touched (then changing saves still needs
  a restart).
- **Always pages 1-10.** Opening the stash goes all the way back and then forward to the save's own
  pages (`HomePage`); the arrows stop at its ends; our own page number covers the game's. An item on
  another save's page can't be taken (the click is refused and the stash goes back).
- **Gold.** The game has one purse for every page, so gold is kept per save in `stash.txt` (`purse`),
  with Deposit and Take buttons for the amount between the arrows (100 to 100,000, or all). The game's own gold controls are moved out of the window and refused.
  The old purses were put in at the joining. Paying uses the hero's gold first, then the purse.
- **Deleting a save** copies its pages to `camp/trash`. If the game holds the stash, they are emptied
  when the new save's stash is first opened (`camp/wipe.txt`), or at the next start.
- **Going back** to a version before this: `python tools/split_stash.py "<save folder>"` first.
- **Not for the controller layout:** it still shows the game's page number and gold.
- **Opening on Shared.** The game opens the stash on the hero's own page and plugins can't choose a tab.
  Where the player clicks Shared is remembered (`camp/stash-spot.txt`, for that window size) and clicked
  for them from then on; `dd-stash autotab` turns it off.
