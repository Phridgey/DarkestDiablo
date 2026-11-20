"""Tower zone (prototype, Sam 2026-11-20): the Forgotten Tower's last floor as a small quest map.
Reached through a red portal by the town fire (src/plugin/zone.cpp; docs/linear-zone.md).

- Level: "Act 1 - Jail 3" (Id 31), a spare level with its own layer and a fixed place
  (3500, 1600), so the plugin knows where every tile is. The real Tower floor (level 25) stays
  as it is: the Countess stop uses it.
- Map: src/maps/tower_zone.json, a copy of Blizzard's Countess floor (its second layout) without its monsters and
  chests. Look and sound: the Tower cellar's (level 25). Monster level: the Den's (level 8).
- Monsters: the Tower floor's own kinds as copies ("dd_tower_<id>") whose life, damage, defense,
  hit chance and experience are 80% of the Den monsters' (the average of the custom Den's
  Fallen, Fallen Shaman and zombie). The plugin spawns them in packs as the hero comes near;
  the game's random packs are off, so the last room stays empty.
- Two elites (new superuniques.txt rows, one per side room), each dropping one Tower seal
  (a new 1x2 item). The plugin takes both seals at the altar and brings out Blood Raven.
"""

ZONE_ID = "31"                      # "Act 1 - Jail 3", reused (ZoneLevelId in src/plugin/zone.cpp)
TOWER_ID = "25"                     # "Act 1 - Crypt 3 E", the Tower's last floor: look and sound
DEN_ID = "8"                        # the real Den of Evil: monster level
MAP_FILE = "Act1/Crypt/darkest_tower.ds1"
KEEP = {"Id", "Name", "Layer", "OffsetX", "OffsetY"}
NO_MONSTERS = ("MonDen", "MonDen(N)", "MonDen(H)", "MonUMin", "MonUMax", "MonUMin(N)", "MonUMax(N)", "MonUMin(H)", "MonUMax(H)")

DEN_MONSTERS = ["fallen1", "fallenshaman1", "zombie1"]  # the custom Den's wave monsters: the reference
TOWER_MONSTERS = ["fallen3", "wraith1", "goatman3", "cr_archer3"]  # the Tower floor's own kinds
STRENGTH = 0.8                      # 80% of the Den monsters' numbers
STAT_COLUMNS = ["minHP", "maxHP", "AC", "Exp",
                "A1MinD", "A1MaxD", "A1TH", "A2MinD", "A2MaxD", "A2TH", "S1MinD", "S1MaxD", "S1TH"]

# The two elites: (monster type Id, made from, name, seal item code, seal name, item it looks like)
ELITES = [
    ("dd_tower_keeper_w", "goatman3", "Warden of the West Seal", "zs1", "West Tower Seal", "j34"),
    ("dd_tower_keeper_e", "wraith1",  "Warden of the East Seal", "zs2", "East Tower Seal", "g34"),
]
ELITE_MODS = ("5", "8")             # monumod rows: extra strong, magic resistant (as the Den's elites)
STRING_ID = 91100                   # clear of the traveler (91000) and camp heroes (91001)


def patch(t):
    level(t)
    monsters(t)
    seals_and_elites(t)


def level(t):
    levels = t.table("levels")
    zone, tower, den = levels.first("Id", ZONE_ID), levels.first("Id", TOWER_ID), levels.first("Id", DEN_ID)
    for col in levels.columns:
        if col not in KEEP:
            zone[col] = tower[col]
    zone["__width__"] = max(zone.get("__width__", 0), tower.get("__width__", 0))
    for col in levels.columns:      # the Den's monster level, so the 80% is of the Den's monsters
        if col.startswith("MonLvl"):
            zone[col] = den[col]
    zone["Quest"] = "0"
    zone["Waypoint"] = "255"
    zone["DrlgType"] = "2"
    for col in NO_MONSTERS:         # no random packs: the plugin spawns them, so the last room stays empty
        zone[col] = "0"
    for prefix in ("mon", "nmon", "umon"):
        for k in range(1, 26):
            if f"{prefix}{k}" in zone:
                zone[f"{prefix}{k}"] = f"dd_tower_{TOWER_MONSTERS[k - 1]}" if k <= len(TOWER_MONSTERS) else ""
    for row in levels.rows:         # nothing leads in or out
        for k in range(8):
            if row is zone or row.get(f"Vis{k}") == ZONE_ID:
                row[f"Vis{k}"], row[f"Warp{k}"] = "0", "-1"

    prest = t.table("lvlprest")
    for other in prest.rows:        # a preset still naming the level would load instead of ours
        if other.get("LevelId") == ZONE_ID:
            other["LevelId"] = "0"
    row = dict(prest.first("Name", "Act 1 - Crypt Countess X"))
    row["Name"] = "Act 1 - Darkest Diablo Tower Zone"
    row["Def"] = str(max(int(r["Def"]) for r in prest.rows if r["Def"].isdigit()) + 1)
    row["LevelId"] = ZONE_ID
    row["Populate"] = "0"
    row["Scan"] = "0"               # no exits
    row["Files"] = "1"
    row["File1"] = MAP_FILE
    for k in range(2, 7):
        row[f"File{k}"] = "0"
    prest.rows.append(row)


def next_class(stats):
    return max(int(r["*hcIdx"]) for r in stats.rows if r.get("*hcIdx", "").strip().isdigit()) + 1


def den_numbers(stats):
    """Each stat column (all difficulties): 80% of the Den monsters' average."""
    den = [stats.first("Id", m) for m in DEN_MONSTERS]
    out = {}
    for base in STAT_COLUMNS:
        for col in (base, f"{base}(N)", f"{base}(H)"):
            if col not in stats.columns:
                continue
            values = [int(r[col]) for r in den if r.get(col, "").strip().lstrip("-").isdigit()]
            if values:
                out[col] = str(max(1, round(STRENGTH * sum(values) / len(values))))
    return out


def monsters(t):
    stats = t.table("monstats")
    models = t.data("hd/character/monsters.json")
    numbers = den_numbers(stats)
    for source in TOWER_MONSTERS:
        new_id = f"dd_tower_{source}"
        row = stats.add_copy("Id", source, Id=new_id, NextInClass="", **{"*hcIdx": str(next_class(stats))})
        for col, value in numbers.items():
            if row.get(col, "") != "":   # only attacks the kind has
                row[col] = value
        models[new_id] = models[source]  # without this the monster is invisible


def seals_and_elites(t):
    stats = t.table("monstats")
    models = t.data("hd/character/monsters.json")
    misc = t.table("misc")
    supers = t.table("superuniques")
    tcs = t.table("treasureclassex")
    items = t.data("hd/items/items.json")
    assets = {code: v for entry in items for code, v in entry.items()}
    taken = {r.get("code") for name in ("weapons", "armor", "misc") for r in t.table(name).rows}
    numbers = den_numbers(stats)
    names = []
    next_super = max(int(r["hcIdx"]) for r in supers.rows if r.get("hcIdx", "").strip().isdigit()) + 1
    for n, (elite, source, name, code, seal_name, look) in enumerate(ELITES):
        # The seal: a 1x2 item with a quest item's look, kept out of the rest of the game.
        if code in taken:
            raise RuntimeError(f"item code {code} is taken")
        seal = misc.add_copy("code", look, code=code, name=seal_name, namestr=f"dd{code}", spawnable="0",
                             quest="0", questdiffcheck="0", level="1", levelreq="0")
        for col in ("invwidth", "invheight"):
            if col in seal:
                seal[col] = "1" if col == "invwidth" else "2"
        items.append({code: dict(assets[look])})
        names.append((f"dd{code}", seal_name))

        # Its loot table: the seal, always, first (negative picks drop the entries in order).
        tc_name = f"DD Tower Seal {n + 1}"
        tc = {k: "" for k in tcs.rows[0] if k != "__width__"}
        tc.update({"Treasure Class": tc_name, "Picks": "-1", "NoDrop": "0", "Item1": code, "Prob1": "1"})
        tcs.rows.append(tc)

        # The elite's own monster type (the console's superunique may drop from its type's table).
        row = stats.add_copy("Id", source, Id=elite, NextInClass="", **{"*hcIdx": str(next_class(stats))})
        for col, value in numbers.items():
            if row.get(col, "") != "":
                row[col] = value
        for c in stats.columns:
            if c.startswith("TreasureClass"):
                row[c] = tc_name
        models[elite] = models[source]

        sup = dict(supers.first("Superunique", "Bishibosh"))
        sup.update({"Superunique": name, "Name": f"dd{elite}", "Class": elite, "hcIdx": str(next_super + n),
                    "Mod1": ELITE_MODS[0], "Mod2": ELITE_MODS[1], "Mod3": "0", "MinGrp": "2", "MaxGrp": "3",
                    "TC": tc_name, "TC(N)": tc_name, "TC(H)": tc_name})
        for col in ("TC Desecrated", "TC(N) Desecrated", "TC(H) Desecrated"):
            if col in sup:
                sup[col] = tc_name
        supers.rows.append(sup)
        names.append((f"dd{elite}", name))
        # The plugin spawns the elite with "cheat monster super:<row>" and watches for the seal's code.
        t.loot_lines.append(f"towerelite {n} {next_super + n} {code}")

    if t.excel.name != "excel":     # the base copy only keeps tables; strings are shipped once
        return
    for path, keys in (("local/lng/strings/item-names.json", [k for k in names if k[0].startswith("ddz")]),
                       ("local/lng/strings/monsters.json", [k for k in names if not k[0].startswith("ddz")])):
        strings = t.data(path)
        langs = [k for k in strings[0] if k not in ("id", "Key")]
        for offset, (key, text) in enumerate(keys):
            if not any(e.get("Key") == key for e in strings):
                strings.append({"id": STRING_ID + offset + (0 if "item" in path else 10),
                                "Key": key, **{lang: text for lang in langs}})
