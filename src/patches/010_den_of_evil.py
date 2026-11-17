"""Den of Evil: a fixed hand-made map instead of the random maze.

The map is composed from the game's own cave rooms by tools/compose_map.py from
src/maps/den_of_evil.json (a straight tunnel ending in the Den special room with Corpsefire).
This patch turns the Den (levels.txt Id 8) into a preset level that loads that map.
"""

DEN_LEVEL_ID = "8"
MAP_FILE = "Act1/Caves/mephisto_den.ds1"
SIZE_X, SIZE_Y = "24", "96"  # map width - 1, height - 1 (1 x 4 rooms of 24 tiles)


def patch(t):
    levels = t.table("levels")
    den = levels.first("Id", DEN_LEVEL_ID)
    den["DrlgType"] = "2"  # preset map instead of maze
    for col in ("SizeX", "SizeX(N)", "SizeX(H)"):
        den[col] = SIZE_X
    for col in ("SizeY", "SizeY(N)", "SizeY(H)"):
        den[col] = SIZE_Y

    prest = t.table("lvlprest")
    template = prest.first("Name", "Act 1 - Cave Den Of Evil N")
    row = dict(template)
    row["Name"] = "Act 1 - Mephistorghast Den"
    row["Def"] = str(max(int(r["Def"]) for r in prest.rows if r["Def"].isdigit()) + 1)
    row["LevelId"] = DEN_LEVEL_ID
    row["SizeX"], row["SizeY"] = SIZE_X, SIZE_Y
    row["Files"] = "1"
    row["File1"] = MAP_FILE
    prest.rows.append(row)
