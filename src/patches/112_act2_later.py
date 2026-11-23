"""The rest of Act II (docs/act2.md), each on a copy of its level's own rooms with no stairs
(src/maps/<name>.json), reached by the route map and left by Town Portal. The plugin spawns
every monster (darkestdiablo_plugin.cpp: "Act II").

Tier 3, both forced, any order (quest stops: cleared on picking up the quest item):
- Maggot Lair 3 (level 64): three lair rooms in a line ending in the treasure room, whose
  unique-chest spot holds the Staff of Kings' chest on this level. Coldworm the Burrower waits there.
- Claw Viper Temple 2 (level 61): the temple's altar room alone. Fangskin and his clan; the
  Tainted Sun altar gives the Amulet of the Viper.
Tier 4, the fork:
- Arcane Sanctuary (level 74, crafting hunt): one arm of the cross, ending at the Summoner's
  platform; monsters only come from ahead; the Summoner's death clears it. Crafting loot as the
  Stony Tomb's (runes up to Shael).
- Ancient Tunnels (level 65, MF run): four sewer rooms in a line (the tunnels use the sewers'
  tiles). Every monster drops as if magic find were 1.4 times as strong (035_inifuss_loot.py's
  with_mf); each pack is led by a unique, and a random one of them is the target.
- Canyon of the Magi (level 46, XP grind): a round sandy hollow; +40% experience and triple gold.
Tier 5:
- Duriel: the hero arrives in the true Tal Rasha's Tomb's last room (level 66, the orifice
  room alone). Inserting the Horadric Staff plays the game's own event, which opens the way to
  Duriel's lair (level 73, untouched). His death clears the stop.
"""
import importlib.util
from pathlib import Path


def _load(name, file):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(file))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


arenas = _load("act2_arenas", "111_act2_arenas.py")
inifuss = _load("inifuss_loot", "035_inifuss_loot.py")

MAGGOT = {"id": "64", "map": "Act2/Maggot/darkest_maggot.ds1", "size": ("10", "30"), "template": "Act 2 - Lair Treasure S",
          "name": "Act 2 - Darkest Diablo Maggot Lair", "waves": ["sandmaggot1", "sandmaggot2", "swarm2", "scarab2", "scarab3"],
          "prefix": "dd_maggot_"}
CLAWVIPER = {"id": "61", "map": "Act2/Tomb/darkest_clawviper.ds1", "size": ("16", "16"), "template": "Act 2 - Tomb Tainted Sun X",
             "name": "Act 2 - Darkest Diablo Claw Viper Temple", "waves": ["clawviper2", "clawviper3", "mummy3"], "prefix": "dd_viper_"}
ARCANE = {"id": "74", "map": "Act2/Arcane/darkest_arcane.ds1", "size": ("12", "48"), "template": "Act 2 - Arcane Summoner S",
          "name": "Act 2 - Darkest Diablo Arcane Sanctuary", "waves": ["wraith3", "goatman4", "vampire1"], "prefix": "dd_craft_a2b_"}
TUNNELS = {"id": "65", "map": "Act2/Sewer/darkest_tunnels.ds1", "size": ("12", "48"), "template": "Act 2 - Sewer NS",
           "name": "Act 2 - Darkest Diablo Ancient Tunnels", "waves": ["zombie5", "sandraider3", "mummy3", "skmage_ltng4"],
           "prefix": "dd_mf_a2_"}
CANYON = {"id": "46", "map": "Act2/Outdoors/darkest_canyon.ds1", "size": ("24", "24"), "template": "Act 2 - Desert Valley Ruin 1",
          "name": "Act 2 - Darkest Diablo Canyon", "waves": ["brute4", "sandmaggot3", "pantherwoman4", "scarab4", "slinger4"],
          "prefix": "dd_xp_a2b_"}
TALRASHA = {"id": "66", "map": "Act2/Tomb/darkest_talrasha.ds1", "size": ("16", "16"), "template": "Act 2 - Tomb Talrasha N",
            "name": "Act 2 - Darkest Diablo Tal Rasha", "waves": [], "prefix": "dd_tal_"}


def tunnels_mf(t):
    """The tunnels' copies drop as if magic find were 1.4 times as strong (as the Pit's)."""
    stats = t.table("monstats")
    loot = inifuss.Loot(t.table("treasureclassex"))
    columns = [c for c in stats.columns if c.startswith("TreasureClass")]
    for source in TUNNELS["waves"]:
        row = stats.first("Id", TUNNELS["prefix"] + source)
        for c in columns:
            if row.get(c) and row[c] in loot.by_name:
                row[c] = loot.with_mf(row[c])


def bosses_drop_staff_pieces(t):
    """Coldworm drops the Staff of Kings and Fangskin the Amulet of the Viper, first, every time
    (2026-11-22: the chest and the altar gave nothing, so neither stop could be cleared). Like
    Radament's book: negative picks drop in order, so the piece can't be crowded out. Both pieces
    are quest items of the Horadric Staff quest, which only drop while it is open; their quest
    columns are cleared so they drop anywhere (CLAUDE.md, lessons). The Horadric Staff itself
    keeps its own, for the orifice event."""
    tcs = t.table("treasureclassex")
    template = tcs.rows[0]
    supers = t.table("superuniques")
    for boss, code, short in (("Coldworm the Burrower", "msf", "Coldworm"), ("Fangskin", "vip", "Fangskin")):
        row = supers.first("Superunique", boss)
        for col, tag in (("TC", ""), ("TC(N)", " (N)"), ("TC(H)", " (H)")):
            name = f"DD {short}{tag}"
            tc = {k: "" for k in template if k != "__width__"}
            tc.update({"Treasure Class": name, "Picks": "-2", "NoDrop": "0",
                       "Item1": code, "Prob1": "1", "Item2": row[col], "Prob2": "1"})
            tcs.rows.append(tc)
            row[col] = name
    t.table("weapons").first("code", "msf").update({"quest": "0", "questdiffcheck": "0"})
    t.table("misc").first("code", "vip").update({"quest": "0", "questdiffcheck": "0"})


def patch(t):
    for arena in (MAGGOT, CLAWVIPER, TUNNELS, TALRASHA):
        arenas.arena_level(t, arena)
        if arena["waves"]:
            arenas.copies(t, arena)
    bosses_drop_staff_pieces(t)
    # The orifice room keeps its preset's warp scan, as Blizzard's has it (the event's way to Duriel).
    t.table("lvlprest").first("Name", TALRASHA["name"])["Scan"] = "1"
    tunnels_mf(t)
    canyon = arenas.arena_level(t, CANYON)
    arenas.copies(t, CANYON, arenas.XP_BONUS)
    arcane = arenas.arena_level(t, ARCANE)
    arenas.mausoleum.crafting_loot(t, arcane, arena_id=ARCANE["id"], wave_types=ARCANE["waves"], craft=arenas.STONY_CRAFT,
                                   name="DD Craft A2B", id_prefix=ARCANE["prefix"])
