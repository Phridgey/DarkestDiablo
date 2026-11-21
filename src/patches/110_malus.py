"""Horadric Malus (fork 2 XP grind): a cut-down Barracks (level 28). A large hall where waves of
the Barracks' own monsters come, worth 40% more experience; its door leads into the Smith's forge,
where the Smith guards the Malus as in the game. The stop counts as cleared when the Smith dies
(the plugin). Picking up the Malus is Charsi's quest, as in the game (her imbue).

- Map: src/maps/malus.json (two Barracks rooms opened into one hall, then the forge), a fixed map
  at a fixed spot (the real Barracks is placed next to the Outer Cloister, which we don't use).
- The level spawns no monsters of its own; the Smith is the forge's own (his spot in the map).
- Nothing leads in or out: reached by the route map, left by town portal.
"""

ARENA_ID = "28"                     # "Act 1 - Barracks"
MAP_FILE = "Act1/Barracks/darkest_malus.ds1"
SIZE = ("30", "14")                 # map width - 1, height - 1 (3 rooms of 10x14 tiles)
OFFSET = ("5500", "1000")           # a free spot in Act I's world
NO_MONSTERS = ("MonDen", "MonDen(N)", "MonDen(H)", "MonUMin", "MonUMax", "MonUMin(N)", "MonUMax(N)", "MonUMin(H)", "MonUMax(H)")
WAVE_TYPES = ["corruptrogue4", "goatman5", "fallenshaman3", "sk_archer3", "skmage_fire2", "fallen4"]
XP_BONUS = 1.40                     # +40% experience from every kill


def patch(t):
    levels = t.table("levels")
    barracks = levels.first("Id", ARENA_ID)
    barracks["DrlgType"] = "2"
    barracks["OffsetX"], barracks["OffsetY"] = OFFSET
    for col in ("SizeX", "SizeX(N)", "SizeX(H)"):
        barracks[col] = SIZE[0]
    for col in ("SizeY", "SizeY(N)", "SizeY(H)"):
        barracks[col] = SIZE[1]
    for col in NO_MONSTERS:
        barracks[col] = "0"
    for row in levels.rows:  # nothing leads in or out
        for k in range(8):
            if row is barracks or row.get(f"Vis{k}") == ARENA_ID:
                row[f"Vis{k}"], row[f"Warp{k}"] = "0", "-1"

    prest = t.table("lvlprest")
    for other in prest.rows:
        if other.get("LevelId") == ARENA_ID:
            other["LevelId"] = "0"
    row = dict(prest.first("Name", "Act 1 - Barracks Forge W"))
    row["Name"] = "Act 1 - Darkest Diablo Malus"
    row["Def"] = str(max(int(r["Def"]) for r in prest.rows if r["Def"].isdigit()) + 1)
    row["LevelId"] = ARENA_ID
    row["SizeX"], row["SizeY"] = SIZE
    row["Files"] = "1"
    row["File1"] = MAP_FILE
    for k in range(2, 7):
        row[f"File{k}"] = "0"
    prest.rows.append(row)

    stats = t.table("monstats")
    models = t.data("hd/character/monsters.json")
    for source in WAVE_TYPES:
        new_id = f"dd_malus_{source}"
        copy = stats.add_copy("Id", source, Id=new_id, NextInClass="")
        if copy.get("minion1") in WAVE_TYPES:
            copy["minion1"] = f"dd_malus_{copy['minion1']}"
        for col in ("Exp", "Exp(N)", "Exp(H)"):
            if copy.get(col, "").strip().isdigit():
                copy[col] = str(round(int(copy[col]) * XP_BONUS))
        models[new_id] = models[source]  # without this the monster is invisible
