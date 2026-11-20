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
  Larzuk's socket quest 3 uses. Anya's resistance scroll is the exception: it is being reworked.
- **Highest rune per act** (decided 2026-11-20, replacing every earlier Act I limit): Act I Ral,
  Act II Shael, Act III Lem, Act IV Ohm (Act V: no limit set yet). Every rune source in an act
  stays at or below its cap, at the game's own odds between runes.
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
   rewards given once per difficulty are tripled; Anya's scroll gets its own rework (to decide).
   Note: our Den arena is not the real Den, so its quest (skill point, Akara's respec) needs a new
   way to be given.

## Act plans
- Act II: `docs/act2.md`.

## Still to decide (ask Fabrizio)
- What Anya's resistance scroll becomes.
- Item levels for 4, 5 and 6 sockets.
