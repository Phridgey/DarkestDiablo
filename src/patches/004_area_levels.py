"""Monster levels for one run through the five acts (docs/flattening.md: about 18 hero levels per
act, Andariel near 18, Duriel 36, Mephisto 54, Diablo 72, Baal 90; decided 2026-11-21).

The game always runs on Normal, so only the Normal columns change. A monster's level comes from
the area it is in (levels.txt MonLvl / MonLvlEx); the act bosses have their own fixed level
(monstats.txt Level). Both rise step by step along the route: every stop's level, and the level
of the boss that ends an act. Experience, gold and what can drop (item level) follow the level.

This runs first (004), so later patches that read an arena's level (crafting loot's white
bases, the Pit's and Treehead's set/unique picks) see the new one.
"""

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
    74: 31, 65: 31, 46: 31,      # fork: Arcane Sanctuary, Ancient Tunnels, Canyon of the Magi
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


def patch(t):
    levels = t.table("levels")
    for level_id, lvl in AREA_LEVELS.items():
        row = levels.first("Id", str(level_id))
        for col in ("MonLvl", "MonLvlEx"):
            row[col] = str(lvl)
    stats = t.table("monstats")
    for monster, lvl in BOSS_LEVELS.items():
        stats.first("Id", monster)["Level"] = str(lvl)
