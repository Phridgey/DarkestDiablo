"""Den of Evil: a fixed hand-made map instead of the random maze.

The map is composed from the game's own cave rooms by tools/compose_map.py from
src/maps/den_of_evil.json (a straight tunnel ending in the Den special room with Corpsefire).
This patch turns the Den (levels.txt Id 8) into a preset level that loads that map.
"""

DEN_LEVEL_ID = "8"
MAP_FILE = "Act1/Caves/darkest_den.ds1"
SIZE_X, SIZE_Y = "24", "24"  # map width - 1, height - 1 (one 24-tile room)


def patch(t):
    levels = t.table("levels")
    den = levels.first("Id", DEN_LEVEL_ID)
    den["DrlgType"] = "2"  # preset map instead of maze
    for col in ("SizeX", "SizeX(N)", "SizeX(H)"):
        den[col] = SIZE_X
    for col in ("SizeY", "SizeY(N)", "SizeY(H)"):
        den[col] = SIZE_Y

    # Players reach every location by waypoint. The Den's waypoint (in the map, by the entrance)
    # takes Act 1 waypoint slot 1, which vanilla gives to the Cold Plains.
    # Arena: no random monsters. The plugin spawns the waves and Corpsefire.
    for col in ("MonDen", "MonDen(N)", "MonDen(H)", "MonUMin", "MonUMax", "MonUMin(N)", "MonUMax(N)", "MonUMin(H)", "MonUMax(H)"):
        den[col] = "0"
    den["Waypoint"] = "1"
    levels.first("Id", "3")["Waypoint"] = "255"  # Cold Plains: no waypoint

    # The waypoint replaces the entrance stairs, so remove the stairs link both ways:
    # the Den's exit to the Blood Moor, and the Blood Moor's cave entrance into the Den.
    den["Vis0"], den["Warp0"] = "0", "-1"
    blood_moor = levels.first("Id", "2")
    for k in range(8):
        if blood_moor[f"Vis{k}"] == DEN_LEVEL_ID:
            blood_moor[f"Vis{k}"], blood_moor[f"Warp{k}"] = "0", "-1"

    # D2R 3.x builds each act's waypoint menu from actinfo.txt (waypoint1-9, by level name).
    # Slot 2 was Cold Plains; point it at the Den so the Den shows in the menu.
    act1 = t.table("actinfo").first("act", "1")
    act1["waypoint2"] = den["Name"]

    prest = t.table("lvlprest")
    template = prest.first("Name", "Act 1 - Cave Den Of Evil N")
    row = dict(template)
    row["Name"] = "Act 1 - Darkest Diablo Den"
    row["Def"] = str(max(int(r["Def"]) for r in prest.rows if r["Def"].isdigit()) + 1)
    row["LevelId"] = DEN_LEVEL_ID
    row["SizeX"], row["SizeY"] = SIZE_X, SIZE_Y
    row["Files"] = "1"
    row["File1"] = MAP_FILE
    prest.rows.append(row)
