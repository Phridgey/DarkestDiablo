# Flattening difficulties (the plan, decided 2026-11-20)

Fabrizio's direction for the whole mod. Both of our Claude sessions follow it: new work should fit
it, and nothing should be built that only makes sense with Normal / Nightmare / Hell.
The same plan is on a whiteboard: https://claude.ai/artifact/1SPdF9aSPgn8n562khHspp

## Rules (settled)
- **No difficulties.** There is no Normal, Nightmare or Hell for the player. A run is one pass
  through Acts I to V. The game always runs on Normal underneath.
- **About 18 character levels per act.** After Andariel a hero should be level 18, give or take 2;
  after Act II about 36, and so on (about 90 at the end of Act V).
- **One world per camp.** Route progress belongs to the camp, not to each hero. Clearing a stop's
  objective opens the next tier for the whole camp. The route map is the same for every hero.
- **Free re-runs, limited by level.** Any open stop can be played again, until the hero is
  **more than 2 levels above the stop's level** (its monster level). That hero is then locked out
  of it; other heroes can still go. To keep farming, swap heroes.
- **Forks have no pick limit.** Every option of a fork opens once its tier is reached.
- **Winning a stop** is still its objective (Griswold, the target unique, the Countess, Andariel...).
- **No first-time bonuses** (the +25% magic find for a stop's first pick goes).
- **Potions:** 3 per clear stay, and can be farmed.
- **Death:** a dead hero is gone; the camp carries on.
- **Quitting mid-stop starts it over.** The resume and give-up rules (route.h) go away: the good
  rewards are at the end, and a hero who doesn't clear doesn't win back what they spent.
- **Resistance penalty: linear, by camp progress** (decided 2026-11-20). It grows evenly
  instead of in the base game's two steps (-40 at Nightmare, -100 at Hell), and tops out at
  **-70** at level 90: 30 softer than the base game's -100, because Anya's +30 resistance scroll
  (Act V) is being reworked and no longer makes up for it. It follows the camp's progress, not
  each hero's level: the penalty is -70 x (camp level / 90), where the camp level is the target
  level of the furthest stop the camp has cleared. So after Andariel (18) every hero has -14; a
  fresh hero joining a late camp gets the camp's penalty too.
- **Quest rewards given once per difficulty are tripled** (decided 2026-11-20), since there is
  only one pass: for example the Den's skill point becomes 3, Radament's book 3 skill points,
  Lam Esen's tome +15 stat points, Izual's 2 skill points 6, the Golden Bird's +20 life +60, and
  Larzuk's socket quest 2 uses (decided 2026-11-21). Two more socket rewards are added: Charsi
  after the Malus (Act I) and Hratli once the hero holds Khalim's three parts, before Khalim's Will
  is made (Act III); see "Socket rewards" below. Two exceptions (decided 2026-11-21): the barbarians' rescue gives
  3 random runes from Pul to Ohm, and Anya's scroll becomes a scroll that raises the maximum of one
  resistance of the player's choice by 5.
- **Highest rune per act** (decided 2026-11-20, replacing every earlier Act I limit): Act I Ral,
  Act II Shael, Act III Lem, Act IV Ohm, Act V no limit. The caps hold for the crafting hunts
  and quest rewards; regular monster loot is not capped (decided 2026-11-21): it follows the
  monsters' levels, and higher tables can drop higher runes.
- **Every XP grind** (in every act) gives +40% experience and gold piles worth 3 times as much.
- Why: resistances and immunities are fixed per encounter, so players learn which hero suits which
  stop and bring another hero when one is resisted.

## The work, in order
1. **Level curve.** A target level for every stop. Act I: Den 2-3, Blood Raven 5, fork 1 at 8-10,
   Tristram 12, fork 2 at 14-15, Andariel 18. The same number drives the 2-level lockout.
2. **Experience.** Add up each stop's experience (monsters per wave times their worth at that
   level) and tune it so a full Act I ends at level 18 +/- 2. A script reports the expected
   character level after each stop.
3. **Monster strength.** Monster power comes from the monster-level table (monlvl.txt), which has
   Normal, Nightmare and Hell columns. Rebuild its Normal columns: Normal values at low levels,
   Nightmare in the middle, Hell high, blended at the joins. Resistances and immunities are per
   monster type, and each act has its own types, so each act takes them from the difficulty we pick.
4. **Treasure.**
   - Base items mostly scale by themselves: the game moves a monster's loot table up within its
     family as monster level rises, and the families run through all three difficulties.
   - Our own loot tables (Mausoleum, magic-find copies, Countess, Griswold...) are cut off from that
     (their group and level are cleared), so they must be rebuilt to follow level.
   - Sockets: each item type's most sockets depend on item level (itemtypes.txt, two thresholds per
     type, 25 and 40 by default). Move them so 4+ sockets don't arrive too early.
   - Rune caps per act: Ral, Shael, Lem, Ohm (see the rules above). Act I is done: the Mausoleum
     drops El to Ral, the Countess Ith to Ral.
5. **Penalties and rewards.** The resistance penalty becomes the linear, camp-based one above,
   to -70 (the game always runs on Normal, which has none, so the plugin applies it). The quest
   rewards given once per difficulty are tripled; Anya's scroll is reworked (+5 to one maximum resistance).
   Note: our Den arena is not the real Den, so its quest (skill point, Akara's respec) needs a new
   way to be given.

## Socket rewards
The game's only socket reward is Larzuk's, a quest flag the plugin cannot set again. The added
rewards (Charsi in Act I, Hratli in Act III) work with an item of our own instead: the smith hands
over a socket token (the plugin creates it; it is not a quest item, so `createItem` accepts it),
and a Horadric Cube recipe turns token + any item without sockets into the same item with one
socket (the game's own recipe shape: `"any,nos"` input, `"useitem,sock=1"` output, as the Stone of
Jordan rare-socket recipe). To test: that the recipe adds a socket to unique and set items too.

## Catching up: the camp, retirement and camp charms (decided 2026-11-21)
The camp is the central mechanic: no one hero is meant to walk Acts I-V alone. Heroes stall, die
or retire, and new heroes push further using the loot the camp has banked. The monster levels
(`src/patches/004_area_levels.py`) are the slope the camp climbs, not one hero's path.

Two things bring a new hero up to the camp's frontier:
- **Retirement** (the "blaze of glory" in `docs/design.md`): a hero who won't go further takes one
  last fight and brings everything home to the camp.
- **Camp charms**: charms that raise experience gained and magic find, and stop working above a
  maximum hero level, so they help new heroes catch up and do nothing for veterans.

How the cap works (the game has no "stops above level X" rule for items; a requirement only
keeps a hero below it from using an item). First choice:
1. The plugin switches a charm off for a hero above its cap: it cancels the charm's bonus
   through `gamewrite.h` (stat changes, allowed by the project rules). The charm stays in the
   inventory and works again on a lower hero.
Other options, if that fails: 2. bonuses that shrink each level to zero at the cap (only if the
game has per-level versions of the experience and magic find stats); 3. charms kept by the camp
(the stash) whose bonus the plugin gives every hero under the cap.

Open questions:
- Caps: one per act (e.g. the act's highest stop level), or fixed levels?
- How big the bonuses are (experience %, magic find %), and whether they stack.
- Where charms come from (camp rewards, drops, challenges) and whether they are shared.

## Act plans
- Act II: `docs/act2.md`.
- Act III: `docs/act3.md`.
- Act IV: `docs/act4.md`.
- Act V: `docs/act5.md`.

## Still to decide (ask Fabrizio)
- Camp charms: caps, bonus sizes, where they come from (see "Catching up").
- Item levels for 4, 5 and 6 sockets.
