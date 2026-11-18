"""Den of Evil: a fixed hand-made map instead of the random maze.

The map is composed from the game's own cave rooms by tools/compose_map.py from
src/maps/den_of_evil.json: 2x2 open cave rooms (48x48 tiles), joined the way the Pit joins them.

The arena is NOT the real Den (levels.txt Id 8): killing everything there completes the Den
quest, and Akara then offers a free respec, which the design doesn't allow. So the arena is a
spare cave level ("Cave 2 Treasure", Id 13, fixed at 1500, 2500) that copies the Den's look and
name. Corpsefire always drops a Book of Skill instead (the quest's skill point). The real Den
can't be reached any more.
"""

ARENA_ID = "13"               # "Act 1 - Cave 2 Treasure", reused
REAL_DEN_ID = "8"
MAP_FILE = "Act1/Caves/darkest_den.ds1"
SIZE_X, SIZE_Y = "48", "48"  # map width - 1, height - 1 (2x2 rooms of 24 tiles)
KEEP = {"Id", "Name", "Layer", "OffsetX", "OffsetY"}
CORPSEFIRE_TYPE = "dd_corpsefire"  # his own monstats row (see the end of patch())
CORPSEFIRE_CLASS = 751             # its row number: the first one after the game's 751 monsters


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
    template = prest.first("Name", "Act 1 - Cave Den Of Evil N")
    row = dict(template)
    row["Name"] = "Act 1 - Darkest Diablo Den"
    row["Def"] = str(max(int(r["Def"]) for r in prest.rows if r["Def"].isdigit()) + 1)
    row["LevelId"] = ARENA_ID
    row["SizeX"], row["SizeY"] = SIZE_X, SIZE_Y
    row["Files"] = "1"
    row["File1"] = MAP_FILE
    prest.rows.append(row)

    # The Book of Skill is Radament's quest item (Act II quest 15, "questdiffcheck"): the game only
    # lets it drop while that quest is open, so in Act I it silently dropped nothing. Without the
    # quest tie it drops wherever a table names it; using it still gives the skill point (pSpell).
    book = t.table("misc").first("code", "ass")
    book["quest"], book["questdiffcheck"] = "0", "0"

    # Corpsefire always drops a Book of Skill ("ass") on top of his normal loot. Negative Picks
    # drop every listed item that many times: one more pick for the book, listed first.
    tcs = t.table("treasureclassex")
    corpsefire = t.table("superuniques").first("Superunique", "Corpsefire")
    for col in ("TC", "TC(N)", "TC(H)"):
        base = tcs.first("Treasure Class", corpsefire[col])
        row = dict(base)
        row["Treasure Class"] = f"Darkest {corpsefire[col]} Corpsefire"
        row["Picks"] = str(int(base["Picks"]) - 1)
        put_first(row, "ass", "1")  # first, so the 6-item drop cap can't cut it
        tcs.rows.append(row)
        corpsefire[col] = row["Treasure Class"]

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


def put_first(row, item, prob):
    """Insert a loot table entry at the top, moving the others down. A monster drops at most 6
    items per death, and with negative Picks the entries drop in order: an entry listed last
    after 6 items' worth (2 item picks + 2 picks of 2 potions) never drops at all."""
    entries = [(row[f"Item{k}"], row[f"Prob{k}"]) for k in range(1, 11) if row.get(f"Item{k}")]
    entries.insert(0, (item, prob))
    if len(entries) > 10:
        raise RuntimeError(f"{row['Treasure Class']}: more than 10 entries")
    for k in range(1, 11):
        row[f"Item{k}"], row[f"Prob{k}"] = entries[k - 1] if k <= len(entries) else ("", "")
