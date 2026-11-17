"""Den of Evil: a fixed hand-made map instead of the random maze.

The map is composed from the game's own cave rooms by tools/compose_map.py from
src/maps/den_of_evil.json (one room: the Den special room with Corpsefire).

The arena is NOT the real Den (levels.txt Id 8): killing everything there completes the Den
quest, and Akara then offers a free respec, which the design doesn't allow. So the arena is a
spare cave level ("Cave 2 Treasure", Id 13, fixed at 1500, 2500) that copies the Den's look and
name. Corpsefire always drops a Book of Skill instead (the quest's skill point). The real Den
can't be reached any more.
"""

ARENA_ID = "13"               # "Act 1 - Cave 2 Treasure", reused
REAL_DEN_ID = "8"
MAP_FILE = "Act1/Caves/darkest_den.ds1"
SIZE_X, SIZE_Y = "24", "24"  # map width - 1, height - 1 (one 24-tile room)
KEEP = {"Id", "Name", "Layer", "OffsetX", "OffsetY"}


def patch(t):
    levels = t.table("levels")
    real_den = levels.first("Id", REAL_DEN_ID)
    den = levels.first("Id", ARENA_ID)
    for col in levels.columns:              # look, sound and name of the Den
        if col not in KEEP:
            den[col] = real_den[col]
    den["__width__"] = max(den.get("__width__", 0), real_den.get("__width__", 0))

    den["DrlgType"] = "2"  # preset map instead of maze
    for col in ("SizeX", "SizeX(N)", "SizeX(H)"):
        den[col] = SIZE_X
    for col in ("SizeY", "SizeY(N)", "SizeY(H)"):
        den[col] = SIZE_Y

    # Arena: no random monsters. The plugin spawns the waves and Corpsefire.
    for col in ("MonDen", "MonDen(N)", "MonDen(H)", "MonUMin", "MonUMax", "MonUMin(N)", "MonUMax(N)", "MonUMin(H)", "MonUMax(H)"):
        den[col] = "0"

    # Players reach every location by waypoint. The Den's waypoint (in the map, by the entrance)
    # takes Act 1 waypoint slot 1, which vanilla gives to the Cold Plains.
    den["Waypoint"] = "1"
    levels.first("Id", "3")["Waypoint"] = "255"  # Cold Plains: no waypoint

    # No stairs anywhere: nothing leads into the arena or the real Den, and neither leads out.
    for row in levels.rows:
        for k in range(8):
            if row is den or row is real_den or row.get(f"Vis{k}") in (ARENA_ID, REAL_DEN_ID):
                row[f"Vis{k}"], row[f"Warp{k}"] = "0", "-1"

    # D2R 3.x builds each act's waypoint menu from actinfo.txt (waypoint1-9, by level name).
    # Slot 2 was Cold Plains; point it at the arena so the Den shows in the menu.
    act1 = t.table("actinfo").first("act", "1")
    act1["waypoint2"] = den["Name"]

    prest = t.table("lvlprest")
    template = prest.first("Name", "Act 1 - Cave Den Of Evil N")
    row = dict(template)
    row["Name"] = "Act 1 - Darkest Diablo Den"
    row["Def"] = str(max(int(r["Def"]) for r in prest.rows if r["Def"].isdigit()) + 1)
    row["LevelId"] = ARENA_ID
    row["SizeX"], row["SizeY"] = SIZE_X, SIZE_Y
    row["Files"] = "1"
    row["File1"] = MAP_FILE
    prest.rows.append(row)

    # Corpsefire always drops a Book of Skill ("ass") on top of his normal loot. Negative Picks
    # drop every listed item that many times: one more pick for the book.
    tcs = t.table("treasureclassex")
    corpsefire = t.table("superuniques").first("Superunique", "Corpsefire")
    for col in ("TC", "TC(N)", "TC(H)"):
        base = tcs.first("Treasure Class", corpsefire[col])
        row = dict(base)
        row["Treasure Class"] = f"Darkest {corpsefire[col]} Corpsefire"
        row["Picks"] = str(int(base["Picks"]) - 1)
        free = next(k for k in range(1, 11) if not base.get(f"Item{k}"))
        row[f"Item{free}"], row[f"Prob{free}"] = "ass", "1"
        tcs.rows.append(row)
        corpsefire[col] = row["Treasure Class"]
