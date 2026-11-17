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
  change life or stats. So the roll has to be a **skill** in the game's tables. The closest
  engine code to a roll is the Barbarian's **Leap**: it carries the hero to a point and knocks
  back monsters where it lands. Its range, minimum airtime and speed are table values we can tune.
- **Input.** The plugin can add a rebindable key (InputService), but it has no way to make the
  hero use a skill. On keyboard and mouse D2R only casts skills from the mouse buttons, so the
  roll is a right-click skill (or a skill hotkey, then right-click). A real "press Space to
  roll" would need engine work the SDK does not offer.
- **Cancelling.** D2 never lets a new action cut an attack or cast short. The roll can't cancel
  other actions the way PoE2's does.
- **Avoiding hits.** No table setting gives a short window of immunity. Leap may already make
  the hero untouchable in the air; that needs testing. If not, the options are a plugin timer
  that runs the `cheat invincible` console command around the roll (works only because the mod
  is offline, and the plugin would need to know when the roll starts, which it currently
  can't), or accepting a roll that only moves.
- **Animation.** D2R's player animations are 3D files in Blizzard's own formats; there are no
  public tools to author new ones. The roll uses Leap's animation sequence. On a Barbarian
  that is the leap; on the other classes it is untested and may look odd or fall back to
  another pose. A real roll animation would need custom model tooling and is not in this step.

## First version (prototype, `src/patches/050_dodge_roll.py`)

A new skill, **Dodge Roll**, that every class has from level 1 (in the same free skill slots as
Throw and Kick, so it shows up with them in the skill picker):

- Leap's code with a short fixed range (every roll is the same length), less airtime, more speed.
- No mana cost, no cooldown, works in town.
- A small knockback where it lands, like PoE2 pushing small monsters aside.
- Placeholder icon (Leap's picture slot), English text in every language.

The tuning values are at the top of the patch file.

## Test in game

Use a new hero (skills from `charstats.txt` are given when a hero is made).

1. Put Dodge Roll on the right mouse button. Roll in town and in a fight, with a Barbarian and
   with one other class.
2. Does each roll go the same distance toward the cursor? Too long, too short, too slow?
3. What does the animation look like on the non-Barbarian?
4. Stand in a pack of melee monsters and next to an archer, and roll through them. Do hits
   land while in the air?
5. Any crash or error in `d2rloader/logs/d2rloader.log`.
