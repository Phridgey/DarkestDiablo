"""Monster levels for one run through the five acts (docs/flattening.md: about 18 hero levels per
act, Andariel near 18, Duriel 36, Mephisto 54, Diablo 72, Baal 90; decided 2026-11-21).

The game always runs on Normal, so only the Normal columns change. A monster's level comes from
the area it is in (levels.txt MonLvl / MonLvlEx); the act bosses have their own fixed level
(monstats.txt Level). Both rise step by step along the route: every stop's level, and the level
of the boss that ends an act. Experience, gold and what can drop (item level) follow the level.

This runs first (004), so later patches that read an arena's level (crafting loot's white
bases, the Pit's and Treehead's set/unique picks) see the new one.

Loot follows the level too (decided 2026-11-21: "we follow my level progression objective in all
things"). In Nightmare and Hell the game moves a monster's loot table up its group (Act 1 H2H A,
B, C, Act 2 H2H A ... Act 5 (H) H2H C) to the highest one at or below the monster's level; in
Normal it never does. So here every monster type a stop's waves are made from gets, before the
stops copy it, the table of its group that fits the stop's level (champion tables 2 levels up,
unique tables 3). A type used by several stops takes the lowest of their levels. The monsters'
own levels are set on the copies at the end (996_monster_levels.py).
"""
import importlib.util
from pathlib import Path


def _load(name, file):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(file))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

# Area level by levels.txt Id (the stops' arena and arrival levels, route.h).
AREA_LEVELS = {
    # Act I: 1-18
    13: 3,                       # Den of Evil (arena)
    9: 5, 10: 5,                 # Burial Grounds and the crypt entry room
    11: 7, 12: 7, 14: 7, 15: 7,  # fork 1: Inifuss (arena and entry), Mausoleum, Crypt
    38: 10,                      # Tristram
    16: 13, 25: 13, 28: 13,      # fork 2: Pit, Countess, Malus
    37: 17,                      # Andariel
    # Act II: 18-36
    60: 20,                      # Halls of the Dead
    49: 23, 55: 23,              # fork: Sewers, Stony Tomb
    64: 27, 61: 27,              # Maggot Lair, Claw Viper Temple
    74: 31, 65: 31, 59: 31,      # fork: Arcane Sanctuary, Ancient Tunnels, Canyon of the Magi
    66: 35, 73: 35,              # Tal Rasha's Tomb, Duriel's lair
    # Act III: 36-54
    85: 40, 91: 40, 93: 40,      # Khalim's three parts
    94: 46, 90: 46, 95: 46,      # fork: Ruined Temple, Swampy Pit, Lower Kurast
    96: 50,                      # Travincal
    102: 53,                     # Durance of Hate 3
    # Act IV: 54-72
    105: 57,                     # Plains of Despair
    125: 62, 126: 62, 127: 62,   # fork: the three red portals
    107: 67,                     # Hellforge
    108: 71,                     # Chaos Sanctuary
    # Act V: 72-90
    116: 74,                     # Shenk
    113: 78, 121: 78, 119: 78,   # fork: Frigid Highlands, Pindleskin, Icy Cellar
    114: 81,                     # Anya
    124: 84, 39: 84,             # fork: Halls of Vaught, Cow Level
    120: 87,                     # the Ancients
    131: 89, 132: 89,            # Throne of Destruction, Worldstone Chamber
}

# Bosses with a fixed level of their own (monstats.txt Level, Normal).
BOSS_LEVELS = {
    "bloodraven": 5, "griswold": 10, "andariel": 17,
    "radament": 23, "summoner": 31, "duriel": 35,
    "mephisto": 53, "izual": 57, "diablo": 71,
    "nihlathakboss": 84, "putriddefiler1": 84, "putriddefiler2": 84, "putriddefiler3": 84,
    "putriddefiler4": 84, "putriddefiler5": 84, "baalcrab": 90,
}


# Superuniques a stop spawns or keeps: their stop's level (their loot table moves up its group
# to the stop's level + 3, as their monster level is).
SUPER_LEVELS = {
    "Corpsefire": 3, "Rakanishu": 7, "Treehead WoodFist": 7, "Bloodwitch the Wild": 20,
    "Coldworm the Burrower": 27, "Fangskin": 27, "Sarina the Battlemaid": 46,
    "Ismail Vilehand": 50, "Geleb Flamefinger": 50, "Toorc Icefist": 50,
    "Bremm Sparkfist": 53, "Wyand Voidfinger": 53, "Maffer Dragonhand": 53,
    "Grand Vizier of Chaos": 71, "Lord De Seis": 71, "Infector of Souls": 71,
    "Siege Boss": 74, "Megaflow Rectifier": 74, "Pindleskin": 78, "Frozenstein": 81,
}


def wave_sources():
    """{monster type: the lowest stop level it is used at} from the stops' patches."""
    uses = {}

    def add(types, level):
        for m in types:
            uses[m] = min(uses.get(m, 999), level)

    add(_load("p035", "035_inifuss_loot.py").WAVE_TYPES, 7)
    add(_load("p060", "060_mausoleum.py").WAVE_TYPES, 7)
    add(_load("p070", "070_crypt.py").WAVE_TYPES, 7)
    add(_load("p090", "090_pit.py").WAVE_TYPES, 13)
    add(_load("p110", "110_malus.py").WAVE_TYPES, 13)
    add(_load("p120", "120_linear_zone.py").TOWER_MONSTERS, 5)
    add(["zombie1", "fallen1", "fallenshaman1"], 3)                                # the Den (plugin waves)
    add(["skeleton1", "fallen3", "goatman3", "wraith1", "cr_archer3"], 5)       # Burial Grounds and the Tower (plugin)
    add(["zombie3", "bighead2", "vampire5", "bighead1", "fallen4", "arach1"], 17)  # Andariel's packs (andariel.cpp)
    for name, file in (("p111", "111_act2_arenas.py"), ("p112", "112_act2_later.py"), ("p113", "113_act3.py"),
                       ("p114", "114_act4.py"), ("p114a", "114a_act5.py")):
        module = _load(name, file)
        for value in vars(module).values():
            if isinstance(value, dict) and "waves" in value and "id" in value and value["waves"]:
                add(value["waves"], AREA_LEVELS[int(value["id"])])
    return uses


def scale_loot(t, uses):
    tcs = t.table("treasureclassex")
    ladders = {}
    for r in tcs.rows:
        if r.get("group", "").strip() and r.get("level", "").strip().isdigit():
            ladders.setdefault(r["group"], []).append((int(r["level"]), r["Treasure Class"]))
    where = {name: group for group, rows in ladders.items() for _, name in rows}
    stats = t.table("monstats")
    for monster, level in uses.items():
        row = stats.first("Id", monster)
        for col, extra in (("TreasureClass", 0), ("TreasureClassChamp", 2), ("TreasureClassUnique", 3)):
            group = where.get(row.get(col, ""))
            if group is None:
                continue
            fits = [tc for lvl, tc in sorted(ladders[group]) if lvl <= level + extra]
            if fits:
                row[col] = fits[-1]


def ladder_pick(ladders, where, name, level):
    group = where.get(name)
    if group is None:
        return name
    fits = [tc for lvl, tc in sorted(ladders[group]) if lvl <= level]
    return fits[-1] if fits else name


def scale_supers_and_bosses(t):
    tcs = t.table("treasureclassex")
    ladders = {}
    for r in tcs.rows:
        if r.get("group", "").strip() and r.get("level", "").strip().isdigit():
            ladders.setdefault(r["group"], []).append((int(r["level"]), r["Treasure Class"]))
    where = {name: group for group, rows in ladders.items() for _, name in rows}
    supers = t.table("superuniques")
    for name, level in SUPER_LEVELS.items():
        row = supers.first("Superunique", name)
        row["TC"] = ladder_pick(ladders, where, row["TC"], level + 3)
    # Act bosses have their own tables, one per difficulty: the one whose (vanilla) monster level
    # is the highest at or below the boss's new level (e.g. Diablo at 71 drops as in Nightmare).
    stats = t.table("monstats")
    for monster, level in BOSS_LEVELS.items():
        row = stats.first("Id", monster)
        tiers = [(row.get(c, ""), suffix) for c, suffix in (("Level", ""), ("Level(N)", "(N)"), ("Level(H)", "(H)"))]
        best = ""
        for lvl, suffix in tiers:
            if lvl.strip().isdigit() and int(lvl) <= level:
                best = suffix
        if best:
            for col in ("TreasureClass", "TreasureClassChamp", "TreasureClassUnique", "TreasureClassQuest"):
                if row.get(col + best):
                    row[col] = row[col + best]


def patch(t):
    scale_supers_and_bosses(t)  # before the bosses' levels change below
    uses = wave_sources()
    levels_table = t.table("levels")
    for level_id, level in ((38, 10), (108, 71)):  # Tristram and the Chaos Sanctuary copy their level's own monsters
        row = levels_table.first("Id", str(level_id))
        for col, value in row.items():
            if col.rstrip("0123456789") in ("mon", "nmon", "umon") and value:
                uses[value] = min(uses.get(value, 999), level)
    scale_loot(t, uses)
    levels = t.table("levels")
    for level_id, lvl in AREA_LEVELS.items():
        row = levels.first("Id", str(level_id))
        for col in ("MonLvl", "MonLvlEx"):
            row[col] = str(lvl)
    stats = t.table("monstats")
    for monster, lvl in BOSS_LEVELS.items():
        stats.first("Id", monster)["Level"] = str(lvl)
