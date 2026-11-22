"""Act V (docs/act5.md), on copies of the game's own rooms with no stairs (src/maps/<name>.json).
The plugin spawns the waves (darkestdiablo_plugin.cpp: "Act V").

- Shenk (on level 116, tier 19): one strip of the siege road. The Bloody Foothills are an outdoor
  level placed anew every game, so the stop borrows the spare ice cave level 116 (Act V, its own
  layer) and copies the foothills' look and name onto it. Shenk and Eldritch the Rectifier.
- The fork (tier 20): Frigid Highlands (on level 113, XP grind): the barbarians' three prison pens
  in a line; Pindleskin's courtyard (level 121, MF run); the Icy Cellar (level 119, crafting hunt).
- Anya (level 114, tier 21, MF loot): the Frozen River cave. Frozenstein always drops Malah's
  potion first (any hero can free Anya with it); freeing her clears the stop.
- The fork (tier 22): the Halls of Vaught (level 124, MF run), Nihlathak at the end; the Cow
  Level (level 39, crafting hunt), the Cow King at the end.
- The Ancients (level 120, tier 23): the summit without its altar and doors.
- Baal (level 131, tier 24): the throne room as the game has it (his own waves, then his portal to
  the Worldstone Chamber, level 132). He is an MF fight, and his loot always starts with one elite
  unique item and one elite set item.

Also the socket rewards (docs/flattening.md, "Socket rewards"): a "Socket Chisel" item (zsk) the
plugin hands out (Charsi after the Malus, Hratli once Khalim's three parts are held, Larzuk's
second socket after Shenk), and the Horadric Cube recipe chisel + any item without sockets ->
the item with one socket.
"""
import importlib.util
from pathlib import Path


def _load(name, file):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(file))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


act3 = _load("act3", "113_act3.py")
inifuss = act3.inifuss

CRAFT_A5 = {"": ("Runes 17", ("Flawless Gem", "Perfect Gem"), "Hpotion 6"),
            "(N)": ("Runes 17", ("Flawless Gem", "Perfect Gem"), "Hpotion 6"),
            "(H)": ("Runes 17", ("Flawless Gem", "Perfect Gem"), "Hpotion 6")}

SHENK = {"id": "116", "look": "110", "map": "Expansion/Siege/darkest_shenk.ds1", "size": ("16", "48"),
         "template": "Act 5 - Siege Strip 3", "name": "Act 5 - Darkest Diablo Bloody Foothills",
         "waves": ["minion1", "deathmauler1", "imp1"], "prefix": "dd_shenk_", "pieces": ["strip03"]}
HIGHLANDS = {"id": "113", "look": "111", "map": "Expansion/Siege/darkest_highlands.ds1", "size": ("16", "48"),
             "template": "Act 5 - Barricade Prison 1", "name": "Act 5 - Darkest Diablo Frigid Highlands",
             "waves": ["imp1", "siegebeast2", "minion1"], "prefix": "dd_xp_a5_",
             "pieces": ["sharptoothaa", "sharptoothbb", "sharptoothcc"], "kind": "xp"}
PINDLE = {"id": "121", "map": "Expansion/WildTemple/darkest_pindle.ds1", "size": ("22", "27"), "template": "Act 5 - Temple Entrance",
          "name": "Act 5 - Darkest Diablo Pindleskin", "waves": ["reanimatedhorde1", "succubus1"], "prefix": "dd_mf_a5_",
          "pieces": ["tempenter"], "kind": "mf"}
CELLAR = {"id": "119", "map": "Expansion/IceCave/darkest_cellar.ds1", "size": ("16", "64"), "template": "Act 5 - Ice NS",
          "name": "Act 5 - Darkest Diablo Icy Cellar", "waves": ["snowyeti4", "succubus3", "reanimatedhorde3"],
          "prefix": "dd_craft_a5_", "pieces": ["ices01", "icens01", "icens02", "icen01"], "kind": "craft", "craft": CRAFT_A5,
          "loot_name": "DD Craft A5"}
ANYA = {"id": "114", "map": "Expansion/IceCave/darkest_anya.ds1", "size": ("64", "64"), "template": "Act 5 - Ice River A",
        "name": "Act 5 - Darkest Diablo Frozen River", "waves": ["snowyeti2", "succubus5", "reanimatedhorde1"],
        "prefix": "dd_mf_a5b_", "pieces": ["riverice01"], "kind": "mf"}
VAUGHT = {"id": "124", "map": "Expansion/WildTemple/darkest_vaught.ds1", "size": ("84", "84"), "template": "Act 5 - Temple Final Room",
          "name": "Act 5 - Darkest Diablo Halls of Vaught", "waves": ["minion6", "succubus4", "putriddefiler2"],
          "prefix": "dd_mf_a5c_", "pieces": ["nihle"], "kind": "mf"}
COWS = {"id": "39", "map": "Act1/Outdoors/darkest_cows.ds1", "size": ("32", "32"), "template": "Act 1 - Wild Border 1",
        "name": "Act 1 - Darkest Diablo Cow Level", "waves": ["hellbovine"], "prefix": "dd_craft_a5b_",
        "pieces": ["wild1", "wild2", "wild3", "wild4"], "kind": "craft", "craft": CRAFT_A5, "loot_name": "DD Craft A5B"}
ANCIENTS = {"id": "120", "map": "Expansion/MountainTop/darkest_ancients.ds1", "size": ("20", "28"), "template": "Act 5 - Baal Entrance",
            "name": "Act 5 - Darkest Diablo Arreat Summit", "waves": [], "prefix": "dd_anc_", "pieces": ["mtntop"]}
THRONE = {"id": "131", "map": "Expansion/BaalLair/darkest_throne.ds1", "size": ("40", "52"), "template": "Act 5 - ThroneRoom",
          "name": "Act 5 - Darkest Diablo Throne", "waves": [], "prefix": "dd_throne_", "pieces": ["wthrone"], "scan": True}

BAAL_LEVEL = "90"     # his normal monster level, so his item level reaches every elite unique and set
CHISEL = ("zsk", "Socket Chisel", "key")  # code, name, the item whose look it borrows
STRING_ID = 91300


def malahs_potion(t):
    """Frozenstein drops Malah's potion first, every time, so any hero can free Anya. The potion
    is a quest item that only drops while its quest is open, so that check is cleared."""
    misc = t.table("misc").first("code", "ice")
    misc["quest"], misc["questdiffcheck"] = "0", "0"
    row = t.table("superuniques").first("Superunique", "Frozenstein")
    act3.first_drop(t, row, ["TC", "TC(N)", "TC(H)"], "ice", "DD Frozenstein")


def elite_codes(t, table):
    """Codes of the elite bases (a base is elite when it is its own ultracode)."""
    return {r["code"] for name in ("weapons", "armor") for r in t.table(name).rows
            if r.get("code") and r.get("code") == r.get("ultracode")} & table


def baal(t):
    """Baal (baalcrab): MF loot, and first one elite unique item and one elite set item."""
    stats = t.table("monstats")
    row = stats.first("Id", "baalcrab")
    row["Level"] = BAAL_LEVEL
    loot = inifuss.Loot(t.table("treasureclassex"))
    level = int(BAAL_LEVEL)

    def ok(r, col):
        lvl = r.get("lvl", "").strip()
        return lvl.isdigit() and int(lvl) <= level and r.get("spawnable") == "1" and r.get("disabled", "") in ("", "0")

    uniques = set()
    for r in t.table("uniqueitems").rows:
        if r.get("code") and ok(r, "code"):
            r["nolimit"] = "1"
            uniques.add(r["code"])
    sets = {r["item"] for r in t.table("setitems").rows if r.get("item") and ok(r, "item")}
    eu = sorted(elite_codes(t, uniques))
    es = sorted(elite_codes(t, sets))
    if not eu or not es:
        raise RuntimeError("No elite unique or set base for Baal's drop")
    unique = loot.code_tree("DD Baal Elite Unique", eu, {"Unique": "1024"})
    setitem = loot.code_tree("DD Baal Elite Set", es, {"Set": "1024"})
    tcs = loot.tcs
    for col in ("TreasureClass", "TreasureClassChamp", "TreasureClassUnique", "TreasureClassQuest"):
        if not row.get(col):
            continue
        name = f"DD Baal {col}"
        r = {k: "" for k in tcs.rows[0] if k != "__width__"}
        r.update({"Treasure Class": name, "Picks": "-3", "NoDrop": "0", "Item1": unique, "Prob1": "1", "Item2": setitem,
                  "Prob2": "1", "Item3": loot.with_mf(row[col]) if row[col] in loot.by_name else row[col], "Prob3": "1"})
        tcs.rows.append(r)
        row[col] = name


def chisel(t):
    """The Socket Chisel and its Horadric Cube recipe."""
    code, name, look = CHISEL
    misc = t.table("misc")
    if misc.find("code", code):
        raise RuntimeError(f"item code {code} is taken")
    misc.add_copy("code", look, code=code, name=name, namestr=f"dd{code}", spawnable="0", quest="0", questdiffcheck="0",
                  level="1", levelreq="0")
    items = t.data("hd/items/items.json")
    assets = {c: v for entry in items for c, v in entry.items()}
    items.append({code: dict(assets[look])})
    cube = t.table("cubemain")
    soj = next(r for r in cube.rows if r.get("output", "").strip('"').startswith("useitem,sock=1"))
    recipe = dict(soj)
    recipe.update({"description": "Socket Chisel + any item without sockets -> the item with one socket",
                   "numinputs": "2", "input 1": '"any,nos"', "input 2": code, "input 3": "", "input 4": "", "input 5": "",
                   "input 6": "", "input 7": "", "output": '"useitem,sock=1"'})
    cube.rows.append(recipe)
    if t.excel.name != "excel":  # the base copy only keeps tables; strings are shipped once
        return
    strings = t.data("local/lng/strings/item-names.json")
    langs = [k for k in strings[0] if k not in ("id", "Key")]
    if not any(e.get("Key") == f"dd{code}" for e in strings):
        strings.append({"id": STRING_ID, "Key": f"dd{code}", **{lang: name for lang in langs}})


def patch(t):
    for a in (SHENK, HIGHLANDS, PINDLE, CELLAR, ANYA, VAUGHT, COWS, ANCIENTS, THRONE):
        act3.stop(t, a)
    malahs_potion(t)
    baal(t)
    chisel(t)
