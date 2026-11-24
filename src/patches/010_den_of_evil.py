"""Den of Evil: a fixed hand-made map instead of the random maze.

The map is composed from the game's own cave rooms by tools/compose_map.py from
src/maps/den_of_evil.json: the Pit level 2 room (a round cave, 24x24 tiles) with walls along
the edge of its floor, so the automap shows it.

The arena is NOT the real Den (levels.txt Id 8): killing everything there completes the Den
quest, and Akara then offers a free respec, which the design doesn't allow. So the arena is a
spare cave level ("Cave 2 Treasure", Id 13, fixed at 1500, 2500) that copies the Den's look and
name. The plugin gives the quest's skill point when the Den is first cleared. The real Den
can't be reached any more.
"""

ARENA_ID = "13"               # "Act 1 - Cave 2 Treasure", reused
REAL_DEN_ID = "8"
MAP_FILE = "Act1/Caves/darkest_den.ds1"
SIZE_X, SIZE_Y = "24", "24"  # map width - 1, height - 1 (one 24x24 cave room)
KEEP = {"Id", "Name", "Layer", "OffsetX", "OffsetY"}
CORPSEFIRE_TYPE = "dd_corpsefire"  # his own monstats row (see the end of patch())
CORPSEFIRE_CLASS = 751             # its row number: the first one after the game's 751 monsters
DEN_TYPES = ("fallen1", "zombie1", "fallenshaman1")  # the wave monsters (DenWaves in the plugin)
DEN_PREFIX = "dd_den_"
DEN_DEFENSE = 0.45                 # 55% less defense than the game's own


def patch(t):
    levels = t.table("levels")
    real_den = levels.first("Id", REAL_DEN_ID)
    den = levels.first("Id", ARENA_ID)
    for col in levels.columns:              # look, sound and name of the Den
        if col not in KEEP:
            den[col] = real_den[col]
    den["__width__"] = max(den.get("__width__", 0), real_den.get("__width__", 0))
    # Not the Den's quest level: with Quest = 1 the game counts this level's monsters for the
    # Den quest, and since the arena starts empty (the plugin spawns everything) the quest
    # completed after a few kills, giving Akara's skill point on top of Corpsefire's book.
    den["Quest"] = "0"

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
    # Level 13 already has a vanilla preset (the treasure room "Act 1 - Cave Treasure 2", 24x24).
    # The game uses the first preset that names the level, so with it left in place the arena loaded
    # that 24-tile room into a 48-tile level and crashed (BC_VERIFY nSizeX == nSizeTileX).
    for other in prest.rows:
        if other.get("LevelId") == ARENA_ID:
            other["LevelId"] = "0"
    template = prest.first("Name", "Act 1 - Cave Den Of Evil N")
    row = dict(template)
    row["Name"] = "Act 1 - Darkest Diablo Den"
    row["Def"] = str(max(int(r["Def"]) for r in prest.rows if r["Def"].isdigit()) + 1)
    row["LevelId"] = ARENA_ID
    row["SizeX"], row["SizeY"] = SIZE_X, SIZE_Y
    row["Files"] = "1"
    row["File1"] = MAP_FILE
    prest.rows.append(row)

    # The Den's skill point: the plugin gives it when the Den is first cleared ("cheat
    # skillpoints 1"). A dropped Book of Skill could not be used outside Radament's quest.
    # Saves made with 0.9.6-0.9.12 hold Books of Skill written with these two columns at 0. They
    # change how the item is stored in the save, so put back to the game's values the game misreads
    # those books and crashes loading the hero (BC_ASSERT setItemsTbl). Keep them at 0.
    book = t.table("misc").first("code", "ass")
    book["quest"], book["questdiffcheck"] = "0", "0"
    corpsefire = t.table("superuniques").first("Superunique", "Corpsefire")

    # Corpsefire gets a monster type of his own, a copy of the zombie, so the plugin can tell his
    # death apart: the Den is only cleared once he is dead (like Blood Raven at Burial Grounds).
    # His minions stay ordinary zombies (minion1), so they don't count as him. The plugin waits
    # for row number CORPSEFIRE_CLASS (CorpsefireClass in src/plugin/darkestdiablo_plugin.cpp).
    stats = t.table("monstats")
    numbered = [r for r in stats.rows if r.get("*hcIdx", "").strip().isdigit()]  # "Expansion" has no number
    if len(numbered) != CORPSEFIRE_CLASS:
        raise RuntimeError(f"monstats has {len(numbered)} rows, so Corpsefire's type would be row "
                           f"{len(numbered)}, not {CORPSEFIRE_CLASS}. Update CORPSEFIRE_CLASS here and in the plugin.")
    stats.add_copy("Id", "zombie1", Id=CORPSEFIRE_TYPE, NextInClass="", minion1="zombie1",
                   **{"*hcIdx": str(CORPSEFIRE_CLASS)})
    corpsefire["Class"] = CORPSEFIRE_TYPE
    # A superunique spawned by the plugin ("cheat monster super:40") may drop from its monster
    # type's columns rather than his superunique row, so his type (his alone) drops his table too.
    own = stats.first("Id", CORPSEFIRE_TYPE)
    for c in (c for c in stats.columns if c.startswith("TreasureClass")):
        suffix = "TC(N)" if c.endswith("(N)") else "TC(H)" if c.endswith("(H)") else "TC"
        own[c] = corpsefire[suffix]
    # D2R finds a monster's 3D model by its Id in this file; without an entry he is invisible.
    models = t.data("hd/character/monsters.json")
    models[CORPSEFIRE_TYPE] = models["zombie1"]

    # The Den's monsters have 55% less defense (2026-11-23): the plugin spawns copies of their own
    # ("dd_den_<type>"), so the same types elsewhere (Tristram, the Tree of Inifuss) keep theirs.
    for source in DEN_TYPES:
        copy = stats.add_copy("Id", source, Id=DEN_PREFIX + source, NextInClass="")
        models[DEN_PREFIX + source] = models[source]
    for row in [stats.first("Id", DEN_PREFIX + s) for s in DEN_TYPES] + [own]:
        for col in ("minion1", "minion2"):
            if row.get(col) in DEN_TYPES:
                row[col] = DEN_PREFIX + row[col]
        for col in ("AC", "AC(N)", "AC(H)"):
            if row.get(col, "").strip().isdigit():
                row[col] = str(round(int(row[col]) * DEN_DEFENSE))
