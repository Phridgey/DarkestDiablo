"""Andariel (the act's last stop, tier 6), built like Sam's Tower zone (120_linear_zone.py):
Catacombs level 4 (level 37) on a copy of Blizzard's map of her lair, without its monsters, doors
and stairs (src/maps/andariel.json): the level leads nowhere; the hero comes by the route map and
leaves by Town Portal. The plugin (src/plugin/andariel.cpp) does the rest:

- Packs of Catacombs monsters come out as the hero nears their spots.
- Two unique monsters (random names and affixes, like the Pit's), far apart: one in the south room,
  one at the west end of the hall below her throne. Each drops one of her two keys, a
  "Vial of Maiden's Venom" and a "Shard of Anguish" (new 1x1 items with quest-item looks).
- Approaching her hall with both keys takes them and brings Andariel out; her death clears the
  stop (and the act).

Here: the level and its map, the uniques' monster types ("dd_andy_<id>_k1/_k2", whose unique loot
drops their key first), and the keys and their names. The plugin reads the keys' codes and the
uniques' class numbers from "andykey" and "monclass" lines in d2rloader/loot_data.txt.
"""

ARENA_ID = "37"                     # "Act 1 - Catacombs 4"
MAP_FILE = "Act1/Catacomb/darkest_andariel.ds1"
NO_MONSTERS = ("MonDen", "MonDen(N)", "MonDen(H)", "MonUMin", "MonUMax", "MonUMin(N)", "MonUMax(N)", "MonUMin(H)", "MonUMax(H)")

# The two uniques: (monster type Id, made from, key code, key name, item it looks like)
KEEPERS = [
    ("dd_andy_fallenshaman4_k1", "fallenshaman4", "za1", "Vial of Maiden's Venom", "xyz"),
    ("dd_andy_arach1_k2",        "arach1",        "za2", "Shard of Anguish",       "mss"),
]
STRING_ID = 91200                   # clear of the traveler (91000), camp heroes (91001) and the Tower (91100)


def patch(t):
    level(t)
    keepers(t)


def level(t):
    levels = t.table("levels")
    lair_level = levels.first("Id", ARENA_ID)
    for col in NO_MONSTERS:
        lair_level[col] = "0"
    for row in levels.rows:         # nothing leads in or out (the map has no stairs)
        for k in range(8):
            if row is lair_level or row.get(f"Vis{k}") == ARENA_ID:
                row[f"Vis{k}"], row[f"Warp{k}"] = "0", "-1"
    prest = t.table("lvlprest")
    lair = prest.first("Name", "Act 1 - Andariel")
    lair["File1"] = MAP_FILE        # our copy of the map
    lair["Scan"] = "0"              # no exits to register


def keepers(t):
    stats = t.table("monstats")
    models = t.data("hd/character/monsters.json")
    misc = t.table("misc")
    tcs = t.table("treasureclassex")
    items = t.data("hd/items/items.json")
    assets = {code: v for entry in items for code, v in entry.items()}
    taken = {r.get("code") for name in ("weapons", "armor", "misc") for r in t.table(name).rows}
    names = []
    for n, (keeper, source, code, key_name, look) in enumerate(KEEPERS):
        if code in taken:
            raise RuntimeError(f"item code {code} is taken")
        # Kept, not used: the vial's look comes from the Potion of Life, which a right click drinks
        # (+20 life, 2026-11-22), so the copy can't be used.
        misc.add_copy("code", look, code=code, name=key_name, namestr=f"dd{code}", spawnable="0",
                      quest="0", questdiffcheck="0", level="1", levelreq="0", useable="0", pSpell="-1", spelldesc="0", spelldescstr="", spelldescstr2="", spelldesccolor="0",
                      usesound="item_gem")
        items.append({code: dict(assets[look])})
        names.append((f"dd{code}", key_name))

        # The unique's loot: its key first (negative picks drop the entries in order), then the
        # usual unique loot of its kind.
        original = stats.first("Id", source)
        cls = max(int(r["*hcIdx"]) for r in stats.rows if r.get("*hcIdx", "").strip().isdigit()) + 1
        row = stats.add_copy("Id", source, Id=keeper, NextInClass="", minion1="", minion2="", **{"*hcIdx": str(cls)})
        for suffix in ("", "(N)", "(H)"):
            col = "TreasureClassUnique" + suffix
            name = f"DD Andariel Key {n + 1}{suffix and ' ' + suffix}"
            tc = {k: "" for k in tcs.rows[0] if k != "__width__"}
            tc.update({"Treasure Class": name, "Picks": "-2", "NoDrop": "0", "Item1": code, "Prob1": "1",
                       "Item2": original[col], "Prob2": "1"})
            tcs.rows.append(tc)
            row[col] = name
        models[keeper] = models[source]  # without this the monster is invisible
        t.loot_lines.append(f"andykey {n} {code}")

    if t.excel.name != "excel":     # the base copy only keeps tables; strings are shipped once
        return
    strings = t.data("local/lng/strings/item-names.json")
    langs = [k for k in strings[0] if k not in ("id", "Key")]
    for offset, (key, text) in enumerate(names):
        if not any(e.get("Key") == key for e in strings):
            strings.append({"id": STRING_ID + offset, "Key": key, **{lang: text for lang in langs}})
