"""Tree of Inifuss (fork 2 MF run: Rakanishu, then Treehead Woodfist).

Like the Burial Grounds: an entry room with the waypoint and stairs up into a small arena, both on
unused cave levels so their world position is fixed (outdoor levels move every game):
- Arena: "Cave 4" (level 11), fixed at (1500, 1900). It looks and sounds like the Dark Wood
  (level 5) and uses src/maps/inifuss.json: the Tree of Inifuss with the Den of Evil's cave mouth
  beside it. The cave mouth's door is link 5, "cave floor" door type (lvlwarp 2), exactly how the
  Blood Moor links to the Den.
- Entry: "Cave 5" (level 12), fixed at (1500, 2200), a crypt room with stairs up
  (src/maps/inifuss_entry.json), "Crypt Up" (lvlwarp 8). Copies the Crypt's settings. Cave stairs
  crashed the game when linked to a hand-made outdoor map; crypt stairs work (as in the Burial
  Grounds).
It gets the waypoint: Act 1 slot 3, taken from the Dark Wood, also in actinfo.txt (waypoint4).
The tree is only scenery: it can't be clicked, so it never gives the Scroll of Inifuss.
"""

ARENA_ID, ENTRY_ID = "11", "12"      # "Act 1 - Cave 4" / "Act 1 - Cave 5", reused
DARK_WOOD_ID, CRYPT_ID = "5", "18"
ARENA_MAP, ARENA_SIZE = "Act1/Outdoors/darkest_inifuss.ds1", ("24", "24")
ENTRY_MAP, ENTRY_SIZE = "Act1/Crypt/darkest_inifuss_entry.ds1", ("8", "8")
DOOR_LINK = 5                        # the cave mouth's door tile (denent) uses Vis5/Warp5
WARP_WILD_TO_CAVE = "2"              # lvlwarp "Act 1 Wilderness to Cave Floor L"
WARP_CRYPT_UP = "8"                  # lvlwarp "Act 1 Crypt Up"
SLOT = "3"
PIECE_FILES = {"funktree.ds1", "denent.ds1", "circle.ds1", "clover.ds1", "4rock.ds1", "6rock.ds1"}
KEEP = {"Id", "Name", "Layer", "OffsetX", "OffsetY"}
NO_MONSTERS = ("MonDen", "MonDen(N)", "MonDen(H)", "MonUMin", "MonUMax", "MonUMin(N)", "MonUMax(N)", "MonUMin(H)", "MonUMax(H)")


def copy_row(levels, dst, src):
    for col in levels.columns:
        if col not in KEEP:
            dst[col] = src[col]
    dst["__width__"] = max(dst.get("__width__", 0), src.get("__width__", 0))


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
    row["Waypoint"] = "255"


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
    arena, entry = levels.first("Id", ARENA_ID), levels.first("Id", ENTRY_ID)
    dark_wood = levels.first("Id", DARK_WOOD_ID)
    # The Den row is already patched by 010 (preset, no monsters); copy, then set our own values.
    copy_row(levels, arena, dark_wood)
    copy_row(levels, entry, levels.first("Id", CRYPT_ID))
    preset(arena, ARENA_SIZE)
    preset(entry, ENTRY_SIZE)
    entry["LevelName"] = arena["LevelName"]  # both show as the Dark Wood
    entry["LevelWarp"] = arena["LevelWarp"]
    entry["LevelEntry"] = arena["LevelEntry"]

    for row in levels.rows:                  # nothing else leads in or out
        if row is arena or row is entry:
            continue
        for k in range(8):
            if row.get(f"Vis{k}") in (ARENA_ID, ENTRY_ID):
                row[f"Vis{k}"], row[f"Warp{k}"] = "0", "-1"
    arena[f"Vis{DOOR_LINK}"], arena[f"Warp{DOOR_LINK}"] = ENTRY_ID, WARP_WILD_TO_CAVE
    entry["Vis0"], entry["Warp0"] = ARENA_ID, WARP_CRYPT_UP

    old = levels.first("Waypoint", SLOT)     # the Dark Wood gives up its slot
    old["Waypoint"] = "255"
    entry["Waypoint"] = SLOT
    t.table("actinfo").first("act", "1")["waypoint4"] = entry["Name"]   # 1-based: slot 3

    prest = t.table("lvlprest")
    prest_row(prest, "Act 1 - Inifus", "Act 1 - Darkest Diablo Inifuss", ARENA_ID, ARENA_SIZE, ARENA_MAP)
    # The arena mixes outdoor pieces with the cave mouth: load every tile set they use (the
    # union of the Dt1Mask of each piece's own lvlprest row).
    mask = 0
    for r in prest.rows:
        files = {r.get(f"File{k}", "").lower().rsplit("/", 1)[-1] for k in range(1, 7)}
        if files & PIECE_FILES and r["Dt1Mask"].isdigit():
            mask |= int(r["Dt1Mask"])
    prest.rows[-1]["Dt1Mask"] = str(mask)
    prest.rows[-1]["Scan"] = "1"            # scan the map for warp tiles (the vanilla tree has none)
    prest_row(prest, "Act 1 - Crypt Prev E", "Act 1 - Darkest Diablo Inifuss Entry", ENTRY_ID, ENTRY_SIZE, ENTRY_MAP)

    tree = t.table("objects").first("Class", "InifussTree")   # scenery only: no Scroll of Inifuss
    tree["OperateFn"] = "0"
    for k in range(8):
        tree[f"Selectable{k}"] = "0"
