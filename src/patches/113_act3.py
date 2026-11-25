"""Act III (docs/act3.md), each stop on a copy of the game's own rooms with no stairs
(src/maps/<name>.json), reached by the route map and left by Town Portal. The plugin spawns the
waves (darkestdiablo_plugin.cpp: "Act III").

Tier 11, all three forced, any order, XP grinds (+40% experience, triple gold):
- Spider Cavern (level 85): one round grotto; Khalim's Eye chest. Cleared on picking up the Eye.
- Flayer Dungeon (level 91): two dungeon rooms; Khalim's Brain chest.
- Kurast Sewers (level 93): the sewers' treasure room; Khalim's Heart chest.
  Every pack is led by a unique, and any of them may drop the Jade Figurine (the Golden Bird).
Tier 12, the fork:
- Ruined Temple (level 94, XP grind): Lam Esen's Tome; cleared on picking it up.
- Swampy Pit (level 90, MF run): the pit's treasure room.
- Lower Kurast (on level 95, crafting hunt): three blocks of the town's outskirts with super
  chests. Kurast itself is an outdoor level the game places anew every game, so the stop borrows
  the spare temple level 95 (same tiles) and copies Lower Kurast's look and name onto it.
Tier 13: Travincal (on level 96, borrowed the same way): the temple square with the High Council
  and the Compelling Orb; MF loot. Smashing the orb with Khalim's Will clears it.
Tier 14: Durance of Hate 3 (level 102): Mephisto's hall as the game has it; his death clears it.

Here are also the helpers the later acts' stops use (114_act4.py, 114a_act5.py): `stop()` makes
a level one fixed map and its wave monsters (XP, MF or crafting copies).
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

# The look of a level: copied from the real level onto a borrowed one.
LOOK = ("Pal", "LevelType", "Intensity", "Red", "Green", "Blue", "SoundEnv", "LevelName", "LevelEntry", "Rain", "Mud",
        "*IsInside", "LOSDraw", "FloorFilter", "DrawEdges")

# Crafting loot per act: runes up to the act's highest (docs/flattening.md), better gems.
CRAFT_A3 = {"": ("Runes 10", ("Normal Gem", "Flawless Gem"), "Hpotion 4"),
            "(N)": ("Runes 10", ("Normal Gem", "Flawless Gem"), "Hpotion 4"),
            "(H)": ("Runes 13", ("Flawless Gem", "Perfect Gem"), "Hpotion 5")}

JADE_CHANCE_NODROP = 4  # a unique's Jade Figurine: 1 in 5


def stop(t, a):
    """One stop: the level becomes the fixed map `a["map"]`, borrowing the look of level `a["look"]`
    when set, at `a["offset"]` when the level has none; its wave monsters are copies of a["waves"]
    ("<prefix><id>"): XP (+40%), MF (as the Pit's) or crafting (060_mausoleum.py's hunt loot)."""
    levels = t.table("levels")
    row = levels.first("Id", a["id"])
    if a.get("look"):
        real = levels.first("Id", a["look"])
        for col in LOOK:
            if col in row and col in real and not (a.get("keep_name") and col == "LevelName"):
                row[col] = real[col]
    if a.get("offset"):
        row["OffsetX"], row["OffsetY"] = (str(v) for v in a["offset"])
    arenas.arena_level(t, a)
    prest = t.table("lvlprest")
    mine = prest.first("Name", a["name"])
    mask = 0
    pieces = {p.lower() for p in a["pieces"]}
    for r in prest.rows:
        files = {r.get(f"File{k}", "").lower().replace("\\", "/").rsplit(".", 1)[0].rsplit("/", 1)[-1] for k in range(1, 7)}
        if files & pieces and r.get("Dt1Mask", "").isdigit():
            mask |= int(r["Dt1Mask"])
    if mask:
        mine["Dt1Mask"] = str(mask)
    if a.get("scan"):
        mine["Scan"] = "1"
    kind = a.get("kind")
    if not a["waves"]:
        return row
    if kind == "craft":
        arenas.mausoleum.crafting_loot(t, row, arena_id=a["id"], wave_types=a["waves"], craft=a["craft"],
                                       name=a["loot_name"], id_prefix=a["prefix"])
        return row
    arenas.copies(t, a, arenas.XP_BONUS if kind == "xp" else 1.0)
    if kind == "mf":
        with_mf(t, a)
    return row


def with_mf(t, a):
    """The copies drop as if magic find were 1.4 times as strong (as the Pit's and the tunnels')."""
    stats = t.table("monstats")
    loot = inifuss.Loot(t.table("treasureclassex"))
    columns = [c for c in stats.columns if c.startswith("TreasureClass")]
    for source in a["waves"]:
        r = stats.first("Id", a["prefix"] + source)
        for c in columns:
            if r.get(c) and r[c] in loot.by_name:
                r[c] = loot.with_mf(r[c])


def first_drop(t, row, columns, item, name):
    """`row`'s loot (monster or superunique) drops `item` first, every time, then its own."""
    tcs = t.table("treasureclassex")
    for col in columns:
        if not row.get(col):
            continue
        tag = col.replace("TreasureClassUnique", "").replace("TreasureClass", "").replace("TC", "").strip()
        new = f"{name}{' ' + tag if tag else ''}"
        r = {k: "" for k in tcs.rows[0] if k != "__width__"}
        r.update({"Treasure Class": new, "Picks": "-2", "NoDrop": "0", "Item1": item, "Prob1": "1",
                  "Item2": row[col], "Prob2": "1"})
        tcs.rows.append(r)
        row[col] = new


SPIDER = {"id": "85", "map": "Act3/Spider/darkest_spider.ds1", "size": ("32", "32"), "template": "Act 3 - Spider Chest NE",
          "name": "Act 3 - Darkest Diablo Spider Cavern", "waves": ["arach3", "arach4", "batdemon2"], "prefix": "dd_xp_a3a_",
          "pieces": ["lairnw", "lairnecon", "lairsw", "lairse"], "kind": "xp"}
FLAYER = {"id": "91", "map": "Act3/Jungle/darkest_flayer.ds1", "size": ("14", "28"), "template": "Act 3 - Dungeon S",
          "name": "Act 3 - Darkest Diablo Flayer Dungeon", "waves": ["fetish3", "fetishshaman3", "fetishblow3"], "prefix": "dd_xp_a3b_",
          "pieces": ["dungs", "dungn"], "kind": "xp"}
KSEWERS = {"id": "93", "map": "Act3/Sewer/darkest_ksewers.ds1", "size": ("18", "22"), "template": "Act 3 - Sewer Treasure X",
           "name": "Act 3 - Darkest Diablo Kurast Sewers", "waves": ["mummy4", "unraveler4", "frogdemon3"], "prefix": "dd_xp_a3c_",
           "pieces": ["sewertreasure1"], "kind": "xp"}
LAMESEN = {"id": "94", "map": "Act3/Temple/darkest_lamesen.ds1", "size": ("24", "24"), "template": "Act 3 - Temple 1",
           "name": "Act 3 - Darkest Diablo Ruined Temple", "waves": ["brute5", "corruptrogue5", "arach5", "vampire2"],
           "prefix": "dd_xp_a3d_", "pieces": ["temple0"], "kind": "xp"}
SWAMPY = {"id": "90", "look": "80", "keep_name": True, "map": "Act3/Kurast/darkest_swampy.ds1", "size": ("32", "32"),
          "template": "Act 3 - Slums 16x16", "name": "Act 3 - Darkest Diablo Swampy Pit",
          "waves": ["bonefetish4", "bonefetish5", "batdemon2", "willowisp1"], "prefix": "dd_mf_a3_",
          "pieces": ["slums16x16_2", "metro16x16_4", "slums16x16_1", "metro16x16_5"], "kind": "mf"}
LOWERKURAST = {"id": "95", "look": "79", "map": "Act3/Kurast/darkest_lowerkurast.ds1", "size": ("16", "48"),
               "template": "Act 3 - Burbs 16x16", "name": "Act 3 - Darkest Diablo Lower Kurast",
               "waves": ["baboon4", "sandleaper4", "zealot1"], "prefix": "dd_craft_a3_",
               "pieces": ["burbs16x16_0", "burbs16x16_1", "burbs16x16_2"], "kind": "craft", "craft": CRAFT_A3,
               "loot_name": "DD Craft A3"}
TRAVINCAL = {"id": "96", "look": "83", "map": "Act3/Travincal/darkest_travincal.ds1", "size": ("32", "32"),
             "template": "Act 3 - Travincal N ", "name": "Act 3 - Darkest Diablo Travincal", "waves": ["cantor3", "vampire2"],
             "prefix": "dd_mf_a3b_", "pieces": ["travn"], "kind": "mf"}
DURANCE = {"id": "102", "map": "Act3/Travincal/darkest_durance.ds1", "size": ("41", "29"), "template": "Act 3 - Mephisto Complex",
           "name": "Act 3 - Darkest Diablo Durance", "waves": [], "prefix": "dd_dur_", "pieces": ["mephcomp"]}

KHALIM = (SPIDER, FLAYER, KSEWERS)


def jade_figurine(t):
    """Uniques of the three Khalim stops (the plugin makes one per pack) may drop the Jade Figurine."""
    tcs = t.table("treasureclassex")
    r = {k: "" for k in tcs.rows[0] if k != "__width__"}
    r.update({"Treasure Class": "DD Jade Chance", "Picks": "1", "NoDrop": str(JADE_CHANCE_NODROP), "Item1": "j34", "Prob1": "1"})
    tcs.rows.append(r)
    stats = t.table("monstats")
    for a in KHALIM:
        for source in a["waves"]:
            row = stats.first("Id", a["prefix"] + source)
            first_drop(t, row, ["TreasureClassUnique"], "DD Jade Chance", f"DD Jade {a['prefix']}{source}")


def chests_without_ammo(t):
    """No arrows or bolts from Act III's chests (2026-11-24: Lower Kurast's super chests were full
    of them). The game picks a chest's table by act and difficulty ("Act 3 Chest A/B/C"), so every
    Act III chest changes: each table on the way to "Ammo" becomes a copy without it (its weight
    goes to NoDrop). Monsters keep the game's own tables."""
    tcs = t.table("treasureclassex")
    by_name = {r["Treasure Class"]: r for r in tcs.rows}
    copies = {}

    def reaches_ammo(name, seen):
        if name == "Ammo":
            return True
        if name in seen or name not in by_name:
            return False
        seen.add(name)
        row = by_name[name]
        return any(reaches_ammo(row.get(f"Item{k}", "").split(",")[0], seen) for k in range(1, 11))

    def without(name):
        if name in copies:
            return copies[name]
        copy = dict(by_name[name])
        copy["Treasure Class"] = copies[name] = f"DD NoAmmo {name}"
        kept, dropped = [], 0
        for k in range(1, 11):
            item, prob = copy.get(f"Item{k}", ""), copy.get(f"Prob{k}", "")
            base = item.split(",")[0]
            if base == "Ammo":
                dropped += int(prob) if prob.strip().isdigit() else 0
            elif item:
                if reaches_ammo(base, set()):
                    item = without(base) + item[len(base):]
                kept.append((item, prob))
        if int(copy.get("Picks") or 1) > 0:
            copy["NoDrop"] = str((int(copy["NoDrop"]) if copy.get("NoDrop", "").strip().isdigit() else 0) + dropped)
        for k in range(1, 11):
            copy[f"Item{k}"], copy[f"Prob{k}"] = kept[k - 1] if k <= len(kept) else ("", "")
        tcs.rows.append(copy)
        return copy["Treasure Class"]

    for tag in ("", " (N)", " (H)"):
        for letter in "ABC":
            chest = by_name.get(f"Act 3{tag} Chest {letter}")
            if chest is None:
                continue
            for k in range(1, 11):
                item = chest.get(f"Item{k}", "")
                base = item.split(",")[0]
                if base and base != chest["Treasure Class"] and reaches_ammo(base, set()):
                    chest[f"Item{k}"] = without(base) + item[len(base):]


def orb_without_event(t):
    """Smashing the Compelling Orb crashed the game (2026-11-24): its use (OperateFn 53) plays the
    game's own event, which opens the way into the Durance in the real Travincal, and our copy on
    another level has none of that. The orb now just opens when used (the Tower tome's OperateFn 6,
    a book that only changes its look), which is all the plugin needs to clear the stop (it counts
    the orb's change of state). The Durance is reached from the route map, as before."""
    t.table("objects").first("Class", "CompellingOrb")["OperateFn"] = "6"


def patch(t):
    orb_without_event(t)
    chests_without_ammo(t)
    for a in (SPIDER, FLAYER, KSEWERS, LAMESEN, SWAMPY, LOWERKURAST, TRAVINCAL, DURANCE):
        stop(t, a)
    jade_figurine(t)
