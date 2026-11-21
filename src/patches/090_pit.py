"""Pit (fork 2 MF run): the real Pit level 2 (level 16) as an arena on our copy of its room
(src/maps/pit.json, the Den's room), with waves of the Pit's own monsters.

- Like Rakanishu & Treehead: every monster drops as if magic find were 1.4 times as strong (the
  quality ratios of their loot tables, see 035_inifuss_loot.py), on all of them.
- Five packs, each led by a unique (random name and affixes; the plugin spawns them). One of the
  five, picked at random, is the target: it spawns as its type's "_t" copy, and killing it wins.
  The target drops one guaranteed unique or set item (a set where no unique fits), plus its own
  unique monster loot, like Treehead.
- The plugin learns the target types' class numbers from "targetclass" lines in
  d2rloader/loot_data.txt, so it can tell when the target dies.
- Waypoint: Act 1 slot 6 (actinfo waypoint7), taken from Jail level 1 (29).
"""
import importlib.util
from pathlib import Path

ARENA_ID = "16"                     # "Act 1 - Cave 5 Treasure", the real Pit level 2
MAP_FILE = "Act1/Caves/darkest_pit.ds1"
SIZE = ("24", "24")                 # map width - 1, height - 1 (the Pit level 2 room)
SLOT = "6"
NO_MONSTERS = ("MonDen", "MonDen(N)", "MonDen(H)", "MonUMin", "MonUMax", "MonUMin(N)", "MonUMax(N)", "MonUMin(H)", "MonUMax(H)")
WAVE_TYPES = ["skeleton3", "fallen3", "corruptrogue3", "cr_archer3"]
# The guaranteed pick only offers uniques and sets of the area's level or lower. With the usual
# +3 for unique monsters, a target spawned at the area's level picked a unique too high for it,
# and the game gave a rare instead (a Pit run on 2026-11-19 got no unique or set).
UNIQUE_LEVEL_BONUS = 0

_spec = importlib.util.spec_from_file_location("inifuss_loot", Path(__file__).with_name("035_inifuss_loot.py"))
inifuss = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(inifuss)


def patch(t):
    levels = t.table("levels")
    pit = levels.first("Id", ARENA_ID)
    pit["DrlgType"] = "2"
    for col in ("SizeX", "SizeX(N)", "SizeX(H)"):
        pit[col] = SIZE[0]
    for col in ("SizeY", "SizeY(N)", "SizeY(H)"):
        pit[col] = SIZE[1]
    for col in NO_MONSTERS:
        pit[col] = "0"
    for row in levels.rows:  # nothing leads in or out
        for k in range(8):
            if row is pit or row.get(f"Vis{k}") == ARENA_ID:
                row[f"Vis{k}"], row[f"Warp{k}"] = "0", "-1"
    old = levels.first("Waypoint", SLOT)  # Jail level 1 gives up its slot
    old["Waypoint"] = "255"
    pit["Waypoint"] = SLOT
    t.table("actinfo").first("act", "1")["waypoint7"] = pit["Name"]  # 1-based: slot 6

    prest = t.table("lvlprest")
    for other in prest.rows:  # the level's own presets (the Pit room with its chests)
        if other.get("LevelId") == ARENA_ID:
            other["LevelId"] = "0"
    row = dict(prest.first("Name", "Act 1 - Darkest Diablo Den"))  # 010_den_of_evil.py
    row["Name"] = "Act 1 - Darkest Diablo Pit"
    row["Def"] = str(max(int(r["Def"]) for r in prest.rows if r["Def"].isdigit()) + 1)
    row["LevelId"] = ARENA_ID
    row["File1"] = MAP_FILE
    prest.rows.append(row)

    # Monsters: MF copies, and a target copy of each with the guaranteed unique or set
    stats = t.table("monstats")
    loot = inifuss.Loot(t.table("treasureclassex"))
    tc_columns = [c for c in stats.columns if c.startswith("TreasureClass")]
    models = t.data("hd/character/monsters.json")
    level_cols = (("", "MonLvlEx"), ("(N)", "MonLvlEx(N)"), ("(H)", "MonLvlEx(H)"))
    bonus = {}
    for suffix, lvl_col in level_cols:
        item_level = int(pit[lvl_col]) + UNIQUE_LEVEL_BONUS
        tag = f" {suffix}" if suffix else ""
        bonus[suffix] = loot.code_tree(f"DD Pit Target Bonus{tag}", inifuss.bonus_bases(t, item_level),
                                       {q: "1024" for q in inifuss.QUALITIES})

    def numbered():
        return len([r for r in stats.rows if r.get("*hcIdx", "").strip().isdigit()])

    for source in WAVE_TYPES:
        for target in (False, True):
            new_id = f"dd_pit_{source}" + ("_t" if target else "")
            cls = numbered()  # this row's class number
            row = stats.add_copy("Id", source, Id=new_id, NextInClass="", minion1="",
                                 **{"*hcIdx": str(cls)})
            for c in tc_columns:
                row[c] = loot.with_mf(row[c])
            if target:
                for suffix, _ in level_cols:
                    col = "TreasureClassUnique" + suffix
                    tag = f" {suffix}" if suffix else ""
                    name = f"DD Pit Target {source}{tag}"
                    loot.tcs.rows.append({**{k: "" for k in loot.tcs.rows[0] if k != "__width__"},
                                          "Treasure Class": name, "Picks": "-2", "NoDrop": "0",
                                          "Item1": bonus[suffix], "Prob1": "1", "Item2": row[col], "Prob2": "1"})
                    loot.by_name[name] = loot.tcs.rows[-1]
                    row[col] = name
                t.loot_lines.append(f"targetclass {ARENA_ID} {cls}")
            models[new_id] = models[source]  # without this the monster is invisible
