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
- From the Paladin's Charge: the look and sound on the hero's
  screen ("clt" functions, start sound), its speed, and it can't be interrupted by hits.
- From the Amazon's Dodge: the pose, a sidestep (since 0.11.69; Charge's run pose didn't work
  with claws).
- Fast, then slow (Sam, 2026-11-18): the dash itself is quick, and the hero is slowed for a short
  time, so a dodge plus its slow never covers ground faster than running. The hero glows green
  like poison while slowed. Leap's code puts no effect on the hero, so the slow comes from the
  skill's start code: Berserk's ("srvstfunc" 39), which gives the caster the skill's
  "aurastate" for "auralencalc" frames. Leap's own start code is replaced, so this may break
  the movement (untested, 0.11.65). Tried before: a Battle Orders-style landing pulse
  (0.11.58 to 0.11.61) never reached the hero who made it.

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
MIN_FRAMES = 8        # Leap's "Minimum Airborne Frames" (Leap's own value): the dash takes 0.32 s.
# No travel advantage (Sam, 2026-11-18): a dodge must never cover ground faster than running.
# Run speed is charstats "RunVelocity" (9 for every class) in the same units as Leap's range
# (sub-tiles a second; the game shows both as yards by multiplying by 2/3). In the time the dash
# takes (8 frames) running covers 9 * 8/25 = 2.9, so the dash gains 8 - 2.9 = 5.1. The hero is
# then slowed by SLOW_PERCENT until running slower has given those 5.1 back:
# 9 * 0.40 * t = 5.1, t = 1.42 s = 36 frames. The slow starts with the dash (see srvstfunc
# below), so it lasts the dash's frames plus those.
RUN_VELOCITY = 9
SLOW_PERCENT = 40
SLOW_FRAMES = MIN_FRAMES + -(-(DISTANCE * 25 - RUN_VELOCITY * MIN_FRAMES) * 100 // (RUN_VELOCITY * SLOW_PERCENT))
# Our own state for the slow, so the hero can be tinted green like poison while it lasts
# (Sam, 2026-11-18). A copy of the game's plain "slowed" state with poison's colours, light
# and green life globe; poison's damage comes from separate stats, so this one does no harm.
SLOW_STATE = "dd dodge slow"
BERSERK_START = str(39)  # skills.txt srvstfunc of Berserk
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
    from_charge = ("monanim", "cltstfunc", "cltdofunc", "cltstopfunc",
                   "stsound", "castoverlay", "cltoverlaya", "cltoverlayb", "interrupt")
    # The pose: the Amazon's Dodge sidestep (Sam, 2026-11-19). Charge's run (a "sequence" of
    # Paladin attack frames) has no frames for claws, so Assassins with claws couldn't dodge.
    dodge = skills.first("skill", "Dodge")
    pose = {"anim": dodge["anim"], "seqtrans": dodge["seqtrans"], "seqnum": ""}
    skills.add_copy("skill", "Leap", **{c: charge[c] for c in from_charge}, **pose, **{
        "skill": NAME, "*Id": str(next_id), "charclass": "", "skilldesc": DESC,
        "reqlevel": "1", "maxlvl": "1", "reqskill1": "", "skpoints": "",
        "mana": "0", "minmana": "0", "lvlmana": "0", "localdelay": str(COOLDOWN_FRAMES), "globaldelay": "",
        "InTown": "1", "leftskill": "0", "rightskill": "1", "stsoundclass": "",
        "Param1": str(DISTANCE), "Param2": str(DISTANCE),
        "Param3": "0", "Param4": "0",  # no knockback on landing
        "Param5": str(MIN_FRAMES), "Param7": str(SPEED), "calc2": str(SPEED),
        "seqinput": str(RECOVERY_FRAME),
        "srvmissilea": "",  # no knockback on landing
        # The slow: Berserk's start code gives the caster aurastate, with these stats, for
        # auralencalc frames. Leap's do code ("srvdofunc" 77) still moves the hero.
        "srvstfunc": BERSERK_START, "aurastate": SLOW_STATE, "auratargetstate": "",
        "auralencalc": str(SLOW_FRAMES), "aurastat1": "velocitypercent",
        "aurastatcalc1": str(-SLOW_PERCENT),
    })
    for i in range(2, 7):  # only the one stat above
        skills.rows[-1][f"aurastat{i}"] = ""
        skills.rows[-1][f"aurastatcalc{i}"] = ""

    try:
        states = t.table("states")
    except FileNotFoundError:
        states = None  # a table copy without its own states.txt uses the main one
    if states is not None:
        if states.find("state", SLOW_STATE):
            raise RuntimeError(f"state {SLOW_STATE!r} already exists; names must be unique")
        state_id = max(int(r["*ID"]) for r in states.rows if r["*ID"].isdigit()) + 1
        poison = states.first("state", "poison")
        states.add_copy("state", "slowed", **{
            "state": SLOW_STATE, "*ID": str(state_id),
            **{c: poison[c] for c in ("green", "colorpri", "colorshift", "light-r", "light-g", "light-b")},
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
