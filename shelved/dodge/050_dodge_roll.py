"""Dodge: a quick dash every class has, modelled on how Path of Exile 2's roll plays.

What we take from PoE2 is only how it behaves (see docs/dodge-roll.md): a fast move of set
length toward the cursor, no mana cost, a short cooldown, usable in town. None of GGG's files,
animations or code are used.

How it is built: the game has no free-form "move here fast" in its tables, only skills whose
code is fixed. But a skill's movement code, its poses and its effects are separate columns, so
Dodge mixes two of the game's own skills (Sam, 2026-11-18: "the brains of Leap" with "the
effects, animation and timing" of Charge):

- From the Barbarian's Leap: the movement ("srv" functions). It always goes the same distance
  toward the cursor (min and max range set equal) and stops at walls.
- From the Paladin's Charge: the poses (the hero's own run), the look and sound on the hero's
  screen ("clt" functions, start sound), its speed, and it can't be interrupted by hits.
  The dash lasts as long as running the same distance takes, so it gets the hero out of danger
  but is never a faster way to travel; the hero can act again right after (frame 2).

Tried and dropped (0.11.58 to 0.11.72, see docs/dodge-roll.md): a fast dash followed by a green
slow. A Battle Orders-style landing pulse never reaches its own caster. Other skills' start code
(Berserk's, Blade Shield's) does put a state on the hero, but Leap's movement needs Leap's own
start code, and the Amazon's sidestep pose stops Leap's movement too.

Every class gets it the way every class gets Throw and Kick: through a free "Skill N" slot in
charstats.txt. Put it on the right mouse button (or a skill hotkey) to use it.

Known limit: the game repeats any skill while its button is held, and no table setting stops
that, so holding the button dodges again and again.
"""
import json
from pathlib import Path

# The name tables use. It must be unique: the Amazon already has a skill "Dodge" (and skilldesc
# "dodge"), and a repeated name crashes the game at start. Players see "Dodge" (STRINGS).
NAME = "DD Dodge"
DESC = "dd dodge"

# Tuning. D2 runs at 25 frames a second. PoE2's roll: about 3.7 metres in a bit under a second.
DISTANCE = 8          # Leap's "Range" (min and max equal, so every dodge is the same length). Leap: 8 to 30.
                      # Was 7; Sam (2026-11-18) wanted about 15% further, and 8 is the nearest whole step.
SPEED = 300           # Speed %, read as "Velocity %" by Charge's code and "Leap Speed %" by Leap's (Charge: 150).
# No travel advantage (Sam, 2026-11-18): a dodge must never cover ground faster than running,
# without standing still (the game can't force walking, and nothing tried could slow the hero
# after a dodge). Run speed is charstats "RunVelocity" (9 for every class) in the same units as
# Leap's range (sub-tiles a second; the game shows both as yards by multiplying by 2/3). So
# running 8 takes 8 / 9 s = 0.89 s = 23 frames, and the dash itself is made to last that long
# (Leap's minimum airborne frames). The hero keeps moving the whole time.
RUN_VELOCITY = 9
MIN_FRAMES = -(-DISTANCE * 25 // RUN_VELOCITY)  # Leap's "Minimum Airborne Frames" (Leap: 8): 23.
RECOVERY_FRAME = 2    # Charge's "seqinput" (Charge: 8).
COOLDOWN_FRAMES = 6   # Cooldown in frames (25 a second): 6 = 0.24 s, the nearest to Sam's 250 ms (was 1.5 s).

STRINGS = {
    "skillname_dodge": "Dodge",
    "skillsd_dodge": "dashes out of danger",
    "skillld_dodge": "Dash a set distance toward the cursor",
}


def _new_string_ids(count):
    """Ids above every id the game's string files use, so ours never clash."""
    folder = Path(__file__).resolve().parents[2] / "vanilla" / "data" / "local" / "lng" / "strings"
    top = 0
    for f in folder.glob("*.json"):
        try:
            top = max([top] + [e.get("id", 0) for e in json.loads(f.read_text(encoding="utf-8-sig"))])
        except (ValueError, AttributeError):
            pass
    start = max(top + 1, 90000)
    return range(start, start + count)


def patch(t):
    skills = t.table("skills")
    for table, column, name in ((skills, "skill", NAME), (t.table("skilldesc"), "skilldesc", DESC)):
        if any(r[column].lower() == name.lower() for r in table.rows):
            raise RuntimeError(f"{column} {name!r} already exists; names must be unique")
    next_id = max(int(r["*Id"]) for r in skills.rows if r["*Id"].isdigit()) + 1
    charge = skills.first("skill", "Charge")
    from_charge = ("anim", "seqtrans", "seqnum", "monanim", "cltstfunc", "cltdofunc", "cltstopfunc",
                   "stsound", "castoverlay", "cltoverlaya", "cltoverlayb", "interrupt")
    skills.add_copy("skill", "Leap", **{c: charge[c] for c in from_charge}, **{
        "skill": NAME, "*Id": str(next_id), "charclass": "", "skilldesc": DESC,
        "reqlevel": "1", "maxlvl": "1", "reqskill1": "", "skpoints": "",
        "mana": "0", "minmana": "0", "lvlmana": "0", "localdelay": str(COOLDOWN_FRAMES), "globaldelay": "",
        "InTown": "1", "leftskill": "0", "rightskill": "1", "stsoundclass": "",
        "Param1": str(DISTANCE), "Param2": str(DISTANCE),
        "Param3": "0", "Param4": "0", "srvmissilea": "",  # no knockback on landing
        "Param5": str(MIN_FRAMES), "Param7": str(SPEED), "calc2": str(SPEED),
        "seqinput": str(RECOVERY_FRAME),
    })

    desc = t.table("skilldesc")
    desc.add_copy(
        "skilldesc", "charge",
        **{
            "skilldesc": DESC, "SkillPage": "0", "SkillRow": "0", "SkillColumn": "0", "ListRow": "0",
            "str name": "skillname_dodge", "str short": "skillsd_dodge",
            "str long": "skillld_dodge", "str alt": "skillname_dodge",
        },
    )
    row = desc.rows[-1]
    for column in desc.columns:  # no damage, synergy or range lines
        if column.startswith(("desc", "dsc2", "dsc3", "ddam")):
            row[column] = ""

    # Every class: the first free "Skill N" slot, like Throw and Kick.
    stats = t.table("charstats")
    slots = [f"Skill {i}" for i in range(1, 11)]
    for row in stats.rows:
        if not row.get("Skill 1"):
            continue  # the "Expansion" divider row
        free = next((s for s in slots if not row.get(s)), None)
        if free is None:
            raise RuntimeError(f"charstats: {row['class']} has no free skill slot for {NAME}")
        row[free] = NAME

    strings = t.data("local/lng/strings/skills.json")
    langs = [k for k in strings[0] if k not in ("id", "Key")]
    for new_id, (key, text) in zip(_new_string_ids(len(STRINGS)), STRINGS.items()):
        strings.append({"id": new_id, "Key": key, **{lang: text for lang in langs}})
