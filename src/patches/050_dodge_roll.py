"""Dodge Roll: a short, fast roll every class has, modelled on how Path of Exile 2's roll plays.

What we take from PoE2 is only how it behaves (see docs/dodge-roll.md): a short roll of fixed
length toward the cursor, no mana cost and no cooldown, usable in town, small monsters pushed
aside. None of GGG's files, animations or code are used.

How it is built: a new skill that reuses the game's own Leap code (Barbarian), tuned short,
low and fast. Leap moves the hero to a point and knocks back monsters around the landing,
which is the closest thing the game has to a roll. Every class gets it the way every class
gets Throw and Kick: through a free "Skill N" slot in charstats.txt. Put it on the right mouse
button (or a skill hotkey) to use it.

PROTOTYPE: these numbers are a first guess to be tuned in game. Open questions are listed in
docs/dodge-roll.md (animation on non-Barbarians, whether the hero can be hit mid-roll).
"""
import json
from pathlib import Path

NAME = "Dodge Roll"

# Tuning. D2 runs at 25 frames a second.
ROLL_RANGE = 8        # Leap's "Range" (min and max, so every roll is the same length). Leap's own: 8 to 30.
AIRBORNE_FRAMES = 4   # Leap's "Minimum Airborne Frames" (Leap: 8). Fewer = snappier.
ROLL_SPEED = 250      # Leap's "Leap Speed %" (Leap: 175). Higher = faster across the ground.
PUSH_RADIUS = 2       # Knockback radius on landing; PoE2's roll pushes small monsters aside.

STRINGS = {
    "skillname_dodgeroll": "Dodge Roll",
    "skillsd_dodgeroll": "rolls out of danger",
    "skillld_dodgeroll": "Roll a short distance toward the cursor,\npushing small monsters aside",
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
    next_id = max(int(r["*Id"]) for r in skills.rows if r["*Id"].isdigit()) + 1
    skills.add_copy(
        "skill", "Leap",
        **{
            "skill": NAME, "*Id": str(next_id), "charclass": "", "skilldesc": "dodge roll",
            "reqlevel": "1", "maxlvl": "1", "reqskill1": "", "skpoints": "",
            "mana": "0", "minmana": "0", "lvlmana": "0", "localdelay": "", "globaldelay": "",
            "InTown": "1", "leftskill": "0", "rightskill": "1",
            "Param1": str(ROLL_RANGE), "Param2": str(ROLL_RANGE),
            "Param3": str(PUSH_RADIUS), "Param4": "0",
            "Param5": str(AIRBORNE_FRAMES), "Param7": str(ROLL_SPEED),
            "stsoundclass": "",  # no Barbarian grunt
        },
    )

    desc = t.table("skilldesc")
    desc.add_copy(
        "skilldesc", "leap",
        **{
            "skilldesc": "dodge roll", "SkillPage": "0", "SkillRow": "0", "SkillColumn": "0", "ListRow": "0",
            "str name": "skillname_dodgeroll", "str short": "skillsd_dodgeroll",
            "str long": "skillld_dodgeroll", "str alt": "skillname_dodgeroll",
            # Only the cost line; Leap's range and knockback lines don't apply.
            "descline1": "", "desctexta1": "", "desctextb1": "", "desccalca1": "", "desccalcb1": "",
            "descline2": "", "desctexta2": "", "desctextb2": "", "desccalca2": "", "desccalcb2": "",
            "dsc2line1": "", "dsc2texta1": "", "dsc2calca1": "", "dsc2calcb1": "",
        },
    )

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
