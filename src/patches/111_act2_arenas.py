"""Act II's first stops (docs/act2.md), each on a copy of its level's own rooms with no stairs
(src/maps/<name>.json), reached by the route map and left by Town Portal:

- Halls of the Dead 3 (level 60, tier 6, forced): a short run of tomb rooms ending in the room of
  the Horadric Cube's chest. Cleared when the hero picks up the Cube (the plugin).
- Sewers 3 (level 49, tier 7, XP grind): a heavily shortened sewer ending in Radament's lair.
  +40% experience and triple gold (as every XP grind: 115_xp_gold.py picks up "dd_xp_" copies).
  Cleared when Radament and his waves are dead (his Book of Skill is the game's own quest drop).
- Stony Tomb (level 55, tier 7, crafting hunt): four tomb rooms opened into one hall, with the
  Mausoleum's crafting loot for Act II (runes up to Shael, 060_mausoleum.py's crafting_loot).

All three keep their own monster level for now; the flattening's level curve sets it later.
"""
import importlib.util
from pathlib import Path

NO_MONSTERS = ("MonDen", "MonDen(N)", "MonDen(H)", "MonUMin", "MonUMax", "MonUMin(N)", "MonUMax(N)", "MonUMin(H)", "MonUMax(H)")
XP_BONUS = 1.40

HALLS = {"id": "60", "map": "Act2/Tomb/darkest_halls.ds1", "size": ("16", "48"), "template": "Act 2 - Tomb Cube S",
         "name": "Act 2 - Darkest Diablo Halls", "waves": ["mummy2", "unraveler1", "batdemon1", "slinger2"], "prefix": "dd_halls_"}
SEWERS = {"id": "49", "map": "Act2/Sewer/darkest_sewers.ds1", "size": ("12", "36"), "template": "Act 2 - Sewer Radament's Lair S",
          "name": "Act 2 - Darkest Diablo Sewers", "waves": ["skeleton4", "pantherwoman1", "mummy1", "skmage_fire3"], "prefix": "dd_xp_a2_"}
STONY = {"id": "55", "map": "Act2/Tomb/darkest_stony.ds1", "size": ("32", "32"), "template": "Act 2 - Tomb NSEW",
         "name": "Act 2 - Darkest Diablo Stony Tomb", "waves": ["skeleton5", "scarab1", "skmage_ltng3"], "prefix": "dd_craft_a2_"}

# Stony Tomb's crafting loot per difficulty: runes up to Shael (Act II's highest; the game's
# "Runes 7" without Dol), gems a step up from the Mausoleum's.
STONY_CRAFT = {
    "":     ([("r13", 3), ("Runes 6", 45)], ("Flawed Gem", "Normal Gem"), "Hpotion 2"),
    "(N)":  ("Runes 8", ("Flawed Gem", "Normal Gem"), "Hpotion 3"),
    "(H)":  ("Runes 13", ("Normal Gem", "Flawless Gem"), "Hpotion 5"),
}

_spec = importlib.util.spec_from_file_location("mausoleum", Path(__file__).with_name("060_mausoleum.py"))
mausoleum = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mausoleum)


def arena_level(t, arena):
    """The level becomes one fixed map (our copy) that leads nowhere and spawns nothing itself."""
    levels = t.table("levels")
    row = levels.first("Id", arena["id"])
    row["DrlgType"] = "2"
    for col in ("SizeX", "SizeX(N)", "SizeX(H)"):
        row[col] = arena["size"][0]
    for col in ("SizeY", "SizeY(N)", "SizeY(H)"):
        row[col] = arena["size"][1]
    for col in NO_MONSTERS:
        row[col] = "0"
    # Not a quest level: with Quest set, the game counts the level's monsters for a quest (the
    # Den's gave a skill point after a few kills, since arenas start empty).
    row["Quest"] = "0"
    # No waypoint slot: the game looks for the level's waypoint when the hero arrives, and these
    # maps have none (2026-11-22: entering the Canyon of the Magi, slot 17, crashed the game).
    row["Waypoint"] = "255"
    for other in levels.rows:
        for k in range(8):
            if other is row or other.get(f"Vis{k}") == arena["id"]:
                other[f"Vis{k}"], other[f"Warp{k}"] = "0", "-1"
    prest = t.table("lvlprest")
    for other in prest.rows:
        if other.get("LevelId") == arena["id"]:
            other["LevelId"] = "0"
    new = dict(prest.first("Name", arena["template"]))
    new.update({"Name": arena["name"], "LevelId": arena["id"], "SizeX": arena["size"][0], "SizeY": arena["size"][1],
                "Files": "1", "File1": arena["map"], "Scan": "0", "Populate": "0"})
    new["Def"] = str(max(int(r["Def"]) for r in prest.rows if r["Def"].isdigit()) + 1)
    for k in range(2, 7):
        new[f"File{k}"] = "0"
    prest.rows.append(new)
    return row


def copies(t, arena, xp=1.0):
    """The wave monsters: copies of the level's own kinds ("<prefix><id>"), so the plugin can spawn
    them; with `xp`, worth that much more experience."""
    stats = t.table("monstats")
    models = t.data("hd/character/monsters.json")
    for source in arena["waves"]:
        new_id = arena["prefix"] + source
        copy = stats.add_copy("Id", source, Id=new_id, NextInClass="")
        if copy.get("minion1") in arena["waves"]:
            copy["minion1"] = arena["prefix"] + copy["minion1"]
        if xp != 1.0:
            for col in ("Exp", "Exp(N)", "Exp(H)"):
                if copy.get(col, "").strip().isdigit():
                    copy[col] = str(round(int(copy[col]) * xp))
        models[new_id] = models[source]  # without this the monster is invisible


def radament_drops_his_book(t):
    """Radament's loot starts with his Book of Skill, every time (a monster drops its negative-picks
    entries in order, so the book can't be crowded out). Clearing the stop needs it picked up."""
    tcs = t.table("treasureclassex")
    template = tcs.rows[0]
    supers = t.table("superuniques")
    radament = supers.first("Superunique", "Radament")
    stats = t.table("monstats")
    own = stats.first("Id", "radament")
    for col, tag in (("TC", ""), ("TC(N)", " (N)"), ("TC(H)", " (H)")):
        name = f"DD Radament{tag}"
        row = {k: "" for k in template if k != "__width__"}
        row.update({"Treasure Class": name, "Picks": "-2", "NoDrop": "0",
                    "Item1": "ass", "Prob1": "1", "Item2": radament[col], "Prob2": "1"})
        tcs.rows.append(row)
        radament[col] = name
        suffix = col[2:]  # "", "(N)", "(H)"
        for c in ("TreasureClass" + suffix, "TreasureClassUnique" + suffix):
            if c in own:
                own[c] = name


def patch(t):
    arena_level(t, HALLS)
    copies(t, HALLS)
    arena_level(t, SEWERS)
    copies(t, SEWERS, XP_BONUS)
    # radament_drops_his_book(t): shelved 2026-11-23. The book can't be read outside the real quest and
    # the game's own quest drop already gives one, so the stop clears when Radament dies instead.
    stony = arena_level(t, STONY)
    mausoleum.crafting_loot(t, stony, arena_id=STONY["id"], wave_types=STONY["waves"], craft=STONY_CRAFT,
                            name="DD Craft A2", id_prefix=STONY["prefix"])
