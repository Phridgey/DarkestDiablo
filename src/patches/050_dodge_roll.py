"""Dodge Roll: a quick dash every class has, modelled on how Path of Exile 2's roll plays.

What we take from PoE2 is only how it behaves (see docs/dodge-roll.md): a fast move toward the
cursor, no mana cost and no cooldown, usable in town. None of GGG's files, animations or code
are used.

How it is built: the game has no free-form "move here fast" in its tables, only skills whose
movement code is fixed. Sam asked for a dash to the clicked spot instead of Leap's jump, so:

- "Dodge Roll" reuses the Paladin's Charge code: the hero runs at high speed to the target.
  Its pose is the hero's own run, sped up. Changed from Charge: no weapon needed, no damage,
  no need to click a monster (whether Charge accepts plain ground is the first thing to test).
- "Dodge Spin" (test only) reuses Whirlwind's code: it always moves to the clicked spot, but
  the pose is the whirlwind spin. It is the fallback if Charge refuses plain ground. Every class
  but the Barbarian gets it (the Barbarian has one free skill slot, and has Whirlwind anyway).

Every class gets them the way every class gets Throw and Kick: through free "Skill N" slots in
charstats.txt. Put one on the right mouse button (or a skill hotkey) to use it.

PROTOTYPE: these numbers are a first guess to be tuned in game. Open questions are listed in
docs/dodge-roll.md.
"""
import json
from pathlib import Path

DASH = "Dodge Roll"
SPIN = "Dodge Spin"

# Tuning. D2 runs at 25 frames a second.
DASH_SPEED = 300      # Charge's "Velocity %" (Charge: 150). Higher = faster dash.
SPIN_SPEED = 30       # Whirlwind's Param1 (Whirlwind: 30).

STRINGS = {
    "skillname_dodgeroll": "Dodge Roll",
    "skillsd_dodgeroll": "dashes out of danger",
    "skillld_dodgeroll": "Dash to the target spot",
    "skillname_dodgespin": "Dodge Spin",
    "skillsd_dodgespin": "test: spins to a spot",
    "skillld_dodgespin": "Test version: spin to the target spot",
}

# Shared by both: a free movement skill for everyone, with no weapon, cost, damage or cooldown.
COMMON = {
    "charclass": "", "reqlevel": "1", "maxlvl": "1", "reqskill1": "", "skpoints": "",
    "mana": "0", "minmana": "0", "lvlmana": "0", "localdelay": "", "globaldelay": "",
    "InTown": "1", "leftskill": "0", "rightskill": "1",
    "itypea1": "", "itypea2": "", "itypea3": "", "etypea1": "", "etypea2": "",
    "itypeb1": "", "itypeb2": "", "itypeb3": "", "etypeb1": "", "etypeb2": "",
    "TargetableOnly": "", "SearchEnemyXY": "", "SearchEnemyNear": "",
    "SrcDam": "0", "MinDam": "0", "MaxDam": "0", "calc1": "0", "ToHit": "0", "LevToHit": "0",
    "stsound": "", "stsoundclass": "", "attackrank": "0", "durability": "",
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


def _skill_desc(desc, source, name, key):
    desc.add_copy(
        "skilldesc", source,
        **{
            "skilldesc": name, "SkillPage": "0", "SkillRow": "0", "SkillColumn": "0", "ListRow": "0",
            "str name": f"skillname_{key}", "str short": f"skillsd_{key}",
            "str long": f"skillld_{key}", "str alt": f"skillname_{key}",
        },
    )
    row = desc.rows[-1]
    for column in desc.columns:  # no damage, synergy or range lines
        if column.startswith(("desc", "dsc2", "dsc3", "ddam")):
            row[column] = ""


def patch(t):
    skills = t.table("skills")
    next_id = max(int(r["*Id"]) for r in skills.rows if r["*Id"].isdigit()) + 1
    skills.add_copy("skill", "Charge", **COMMON, **{
        "skill": DASH, "*Id": str(next_id), "skilldesc": "dodge roll",
        "Param1": str(DASH_SPEED), "Param3": "0", "Param4": "0", "Param8": "0",
    })
    skills.add_copy("skill", "Whirlwind", **COMMON, **{
        "skill": SPIN, "*Id": str(next_id + 1), "skilldesc": "dodge spin",
        "Param1": str(SPIN_SPEED),
    })

    desc = t.table("skilldesc")
    _skill_desc(desc, "charge", "dodge roll", "dodgeroll")
    _skill_desc(desc, "whirlwind", "dodge spin", "dodgespin")

    # Every class: the first free "Skill N" slots, like Throw and Kick.
    stats = t.table("charstats")
    slots = [f"Skill {i}" for i in range(1, 11)]
    for row in stats.rows:
        if not row.get("Skill 1"):
            continue  # the "Expansion" divider row
        for skill in (DASH, SPIN):
            if skill == SPIN and row["class"] == "Barbarian":
                continue
            free = next((s for s in slots if not row.get(s)), None)
            if free is None:
                raise RuntimeError(f"charstats: {row['class']} has no free skill slot for {skill}")
            row[free] = skill

    strings = t.data("local/lng/strings/skills.json")
    langs = [k for k in strings[0] if k not in ("id", "Key")]
    for new_id, (key, text) in zip(_new_string_ids(len(STRINGS)), STRINGS.items()):
        strings.append({"id": new_id, "Key": key, **{lang: text for lang in langs}})
