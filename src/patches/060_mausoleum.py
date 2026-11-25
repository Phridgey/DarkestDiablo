"""Mausoleum (fork 1 crafting hunt): a round crypt hall with waves of undead whose loot is only
runes, gems, potions and gold; never a magic item (docs/design.md, "Crafting hunt").

- Arena: "Cave 3 Treasure" (level 14), fixed at (1500, 2624) like the other arenas, with the
  Mausoleum's look, sound and name (level 19). Map: src/maps/mausoleum.json (built with
  compose_map's "circle": Blizzard has no round crypt room).
- The player arrives by waypoint at the hall's west edge: Act 1 waypoint slot 4 (actinfo
  waypoint5), taken from the Black Marsh.
- Its monsters are copies of the Mausoleum's undead ("dd_craft_<id>") that drop only from the
  crafting tables below, so nothing outside the arena changes. Their models come from monsters.json.
- Loot, all from the game's loot tables (see crafting_loot): gems, potions and gold; white
  weapons and armor that can't be magic; and runes at a chance that follows the hero's magic
  find (RUNE_CURVE: 5% at 0 MF, 10% at 250 MF and on up the same way, 100% at most).
"""

ARENA_ID = "14"                     # "Act 1 - Cave 3 Treasure", reused
MAUSOLEUM_ID = "19"                 # "Act 1 - Crypt 2 A", the Mausoleum
MAP_FILE = "Act1/Crypt/darkest_mausoleum.ds1"
SIZE = ("32", "32")                 # map width - 1, height - 1 (4x4 crypt rooms of 8 tiles)
SLOT = "4"
KEEP = {"Id", "Name", "Layer", "OffsetX", "OffsetY"}
NO_MONSTERS = ("MonDen", "MonDen(N)", "MonDen(H)", "MonUMin", "MonUMax", "MonUMin(N)", "MonUMax(N)", "MonUMin(H)", "MonUMax(H)")
WAVE_TYPES = ["skeleton1", "zombie2", "wraith1", "sk_archer1"]
WHITE_CHANCE = 12                   # percent of kills that also make a white item
RUNE_CURVE = [(0, 5), (250, 10)]     # (magic find %, rune chance per kill %), a straight line
RUNE_STEP = 250                     # magic find per step; between steps the plugin mixes the two
                                    # steps' monsters, so the chance still climbs smoothly
BASE_BAND = 24                      # bases up to this many levels under the item level

# Crafting loot per difficulty: (runes table or its entries, gem tables, potion table). The gems,
# potions and gold drop as one pick per kill (two for champions); runes and white items roll on
# their own (crafting_loot).
CRAFT = {
    # Normal: El to Ral, Act I's highest rune (decided 2026-11-20: A1 Ral, A2 Shael, A3 Lem,
    # A4 Ohm), with the game's own odds: its "Runes 4" table.
    "":     ("Runes 4", ("Chipped Gem", "Flawed Gem"), "Hpotion 1"),
    "(N)":  ("Runes 8", ("Flawed Gem", "Normal Gem"), "Hpotion 3"),
    "(H)":  ("Runes 13", ("Normal Gem", "Flawless Gem"), "Hpotion 5"),
}


def patch(t):
    levels = t.table("levels")
    arena, mausoleum = levels.first("Id", ARENA_ID), levels.first("Id", MAUSOLEUM_ID)
    for col in levels.columns:
        if col not in KEEP:
            arena[col] = mausoleum[col]
    arena["__width__"] = max(arena.get("__width__", 0), mausoleum.get("__width__", 0))
    arena["DrlgType"] = "2"
    for col in ("SizeX", "SizeX(N)", "SizeX(H)"):
        arena[col] = SIZE[0]
    for col in ("SizeY", "SizeY(N)", "SizeY(H)"):
        arena[col] = SIZE[1]
    for col in NO_MONSTERS:
        arena[col] = "0"
    for row in levels.rows:  # nothing leads in or out
        for k in range(8):
            if row is arena or row.get(f"Vis{k}") == ARENA_ID:
                row[f"Vis{k}"], row[f"Warp{k}"] = "0", "-1"

    old = levels.first("Waypoint", SLOT)  # the Black Marsh gives up its slot
    old["Waypoint"] = "255"
    arena["Waypoint"] = SLOT
    t.table("actinfo").first("act", "1")["waypoint5"] = arena["Name"]  # 1-based: slot 4

    prest = t.table("lvlprest")
    for other in prest.rows:  # the level's vanilla preset (a treasure cave) would load instead
        if other.get("LevelId") == ARENA_ID:
            other["LevelId"] = "0"
    row = dict(prest.first("Name", "Act 1 - Crypt NSEW"))
    row["Name"] = "Act 1 - Darkest Diablo Mausoleum"
    row["Def"] = str(max(int(r["Def"]) for r in prest.rows if r["Def"].isdigit()) + 1)
    row["LevelId"] = ARENA_ID
    row["SizeX"], row["SizeY"] = SIZE
    row["Files"] = "1"
    row["File1"] = MAP_FILE
    for k in range(2, 7):
        row[f"File{k}"] = "0"
    prest.rows.append(row)

    crafting_loot(t, arena)


def crafting_loot(t, arena, arena_id=ARENA_ID, wave_types=WAVE_TYPES, craft=CRAFT, name="DD Craft",
                  id_prefix="dd_craft_", gem_to_rune=False):
    """The hunt's loot, all from the game's own loot tables, so it drops on the ground like any
    loot. (The plugin can't make items on the ground: the loader's ground drops crash the game.)

    - Gems, potions and gold: "DD Craft Hunt" as before.
    - White items: copies of the bases near the arena's item level ("z" + the base's code) whose
      item types are always normal quality ("Normal" = 1), so they can never come out magic. They
      still roll sockets and ethereal like any normal drop, and work like the base (same name,
      look and runewords, as their item type counts as the original one). They are not
      "spawnable", so they drop only from these tables, never in the rest of the game.
    - Runes: each monster type comes in one copy per magic find step ("dd_craft_<id>_r<k>"),
      whose loot table drops a rune at that step's chance. The plugin reads the hero's magic find
      when a wave spawns and spawns the matching copies ("craftmf" lines in loot_data.txt).

    With gem_to_rune, the lower gem's share of the gems-and-gold pick drops a rune instead, on top of
    the magic-find rune (Lower Kurast: about 7 runes a run instead of 2, 2026-11-25).

    Other crafting hunts (the Stony Tomb, 130_stony_tomb.py) call this with their own level,
    monsters, loot (`craft`), table names and monster Id prefix; the white copies are made once
    (the first call) and found again after that."""
    tcs = t.table("treasureclassex")
    template = tcs.rows[0]

    def table(name, picks, nodrop, entries):
        r = {k: "" for k in template if k != "__width__"}
        r.update({"Treasure Class": name, "Picks": str(picks), "NoDrop": str(nodrop)})
        for k, (item, prob) in enumerate(entries, 1):
            r[f"Item{k}"], r[f"Prob{k}"] = item, str(prob)
        tcs.rows.append(r)
        return name

    def nested(name, items):
        """A loot table picking evenly among `items`, split into sub-tables of 10 at most."""
        if len(items) <= 10:
            return table(name, 1, 0, [(i, 1) for i in items])
        size = -(-len(items) // 10)
        parts = [items[k:k + size] for k in range(0, len(items), size)]
        return table(name, 1, 0, [(nested(f"{name} {n + 1}", part), len(part)) for n, part in enumerate(parts)])

    item_levels = [int(arena[c]) for c in ("MonLvlEx", "MonLvlEx(N)", "MonLvlEx(H)")]
    made = any(r.get("code", "").startswith("z") and r.get("type", "").startswith("zw") for r in t.table("armor").rows)
    whites = white_codes(t, item_levels) if made else white_copies(t, item_levels)
    steps = rune_steps()
    hunt = {}  # difficulty tag -> {step: (normal table, champion table)}
    for d, (diff, (runes, gems, potion)) in enumerate(craft.items()):
        tag = f" {diff}" if diff else ""
        if not isinstance(runes, str):  # our own rune table
            runes = table(f"{name} Runes{tag}", 1, 0, runes)
        entries = [(gems[0], 10), (gems[1], 4), (potion, 8), ("gld", 12)]
        if gem_to_rune:  # the lower gem's share drops a rune instead (Lower Kurast, 2026-11-25)
            entries[0] = (runes, 10)
        base = table(f"{name} Hunt{tag}", 1, 40, entries)
        base_champ = table(f"{name} Hunt Champ{tag}", 2, 10, entries)
        white = table(f"{name} White{tag}", 1, 100 - WHITE_CHANCE,
                      [(nested(f"{name} White Bases{tag}", whites[d]), WHITE_CHANCE)])
        hunt[diff] = {}
        for k, (_, chance) in enumerate(steps):
            rune = table(f"{name} Rune {k}{tag}", 1, round((100 - chance) * 2), [(runes, round(chance * 2))])
            # Negative picks: each entry drops its count of times, in order.
            hunt[diff][k] = (table(f"{name} Hunt {k}{tag}", -3, 0, [(base, 1), (rune, 1), (white, 1)]),
                             table(f"{name} Hunt Champ {k}{tag}", -4, 0, [(base_champ, 1), (rune, 2), (white, 1)]))

    stats = t.table("monstats")
    models = t.data("hd/character/monsters.json")
    for source in wave_types:
        for k in range(len(steps)):
            new_id = f"{id_prefix}{source}_r{k}"
            row = stats.add_copy("Id", source, Id=new_id, NextInClass="")
            for c in stats.columns:
                if c.startswith("TreasureClass"):
                    diff = "(N)" if c.endswith("(N)") else "(H)" if c.endswith("(H)") else ""
                    normal, champ = hunt[diff][k]
                    row[c] = champ if ("Champ" in c or "Unique" in c) else normal
            models[new_id] = models[source]  # without this the monster is invisible
    for k, (mf, chance) in enumerate(steps):
        t.loot_lines.append(f"craftmf {arena_id} {mf} _r{k} {chance}")


def rune_steps():
    """(magic find %, rune chance %) for each step: RUNE_CURVE's line, every RUNE_STEP of magic
    find, up to 100%."""
    (x1, y1), (x2, y2) = RUNE_CURVE
    steps, mf = [], 0
    while True:
        chance = min(100.0, y1 + (y2 - y1) * (mf - x1) / (x2 - x1))
        steps.append((mf, round(chance * 2) / 2))
        if chance >= 100.0:
            return steps
        mf += RUNE_STEP


def white_codes(t, item_levels):
    """The white copies white_copies already made, per difficulty: those from BASE_BAND levels
    under each item level up to it."""
    per_difficulty = [[] for _ in item_levels]
    for name in ("weapons", "armor"):
        for row in t.table(name).rows:
            code, level = row.get("code", ""), row.get("level", "")
            if code.startswith("z") and row.get("type", "").startswith("zw") and level.isdigit():
                for d, top in enumerate(item_levels):
                    if top - BASE_BAND < int(level) <= top:
                        per_difficulty[d].append(code)
    for d, codes in enumerate(per_difficulty):
        if not codes:
            raise RuntimeError(f"No white bases at item level {item_levels[d]}.")
    return per_difficulty


def white_copies(t, item_levels):
    """Always-white copies ("z" + code) of every weapon and armor base that can drop (other stops
    use them too, e.g. Griswold in Tristram). Returns the codes of those from BASE_BAND levels
    under each item level up to it, per difficulty.

    Both copies of the tables (global/excel and global/excel/base) must get the same new rows in
    the same order: the game mixes up the two copies' row numbers, so an item list that is 8 rows
    longer in one copy showed every later item with the picture of the item 8 rows before it (an
    Identify scroll looked like a ring, Minor Healing Potions like rubies). So which bases get a
    copy is always decided from global/excel. Where the base copy has only Blizzard's empty
    placeholder for a base (the Warlock's grimoires), it gets an empty placeholder copy too, which
    never drops there."""
    main = type(t)()  # global/excel, read fresh
    is_main = t.excel == main.excel
    if is_main:
        main = t
    main_types = main.table("itemtypes")
    by_code = {r.get("Code"): r for r in main_types.rows if r.get("Code")}
    quest = {r.get("code") for name in ("weapons", "armor", "misc") for r in main.table(name).rows
             if r.get("quest", "").strip() not in ("", "0")}
    items = main.data("hd/items/items.json")
    assets = {code: v for entry in items for code, v in entry.items()}

    types = t.table("itemtypes")
    if not is_main:
        # Blizzard's own type rows that only global/excel has (they sit at its end) are copied
        # first, so the new types get the same row numbers in both copies.
        have = {r.get("Code") for r in types.rows}
        vanilla = [r for r in type(t)().table("itemtypes").rows]
        for row in vanilla:
            if row.get("Code") and row["Code"] not in have:
                types.rows.append(dict(row))
        if [r.get("Code") for r in types.rows] != [r.get("Code") for r in vanilla]:
            raise RuntimeError("itemtypes: the two copies differ in more than rows at the end")
    own_types = {r.get("Code"): r for r in types.rows if r.get("Code")}
    taken = {r.get("code") for name in ("weapons", "armor", "misc") for r in t.table(name).rows}

    new_types = {}
    per_difficulty = [[] for _ in item_levels]
    for name in ("weapons", "armor"):
        table = t.table(name)
        for row in list(main.table(name).rows):
            code, level = row.get("code", ""), row.get("level", "")
            if (len(code) != 3 or not level.isdigit() or row.get("spawnable") != "1" or code in quest
                    or row.get("type") not in by_code or code not in assets):
                continue
            # Every base gets its copy (other stops use them too, e.g. Griswold in Tristram);
            # the Mausoleum's tables list the ones near its own item levels.
            wanted = [d for d, top in enumerate(item_levels) if top - BASE_BAND < int(level) <= top]
            kind = row["type"]
            if kind not in new_types:
                # Named by the original type's place in global/excel's itemtypes, so both copies
                # give a type the same code.
                n = list(by_code).index(kind)
                digits = "0123456789abcdefghijklmnopqrstuvwxyz"
                new_code = "zw" + digits[n // 36] + digits[n % 36]  # 4 characters at most
                if new_code in own_types:
                    raise RuntimeError(f"item type code {new_code} is taken")
                copy = dict(own_types[kind])
                copy.update({"ItemType": f"DD White {copy['ItemType']}", "Code": new_code, "Equiv1": kind, "Equiv2": "",
                             "Normal": "1", "Magic": "0", "Rare": "0", "TreasureClass": "0"})
                types.rows.append(copy)
                own_types[new_code] = copy
                new_types[kind] = new_code
            new_code = "z" + code
            if new_code in taken:
                raise RuntimeError(f"item code {new_code} is taken")
            taken.add(new_code)
            own = table.first("code", code)
            copy = dict(own)
            copy["code"] = new_code
            copy["spawnable"] = "0"
            if own.get("type"):
                copy["type"] = new_types[kind]
                for d in wanted:
                    per_difficulty[d].append(new_code)
            # else: Blizzard's empty placeholder row in the base copy; it stays empty and never drops.
            table.rows.append(copy)
            if is_main:
                items.append({new_code: dict(assets[code])})
    for d, codes in enumerate(per_difficulty):
        if not codes:
            raise RuntimeError(f"No white bases for the Mausoleum at item level {item_levels[d]}.")
    return per_difficulty
