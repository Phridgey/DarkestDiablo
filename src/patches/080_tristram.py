"""Tristram (forced stop after fork 1): the game's own Tristram map and Deckard Cain's rescue,
fought as waves (the plugin, src/plugin/darkestdiablo_plugin.cpp) ending with Griswold. The stop
counts as cleared when Griswold dies.

- Map: src/maps/tristram.json, the game's Tristram with its own monster spots left out; the level
  spawns no monsters of its own either. The waves are the only monsters.

- Magic find bonus for the whole zone (MF_BONUS): the waves' monsters are copies of Tristram's
  ("dd_tris_<id>") whose loot tables are copies of theirs with the unique, set, rare and magic
  ratios raised. A table ratio of r makes every one of those qualities (1 - r/1024) times as
  hard to roll, the same as the hero having 1 + MF_BONUS times the magic find on top of their own.
- Griswold: 6 white weapon or armor drops (the Mausoleum's always-normal copies, so never magic;
  they can roll sockets and ethereal). A monster drops at most 6 items, so none of his own loot
  is left (it stays listed after the whites, in case the cap is ever raised).
"""

TRISTRAM_ID = "38"                  # "Act 1 - Tristram"
MAP_FILE = "Act1/Tristram/darkest_tristram.ds1"
NO_MONSTERS = ("MonDen", "MonDen(N)", "MonDen(H)", "MonUMin", "MonUMax", "MonUMin(N)", "MonUMax(N)", "MonUMin(H)", "MonUMax(H)")
GRISWOLD = "Griswold"               # superuniques.txt row
GRISWOLD_LEVEL_BONUS = 3            # superuniques are 3 levels above the area
MF_BONUS = 0.40                     # +40% magic find in the zone
WHITES = 6
BASE_BAND = 24                      # white bases up to this many levels under his item level
MONSTER_LISTS = [f"{kind}{k}" for kind in ("mon", "nmon", "umon") for k in range(1, 11)]


def patch(t):
    ratio = str(round(1024 * (1 - 1 / (1 + MF_BONUS))))
    tcs = t.table("treasureclassex")
    template = tcs.rows[0]
    names = {r["Treasure Class"] for r in tcs.rows}
    boosted = {}

    def boost(name):
        """A copy of loot table `name` with the zone's magic find ratios ("DD Tris ...")."""
        if not name or name not in names:
            return name
        if name not in boosted:
            row = dict(tcs.first("Treasure Class", name))
            row["Treasure Class"] = f"DD Tris {name}"
            # Out of the game's level groups: a copy in the original's group could be picked for
            # other monsters when the game moves a table up by monster level.
            row["group"], row["level"] = "", ""
            for col in ("Unique", "Set", "Rare", "Magic"):
                row[col] = str(max(int(row[col]) if row.get(col, "").isdigit() else 0, int(ratio)))
            tcs.rows.append(row)
            names.add(row["Treasure Class"])
            boosted[name] = row["Treasure Class"]
        return boosted[name]

    def table(name, picks, entries):
        r = {k: "" for k in template if k != "__width__"}
        r.update({"Treasure Class": name, "Picks": str(picks), "NoDrop": "0"})
        for k, (item, prob) in enumerate(entries, 1):
            r[f"Item{k}"], r[f"Prob{k}"] = item, str(prob)
        tcs.rows.append(r)
        names.add(name)
        return name

    # The map without the game's monster spots, and no random monsters
    levels = t.table("levels")
    tristram = levels.first("Id", TRISTRAM_ID)
    for col in NO_MONSTERS:
        tristram[col] = "0"
    prest = t.table("lvlprest").first("LevelId", TRISTRAM_ID)
    prest["File1"] = MAP_FILE

    # The waves' monsters
    stats = t.table("monstats")
    models = t.data("hd/character/monsters.json")
    copies = {}
    for col in MONSTER_LISTS:
        source = tristram.get(col, "")
        if not source:
            continue
        if source not in copies:
            new_id = f"dd_tris_{source}"
            row = stats.add_copy("Id", source, Id=new_id, NextInClass="")
            for c in stats.columns:
                if c.startswith("TreasureClass"):
                    row[c] = boost(row.get(c, ""))
            models[new_id] = models[source]  # without this the monster is invisible
            copies[source] = new_id
        tristram[col] = copies[source]

    # Griswold: 5 whites first, then his own (boosted) loot
    sus = t.table("superuniques")
    griswold = sus.first("Superunique", GRISWOLD)
    area = [int(tristram[c]) for c in ("MonLvlEx", "MonLvlEx(N)", "MonLvlEx(H)")]
    whites = white_bases(t)
    for d, col in enumerate(("TC", "TC(N)", "TC(H)")):
        top = area[d] + GRISWOLD_LEVEL_BONUS
        codes = [code for code, level in whites if top - BASE_BAND < level <= top]
        if not codes:
            raise RuntimeError(f"No white bases for Griswold at item level {top}.")
        tag = ["", " (N)", " (H)"][d]
        white = nested(table, f"DD Griswold White{tag}", codes)
        griswold[col] = table(f"DD Griswold{tag}", -(WHITES + 3), [(white, WHITES), (boost(griswold[col]), 1)])


def white_bases(t):
    """(code, level) of the always-white copies made by 060_mausoleum.py."""
    kinds = {r.get("Code") for r in t.table("itemtypes").rows if r.get("ItemType", "").startswith("DD White ")}
    out = []
    for name in ("weapons", "armor"):
        for row in t.table(name).rows:
            if row.get("type") in kinds and row.get("level", "").isdigit():
                out.append((row["code"], int(row["level"])))
    return out


def nested(table, name, items):
    """A loot table picking evenly among `items`, split into sub-tables of 10 at most."""
    if len(items) <= 10:
        return table(name, 1, [(i, 1) for i in items])
    size = -(-len(items) // 10)
    parts = [items[k:k + size] for k in range(0, len(items), size)]
    return table(name, 1, [(nested(table, f"{name} {n + 1}", part), len(part)) for n, part in enumerate(parts)])
