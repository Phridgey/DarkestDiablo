# Dodge

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

History: 0.11.0 used Leap (wrong pose). 0.11.2 used Charge (right look, but runs to wherever you
click). 0.11.4 mixed Leap's movement with Charge's poses ("Dodge Roll") next to plain Charge
("Dodge Dash") to compare. Sam (2026-11-18): one skill named **Dodge**, always the same distance
(that is very important), Leap's "brains" with Charge's effects, animation and timing, and 75%
less recovery. So since 0.11.9 Dodge takes:

- From Leap: the movement. Same distance every time toward the cursor, stopping at walls.
- From Charge: the poses (the hero's run), the look and sound on screen, the speed (300%, twice
  Charge's), and it can't be interrupted by hits.
- Recovery: the hero can act again from frame 2 of the dash instead of Charge's 8.
- Free (no weapon, damage or cost), works in town, every class from level 1.
- Cooldown (Sam, 0.11.13): 1.5 seconds (38 frames; the game counts 25 frames a second).

In the tables the skill is called `DD Dodge` (skilldesc `dd dodge`): the Amazon already has a
skill named Dodge, and a repeated name crashed the game at start (0.11.10: "Cannot load
skills: row ... repeats ID 13" in d2rloader.log). Players still see "Dodge".

Not possible yet: **dodging in the WASD movement direction.** The game aims every skill at the
cursor (or the clicked monster); no table setting or plugin service changes the aim.

Not possible yet: **one dodge per click.** The game repeats any skill while its button is held,
no table setting stops that, and the plugin can't see mouse buttons.

Unknown until tested: Leap's movement runs in the game's rules and Charge's in what the hero's
screen shows, so the hero might be drawn running toward the cursor and then snap to where Leap
put them.

## Test in game

Use a new hero (skills from `charstats.txt` are given when a hero is made).

1. Put Dodge on the right mouse button. Use it in town and in a fight, with a Paladin or
   Barbarian and with one caster (Sorceress or Necromancer).
2. Does it go the same distance every time, wherever you click, and stop at walls? Does it look
   like a run, or does it stutter, snap, jump or swing? Too slow or too fast?
3. Can you act again sooner after it than before?
4. Stand in a pack of melee monsters and next to an archer, and dash through them. Do hits land
   mid-dash?
5. Any crash or error in `d2rloader/logs/d2rloader.log`.
