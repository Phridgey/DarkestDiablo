"""Burial Grounds (Blood Raven): a one-room arena reached by waypoint.

Outdoor levels like the real Graveyard (level 17) are placed by the game's outdoor map builder,
somewhere new every game, so the plugin can't know where to spawn waves. Cave levels sit at a
fixed place. So the arena is built on an unused cave level instead: "Cave 2" (level 9, the cave
under Cold Plains, not part of the run) takes the Graveyard's look, name, sounds and monsters,
and Blizzard's graveyard map (src/maps/burial_grounds.json), at its own fixed position.

It gets a waypoint: Act 1 slot 2 (taken from Stony Field), also in the waypoint menu (actinfo.txt).
"""

ARENA_ID = "9"                       # levels.txt Id of "Act 1 - Cave 2", reused for the arena
GRAVEYARD_ID = "17"                  # levels.txt Id of "Act 1 - Graveyard" (Burial Grounds), copied from
MAP_FILE = "Act1/Graveyard/darkest_burial.ds1"
SIZE_X, SIZE_Y = "24", "32"          # map width - 1, height - 1
SLOT = "2"
# Kept from the cave row: its key, its own layer and its fixed position (1500, 1300).
KEEP = {"Id", "Name", "Layer", "OffsetX", "OffsetY"}


def patch(t):
    levels = t.table("levels")
    arena = levels.first("Id", ARENA_ID)
    graveyard = levels.first("Id", GRAVEYARD_ID)
    for col in levels.columns:
        if col not in KEEP:
            arena[col] = graveyard[col]
    arena["__width__"] = max(arena.get("__width__", 0), graveyard.get("__width__", 0))
    arena["DrlgType"] = "2"
    for col in ("SizeX", "SizeX(N)", "SizeX(H)"):
        arena[col] = SIZE_X
    for col in ("SizeY", "SizeY(N)", "SizeY(H)"):
        arena[col] = SIZE_Y
    for col in ("MonDen", "MonDen(N)", "MonDen(H)", "MonUMin", "MonUMax", "MonUMin(N)", "MonUMax(N)", "MonUMin(H)", "MonUMax(H)"):
        arena[col] = "0"
    for k in range(8):                       # no doors out
        arena[f"Vis{k}"], arena[f"Warp{k}"] = "0", "-1"
    for row in levels.rows:                  # and nothing leads in except the waypoint
        if row is arena:
            continue
        for k in range(8):
            if row.get(f"Vis{k}") == ARENA_ID:
                row[f"Vis{k}"], row[f"Warp{k}"] = "0", "-1"

    old = levels.first("Waypoint", SLOT)     # Stony Field gives up its slot
    old["Waypoint"] = "255"
    arena["Waypoint"] = SLOT
    act1 = t.table("actinfo").first("act", "1")
    act1["waypoint3"] = arena["Name"]        # actinfo columns are 1-based: slot 2 = waypoint3

    prest = t.table("lvlprest")
    template = prest.first("Name", "Act 1 - Graveyard")
    row = dict(template)
    row["Name"] = "Act 1 - Darkest Diablo Burial Grounds"
    row["Def"] = str(max(int(r["Def"]) for r in prest.rows if r["Def"].isdigit()) + 1)
    row["LevelId"] = ARENA_ID
    row["SizeX"], row["SizeY"] = SIZE_X, SIZE_Y
    row["Files"] = "1"
    row["File1"] = MAP_FILE
    for k in range(2, 7):
        row[f"File{k}"] = "0"
    prest.rows.append(row)
