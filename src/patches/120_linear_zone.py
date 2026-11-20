"""Linear zone (prototype): a short cave the player crosses from west to east, fighting the
game's own random monster packs on the way, with an elite at the far end. Reached through a red
portal by the town fire (src/plugin/zone.cpp). Think of a much shorter Path of Exile map.

- Level: "Act 1 - Jail 3" (Id 31), a spare level with its own layer and a fixed place
  (3500, 1600), so the plugin knows where every tile is. No Act I stop uses it.
- Look, sound, name and monsters: the Den of Evil's (levels.txt Id 8), as for the Den arena.
- Map: src/maps/linear_zone.json, six of Blizzard's cave rooms in a row (145 x 25 tiles).
- Monsters: unlike the arenas, the game fills the map with its own random packs (Populate = 1 in
  the lvlprest row, MonDen above zero). Den monsters: Fallen, Fallen Shamans and zombies, with
  the odd champion or unique pack.
- Nothing leads in or out; the plugin moves the hero in with "goto" and the boss's death gives a
  Town Portal scroll, as in the arenas.
"""

ZONE_ID = "31"                      # "Act 1 - Jail 3", reused (ZoneLevelId in src/plugin/zone.cpp)
DEN_ID = "8"                        # the real Den of Evil: look, sound, name, monster level
MAP_FILE = "Act1/Caves/darkest_linear.ds1"
SIZE = ("144", "24")                # map width - 1, height - 1 (6 x 1 cave rooms of 24 tiles)
KEEP = {"Id", "Name", "Layer", "OffsetX", "OffsetY"}
MONSTERS = ["fallen1", "fallenshaman1", "zombie1"]  # the custom Den's wave monsters
DENSITY = "900"                     # the Den's maze has 600; the corridor is short, so pack it


def patch(t):
    levels = t.table("levels")
    zone, den = levels.first("Id", ZONE_ID), levels.first("Id", DEN_ID)
    for col in levels.columns:
        if col not in KEEP:
            zone[col] = den[col]
    zone["__width__"] = max(zone.get("__width__", 0), den.get("__width__", 0))
    zone["Quest"] = "0"             # never count kills here for the Den quest
    zone["Waypoint"] = "255"
    zone["DrlgType"] = "2"          # preset map instead of a maze
    for col in ("SizeX", "SizeX(N)", "SizeX(H)"):
        zone[col] = SIZE[0]
    for col in ("SizeY", "SizeY(N)", "SizeY(H)"):
        zone[col] = SIZE[1]
    for col in ("MonDen", "MonDen(N)", "MonDen(H)"):
        zone[col] = DENSITY
    for col in ("MonUMin", "MonUMin(N)", "MonUMin(H)"):
        zone[col] = "1"
    for col in ("MonUMax", "MonUMax(N)", "MonUMax(H)"):
        zone[col] = "2"
    # Same monsters on every difficulty (the game picks the stronger versions by monster level).
    for prefix in ("mon", "nmon", "umon"):
        for k in range(1, 26):
            if f"{prefix}{k}" in zone:
                zone[f"{prefix}{k}"] = MONSTERS[k - 1] if k <= len(MONSTERS) else ""
    for row in levels.rows:  # nothing leads in or out
        for k in range(8):
            if row is zone or row.get(f"Vis{k}") == ZONE_ID:
                row[f"Vis{k}"], row[f"Warp{k}"] = "0", "-1"

    prest = t.table("lvlprest")
    for other in prest.rows:  # a preset still naming the level would load instead of ours
        if other.get("LevelId") == ZONE_ID:
            other["LevelId"] = "0"
    row = dict(prest.first("Name", "Act 1 - Cave Den Of Evil N"))
    row["Name"] = "Act 1 - Darkest Diablo Linear Zone"
    row["Def"] = str(max(int(r["Def"]) for r in prest.rows if r["Def"].isdigit()) + 1)
    row["LevelId"] = ZONE_ID
    row["SizeX"], row["SizeY"] = SIZE
    row["Populate"] = "1"           # the game's random monster packs (the arenas spawn their own)
    row["Scan"] = "0"               # no exits
    row["Files"] = "1"
    row["File1"] = MAP_FILE
    for k in range(2, 7):
        row[f"File{k}"] = "0"
    prest.rows.append(row)
