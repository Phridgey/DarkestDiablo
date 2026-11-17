"""Burial Grounds (Blood Raven): an entry room with the waypoint, and stairs up into the arena.

The player arrives by waypoint in a small crypt room, preps, and takes the stairs up. They come out
at the graveyard's crypt door, and that starts the waves (the plugin watches for the level change).

Outdoor levels like the real Graveyard (level 17) are placed by the game's outdoor map builder,
somewhere new every game, so the plugin couldn't know where to spawn. Cave levels sit at a fixed
place, so both parts are built on unused cave levels (Cold Plains' caves, not part of the run):
- Arena: "Cave 2" (level 9) takes the Graveyard's look, name, sounds and monsters, and Blizzard's
  graveyard map (src/maps/burial_grounds.json), at its fixed position (1500, 1300).
- Entry: "Cave 3" (level 10), a crypt room with stairs up (src/maps/burial_entry.json), fixed at
  (1500, 1600). It gets the waypoint: Act 1 slot 2 (taken from Stony Field), also in actinfo.txt.
"""

ARENA_ID = "9"                       # levels.txt Id of "Act 1 - Cave 2", reused for the arena
ENTRY_ID = "10"                      # levels.txt Id of "Act 1 - Cave 3", reused for the entry room
CRYPT_ID = "18"                      # levels.txt Id of "Act 1 - Crypt 1 A", copied for the entry
GRAVEYARD_ID = "17"                  # levels.txt Id of "Act 1 - Graveyard" (Burial Grounds), copied from
ARENA_MAP, ARENA_SIZE = "Act1/Graveyard/darkest_burial.ds1", ("24", "32")
ENTRY_MAP, ENTRY_SIZE = "Act1/Crypt/darkest_burial_entry.ds1", ("8", "8")
WARP_GRAVEYARD_DOOR = "6"            # lvlwarp "Act 1 Graveyard to Crypt 1": the graveyard's crypt door
WARP_CRYPT_UP = "8"                  # lvlwarp "Act 1 Crypt Up": the entry room's stairs
SLOT = "2"
# Kept from each cave row: its key, its own layer and its fixed position.
KEEP = {"Id", "Name", "Layer", "OffsetX", "OffsetY"}
NO_MONSTERS = ("MonDen", "MonDen(N)", "MonDen(H)", "MonUMin", "MonUMax", "MonUMin(N)", "MonUMax(N)", "MonUMin(H)", "MonUMax(H)")


def preset(row, size):
    row["DrlgType"] = "2"
    for col in ("SizeX", "SizeX(N)", "SizeX(H)"):
        row[col] = size[0]
    for col in ("SizeY", "SizeY(N)", "SizeY(H)"):
        row[col] = size[1]
    for col in NO_MONSTERS:
        row[col] = "0"
    for k in range(8):
        row[f"Vis{k}"], row[f"Warp{k}"] = "0", "-1"


def prest_row(prest, template_name, name, level_id, size, map_file):
    row = dict(prest.first("Name", template_name))
    row["Name"] = name
    row["Def"] = str(max(int(r["Def"]) for r in prest.rows if r["Def"].isdigit()) + 1)
    row["LevelId"] = level_id
    row["SizeX"], row["SizeY"] = size
    row["Files"] = "1"
    row["File1"] = map_file
    for k in range(2, 7):
        row[f"File{k}"] = "0"
    prest.rows.append(row)


def patch(t):
    levels = t.table("levels")
    arena = levels.first("Id", ARENA_ID)
    entry = levels.first("Id", ENTRY_ID)
    graveyard = levels.first("Id", GRAVEYARD_ID)

    for col in levels.columns:               # the arena looks and sounds like the Graveyard
        if col not in KEEP:
            arena[col] = graveyard[col]
    arena["__width__"] = max(arena.get("__width__", 0), graveyard.get("__width__", 0))
    crypt = levels.first("Id", CRYPT_ID)     # the entry looks like the Crypt (Blizzard pairs its stairs
    for col in levels.columns:               # with the graveyard's crypt door)
        if col not in KEEP:
            entry[col] = crypt[col]
    entry["__width__"] = max(entry.get("__width__", 0), crypt.get("__width__", 0))
    preset(arena, ARENA_SIZE)
    preset(entry, ENTRY_SIZE)                # named Burial Grounds
    entry["LevelName"] = graveyard["LevelName"]
    entry["LevelWarp"] = graveyard["LevelWarp"]
    entry["LevelEntry"] = graveyard["LevelEntry"]

    for row in levels.rows:                  # nothing else leads in or out
        if row is arena or row is entry:
            continue
        for k in range(8):
            if row.get(f"Vis{k}") in (ARENA_ID, ENTRY_ID):
                row[f"Vis{k}"], row[f"Warp{k}"] = "0", "-1"
    # The stairs and the crypt door link the two. The mausoleum door (Vis1) stays closed.
    entry["Vis0"], entry["Warp0"] = ARENA_ID, WARP_CRYPT_UP
    arena["Vis0"], arena["Warp0"] = ENTRY_ID, WARP_GRAVEYARD_DOOR

    old = levels.first("Waypoint", SLOT)     # Stony Field gives up its slot
    old["Waypoint"] = "255"
    entry["Waypoint"] = SLOT
    arena["Waypoint"] = "255"
    act1 = t.table("actinfo").first("act", "1")
    act1["waypoint3"] = entry["Name"]        # actinfo columns are 1-based: slot 2 = waypoint3

    prest = t.table("lvlprest")
    prest_row(prest, "Act 1 - Graveyard", "Act 1 - Darkest Diablo Burial Grounds", ARENA_ID, ARENA_SIZE, ARENA_MAP)
    prest_row(prest, "Act 1 - Crypt Prev E", "Act 1 - Darkest Diablo Burial Entry", ENTRY_ID, ENTRY_SIZE, ENTRY_MAP)
