"""Burial Grounds (Blood Raven): a one-room arena reached by waypoint.

- Turns the Graveyard level into a stand-alone preset map (src/maps/burial_grounds.json),
  at its own fixed place in the act, with no random monsters and no doors to the crypts.
- Gives it a waypoint: Act 1 slot 2 (taken from Stony Field), also in the waypoint menu (actinfo.txt).
"""

LEVEL_ID = "17"                      # levels.txt Id of "Act 1 - Graveyard" (Burial Grounds)
MAP_FILE = "Act1/Graveyard/darkest_burial.ds1"
SIZE_X, SIZE_Y = "24", "32"          # map width - 1, height - 1
OFFSET_X, OFFSET_Y = "1550", "1000"  # world tile position; must match the plugin's Arenas table
SLOT = "2"


def patch(t):
    levels = t.table("levels")
    lvl = levels.first("Id", LEVEL_ID)
    lvl["DrlgType"] = "2"
    for col in ("SizeX", "SizeX(N)", "SizeX(H)"):
        lvl[col] = SIZE_X
    for col in ("SizeY", "SizeY(N)", "SizeY(H)"):
        lvl[col] = SIZE_Y
    lvl["OffsetX"], lvl["OffsetY"] = OFFSET_X, OFFSET_Y
    for col in ("MonDen", "MonDen(N)", "MonDen(H)", "MonUMin", "MonUMax", "MonUMin(N)", "MonUMax(N)", "MonUMin(H)", "MonUMax(H)"):
        lvl[col] = "0"
    for k in range(8):                       # no doors out (crypt, mausoleum, Cold Plains)
        lvl[f"Vis{k}"], lvl[f"Warp{k}"] = "0", "-1"
    for row in levels.rows:                  # and nothing leads in except the waypoint
        if row is lvl:
            continue
        for k in range(8):
            if row.get(f"Vis{k}") == LEVEL_ID:
                row[f"Vis{k}"], row[f"Warp{k}"] = "0", "-1"

    old = levels.first("Waypoint", SLOT)     # Stony Field gives up its slot
    old["Waypoint"] = "255"
    lvl["Waypoint"] = SLOT
    act1 = t.table("actinfo").first("act", "1")
    act1["waypoint3"] = lvl["Name"]          # actinfo columns are 1-based: slot 2 = waypoint3

    prest = t.table("lvlprest")
    template = prest.first("Name", "Act 1 - Graveyard")
    row = dict(template)
    row["Name"] = "Act 1 - Darkest Diablo Burial Grounds"
    row["Def"] = str(max(int(r["Def"]) for r in prest.rows if r["Def"].isdigit()) + 1)
    row["LevelId"] = LEVEL_ID
    row["SizeX"], row["SizeY"] = SIZE_X, SIZE_Y
    row["Files"] = "1"
    row["File1"] = MAP_FILE
    for k in range(2, 7):
        if f"File{k}" in row:
            row[f"File{k}"] = "0"
    prest.rows.append(row)
