"""Throwing a potion from its slot (docs/potions.md): one skill per throwing potion, cast for the hero by
the plugin (potions.cpp). It is a plain missile skill made from Fire Bolt: the potion's own missile
toward the cursor, its own damage (missiles.txt), no mana. It is a right-button skill,
because the game may refuse to cast one that is neither; so it also shows in the skill list once the
hero has it, where using it takes no charge (docs/potions.md). A short cooldown keeps one press one throw.

The base tables have fewer skills than global/excel, so they get placeholder rows first: the same skill
has the same number in both. The plugin reads the numbers from loot_data.txt ("throwskill").
"""

SOURCE = "Fire Bolt"
COOLDOWN = "10"  # frames, 25 a second
CLEAR = ("ELen", "EMin", "EMax", "EType", "EDmgSymPerCalc", "Param8", "*Param8 Description", "aitype", "attackrank", "ItemEffect", "ItemCastSound",
         "castoverlay", "cost mult", "cost add") + tuple(f"{c}Lev{i}" for c in ("EMin", "EMax") for i in range(1, 6))


def patch(t):
    skills = t.table("skills")
    if t.excel.name != "excel":
        main = type(t)()  # global/excel, read fresh: how many rows the game has there
        wanted = len(main.table("skills").rows)
        last = skills.rows[-1]["skill"]
        for k in range(wanted - len(skills.rows)):
            skills.add_copy("skill", last, skill=f"dd placeholder {k}", skilldesc="", charclass="")
        if len(skills.rows) != wanted:
            raise RuntimeError("skills: the base copy has more rows than global/excel")
    missiles = {r["*ID"]: r["Missile"] for r in t.table("missiles").rows if r.get("*ID")}
    for row in [r for r in t.table("weapons").rows if r.get("type") == "tpot"]:
        missile = missiles[row["missiletype"]]
        number = len(skills.rows)
        blank = {c: "" for c in CLEAR if c in skills.columns}
        skills.add_copy("skill", SOURCE, **{**blank, "skill": "ddthrow" + row["code"], "*Id": str(number), "charclass": "",
                        "skilldesc": "throw", "srvmissile": missile, "cltmissile": missile, "stsound": "weapon_throw_1",
                        "reqlevel": "1", "maxlvl": "1", "reqskill1": "", "skpoints": "",
                        "mana": "0", "minmana": "0", "lvlmana": "0", "localdelay": COOLDOWN,
                        "leftskill": "0", "rightskill": "1"})
        if t.excel.name == "excel":
            t.loot_lines.append(f"throwskill {row['code']} {number}")
