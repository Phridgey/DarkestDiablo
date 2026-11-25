"""Throwing from a potion slot (docs/potions.md): the plugin can't launch a missile, so it spawns an
invisible helper at the hero that throws one at the nearest enemy.

Each helper ("dd_pot_<item code>") is a copy of the game's firebolt trap, which is never seen, never
counted and can't be hit: it shoots its A1 missile at an enemy in reach ("Trap-Missile": aip1 reach,
aip2 how often, aip3 delay). On the heroes' side (Align 1) its enemies are monsters. The missile is the
potion's own, with its own damage. The level and attack rating only make it hit.

Runs late, like 994_den_defense.py: new rows go after every row whose number the plugin knows.
"""

REACH = 25
MODEL = "trap-firebolt"


def patch(t):
    missiles = {r["*ID"]: r["Missile"] for r in t.table("missiles").rows if r.get("*ID")}
    stats = t.table("monstats")
    models = t.data("hd/character/monsters.json") if t.excel.name == "excel" else None
    for row in [r for r in t.table("weapons").rows if r.get("type") == "tpot"]:
        name = "dd_pot_" + row["code"]
        missile = missiles.get(row["missiletype"], "")
        if "potion" not in missile and "poition" not in missile:  # the game's own spelling, once
            raise RuntimeError(f"weapons: {row['code']} throws {missile!r}; has the game changed?")
        stats.add_copy("Id", MODEL, Id=name, BaseId=name, NextInClass="", Align="1", Level="40", noRatio="1",
                       aip1=str(REACH), aip2="1", aip3="1", MissA1=missile, A1TH="5000", minHP="1", maxHP="1")
        if models is not None:
            models[name] = models[MODEL]
