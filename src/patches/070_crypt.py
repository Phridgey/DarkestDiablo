"""Crypt (fork 1 XP grind): the Mausoleum's round crypt hall again, with waves of the Crypt's
undead that give 40% more experience. Drops follow the game's normal rules (the Crypt monsters'
own loot tables), unlike the Mausoleum's crafting loot (docs/design.md, "Drop and experience
numbers").

- Arena: "Cave 4 Treasure" (level 15), fixed at (1500, 2748) like the other arenas, with the
  Crypt's look, sound and name (level 18). Map: src/maps/crypt.json (a copy of the Mausoleum's).
- The player arrives by waypoint at the hall's west edge: Act 1 waypoint slot 5 (actinfo
  waypoint6), taken from the Outer Cloister's courtyard (level 27).
- Its monsters are copies of the Crypt's undead ("dd_xp_<id>") whose experience is raised by
  XP_BONUS; everything else about them (loot included) is the original's.
"""

ARENA_ID = "15"                     # "Act 1 - Cave 4 Treasure", reused
CRYPT_ID = "18"                     # "Act 1 - Crypt 1 A", the Crypt
MAP_FILE = "Act1/Crypt/darkest_crypt.ds1"
SIZE = ("32", "32")                 # map width - 1, height - 1 (as the Mausoleum)
SLOT = "5"
KEEP = {"Id", "Name", "Layer", "OffsetX", "OffsetY"}
NO_MONSTERS = ("MonDen", "MonDen(N)", "MonDen(H)", "MonUMin", "MonUMax", "MonUMin(N)", "MonUMax(N)", "MonUMin(H)", "MonUMax(H)")
WAVE_TYPES = ["skeleton1", "zombie2", "wraith1", "sk_archer1"]
XP_BONUS = 1.40                     # +40% experience from every kill


def patch(t):
    levels = t.table("levels")
    arena, crypt = levels.first("Id", ARENA_ID), levels.first("Id", CRYPT_ID)
    for col in levels.columns:
        if col not in KEEP:
            arena[col] = crypt[col]
    arena["__width__"] = max(arena.get("__width__", 0), crypt.get("__width__", 0))
    arena["DrlgType"] = "2"
    for col in ("SizeX", "SizeX(N)", "SizeX(H)"):
        arena[col] = SIZE[0]
    for col in ("SizeY", "SizeY(N)", "SizeY(H)"):
        arena[col] = SIZE[1]
    for col in NO_MONSTERS:
        arena[col] = "0"
    for row in levels.rows:  # nothing leads in or out
        for k in range(8):
            if row is arena or row.get(f"Vis{k}") == ARENA_ID:
                row[f"Vis{k}"], row[f"Warp{k}"] = "0", "-1"

    old = levels.first("Waypoint", SLOT)  # the courtyard gives up its slot
    old["Waypoint"] = "255"
    arena["Waypoint"] = SLOT
    t.table("actinfo").first("act", "1")["waypoint6"] = arena["Name"]  # 1-based: slot 5

    prest = t.table("lvlprest")
    for other in prest.rows:  # the level's vanilla preset (a treasure cave) would load instead
        if other.get("LevelId") == ARENA_ID:
            other["LevelId"] = "0"
    row = dict(prest.first("Name", "Act 1 - Crypt NSEW"))
    row["Name"] = "Act 1 - Darkest Diablo Crypt"
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
        new_id = f"dd_xp_{source}"
        row = stats.add_copy("Id", source, Id=new_id, NextInClass="")
        for col in ("Exp", "Exp(N)", "Exp(H)"):
            if row.get(col, "").strip().isdigit():
                row[col] = str(round(int(row[col]) * XP_BONUS))
        models[new_id] = models[source]  # without this the monster is invisible
