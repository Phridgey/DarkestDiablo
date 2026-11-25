"""Throwing a potion from its slot (docs/potions.md): one skill, "Throw Potion", that every hero has from
the start, the way every hero has Throw and Unsummon (a free "Skill N" slot in charstats.txt).

It is a plain missile skill made from Fire Bolt: a missile toward the cursor, no mana. By itself it sends
off a missile that is invisible and does nothing, so using it without the potion slot throws nothing. The
plugin (potions.cpp) puts the potion's own missile, with its own damage (missiles.txt), in the skill's row
for the moment of a throw. It is a right-button skill, so the player can also put it on a skill key.

The base tables have fewer skills than global/excel, so they get placeholder rows first: the skill has
the same number in both. The plugin reads the numbers from loot_data.txt ("throwskill", "throwmissile",
"throwref": skills whose missile is known, to find the missile columns by).
"""
import json
from pathlib import Path

NAME = "DD Throw Potion"
DESC = "dd throw potion"
SOURCE = "Fire Bolt"
NOTHING = "leapknockback"  # no picture, no damage, 2 frames
START = "oilpotion"  # what it holds until the plugin changes it (0.27.2 test: a real potion)
REFERENCES = ("Fire Bolt", "Ice Bolt", "Glacial Spike", "Holy Bolt", "Lightning", "VampireFireball", "Bone Spear")
STRINGS = {
    "skillname_ddthrow": "Throw Potion",
    "skillsd_ddthrow": "throws a potion slot's potion",
    "skillld_ddthrow": "Throws the potion of a potion slot\\nUse the slot's own key",
}


def _new_string_ids(count):
    folder = Path(__file__).resolve().parents[2] / "vanilla" / "data" / "local" / "lng" / "strings"
    top = 0
    for f in folder.glob("*.json"):
        try:
            top = max([top] + [e.get("id", 0) for e in json.loads(f.read_text(encoding="utf-8-sig"))])
        except (ValueError, AttributeError):
            pass
    start = max(top + 1, 95000)
    return range(start, start + count)


def patch(t):
    skills = t.table("skills")
    desc = t.table("skilldesc")
    for table, column, name in ((skills, "skill", NAME), (desc, "skilldesc", DESC)):
        if any(r[column].lower() == name.lower() for r in table.rows):
            raise RuntimeError(f"{column} {name!r} already exists; a repeated name crashes the game")
    main = t.excel.name == "excel"
    if not main:
        wanted = len(type(t)().table("skills").rows)  # global/excel, read fresh
        last = skills.rows[-1]["skill"]
        for k in range(wanted - len(skills.rows)):
            skills.add_copy("skill", last, skill=f"dd placeholder {k}", skilldesc="", charclass="", **{"*Id": str(len(skills.rows))})
        if len(skills.rows) != wanted:
            raise RuntimeError("skills: the base copy has more rows than global/excel")

    missiles = {r["Missile"].lower(): r for r in t.table("missiles").rows if r.get("*ID")}
    by_id = {r["*ID"]: r["Missile"] for r in missiles.values()}
    number = len(skills.rows)
    skills.add_copy("skill", SOURCE, **{"skill": NAME, "*Id": str(number), "charclass": "", "skilldesc": DESC,
                    "srvmissile": START, "cltmissile": START, "stsound": "weapon_throw_1",
                    "reqlevel": "1", "maxlvl": "1", "reqskill1": "", "skpoints": "",
                    "mana": "0", "minmana": "0", "lvlmana": "0",
                    "leftskill": "0", "rightskill": "1", "ContinueCastUnselected": "1"})

    desc.add_copy("skilldesc", "throw", **{"skilldesc": DESC, "str name": "skillname_ddthrow", "str short": "skillsd_ddthrow",
                  "str long": "skillld_ddthrow", "str alt": "skillname_ddthrow"})
    for column in desc.columns:  # no damage lines
        if column.startswith(("desc", "dsc2", "dsc3", "ddam")):
            desc.rows[-1][column] = ""

    given = 0
    for row in t.table("charstats").rows:
        if not row.get("Skill 1"):
            continue  # the "Expansion" divider
        free = next((s for s in (f"Skill {i}" for i in range(1, 11)) if not row.get(s)), None)
        if free is None:
            raise RuntimeError(f"charstats: {row['class']} has no free skill slot")
        row[free] = NAME
        given += 1
    if given < 7:
        raise RuntimeError(f"charstats: only {given} classes found")

    if not main:
        return
    t.loot_lines.append(f"throwskill skill {number}")
    t.loot_lines.append(f"throwmissile none {missiles[NOTHING]['*ID']}")
    thrown = [r for r in t.table("weapons").rows if r.get("type") == "tpot"]
    for row in thrown:
        if row["missiletype"] not in by_id:
            raise RuntimeError(f"{row['code']}: no missile {row['missiletype']}")
        t.loot_lines.append(f"throwmissile {row['code']} {row['missiletype']}")
    if len(thrown) != 6:
        raise RuntimeError(f"{len(thrown)} throwing potions; has the game changed?")
    for name in REFERENCES:
        row = skills.first("skill", name)
        if row["srvmissile"].lower() != row["cltmissile"].lower():
            raise RuntimeError(f"{name}: its two missiles differ; pick another reference")
        t.loot_lines.append(f"throwref {row['*Id']} {missiles[row['srvmissile'].lower()]['*ID']}")

    strings = t.data("local/lng/strings/skills.json")
    langs = [k for k in strings[0] if k not in ("id", "Key")]
    for new_id, (key, text) in zip(_new_string_ids(len(STRINGS)), STRINGS.items()):
        strings.append({"id": new_id, "Key": key, **{lang: text.replace("\\n", "\n") for lang in langs}})
