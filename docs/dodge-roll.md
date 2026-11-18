# Dodge Roll

Sam asked (2026-11-17) for a dodge roll like Path of Exile 2's. This page lists how PoE2's roll
behaves, what Diablo II: Resurrected with a D2RLoader plugin can and cannot copy, and what the
first version does.

We copy how the roll behaves (numbers and rules). We do not use any of GGG's files, animations
or code: they are GGG's copyrighted work and this mod is public. Any new animation must be made
from D2R's own character animations or made from scratch.

## How PoE2's roll behaves

From public guides (Mobalytics, poe2db, Sportskeeda, the 0.1.0d patch notes):

| | PoE2 |
|---|---|
| Input | Space (Circle on PS5). Rolls the way you aim (toward the cursor with a mouse). Hold it to sprint after the roll. |
| Distance | 3.7 metres, scaled by movement speed. Rolling again and again is slower than walking. |
| Time | "Slightly over a second" from start to standing up. Action speed makes it faster. |
| Avoidance | The first part of the roll avoids dodgeable hits: projectiles and melee strikes. Not area hits, ground effects or damage over time. The second part avoids nothing. |
| Collision | Your size is 0 while rolling, so you slip through gaps. Small monsters are pushed aside. You can still be boxed in. |
| Cancel | Cancels almost any action (attack backswing, skill recovery), but not another roll. |
| Cost / cooldown | None. It is not a skill. |

## What D2R plus our plugin can do

- **Moving the hero.** The plugin SDK (0.3.0) cannot move the hero, read where the hero is, or
  change life or stats. So the roll has to be a **skill** in the game's tables, reusing the
  movement code of an existing skill (see Animation below).
- **Input.** The plugin can add a rebindable key (InputService), but it has no way to make the
  hero use a skill. On keyboard and mouse D2R only casts skills from the mouse buttons, so the
  roll is a right-click skill (or a skill hotkey, then right-click). A real "press Space to
  roll" would need engine work the SDK does not offer.
- **Cancelling.** D2 never lets a new action cut an attack or cast short. The roll can't cancel
  other actions the way PoE2's does.
- **Avoiding hits.** No table setting gives a short window of immunity. If not, the options are a plugin timer
  that runs the `cheat invincible` console command around the roll (works only because the mod
  is offline, and the plugin would need to know when the roll starts, which it currently
  can't), or accepting a roll that only moves.
- **Animation.** D2R's player animations are 3D files in Blizzard's own formats; there are no
  public tools to author new ones. A skill can only use poses the game already has, and skills
  that move the hero come with a fixed sequence of poses in the game's code. Moves that carry
  the hero across the ground: Leap (jump), Charge (fast run, then a swing at a monster),
  Whirlwind (spin), Dragon Flight and Teleport (instant).

## Current version (prototype, `src/patches/050_dodge_roll.py`)

- 0.11.0 used Leap. Sam wanted a different pose and a dash, not a jump.
- 0.11.2 used Charge: the right look, but it runs to wherever you click. Sam wants a set
  distance and PoE2's timing (2026-11-18).
- Now (0.11.4): a skill's movement code and its poses are separate columns, so **Dodge Roll**
  moves with Leap's code (a fixed distance toward the cursor, set by making its min and max
  range equal) and shows Charge's poses (the hero's run). Distance, shortest time and speed are
  at the top of the patch file, set to about PoE2's roll: roughly 3.7 metres in under a second.
  Unknown: whether Leap's movement and Charge's poses fit together, or look broken.
- **Dodge Dash** (test only, not for the Barbarian, who has one free slot): plain Charge, to
  compare against if the mix looks wrong.

Both are free (no weapon, damage, cost or cooldown), work in town, and every class gets them
from level 1 in the same free skill slots as Throw and Kick. Placeholder icons (Charge's
picture slot), English text in every language.

## Test in game

Use a new hero (skills from `charstats.txt` are given when a hero is made).

1. Put Dodge Roll on the right mouse button. Use it in town and in a fight, with a Paladin or
   Barbarian and with one caster (Sorceress or Necromancer).
2. Does it go the same distance every time, wherever you click? Does it look like a run, or
   does it stutter, jump or swing? Too slow or too fast compared with PoE2?
3. Try Dodge Dash the same way to compare the look.
4. Stand in a pack of melee monsters and next to an archer, and dash through them. Do hits land
   mid-dash?
5. Any crash or error in `d2rloader/logs/d2rloader.log`.
