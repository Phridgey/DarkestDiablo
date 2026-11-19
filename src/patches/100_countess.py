"""Countess (fork 2 crafting hunt): the game's own Tower Cellar level 5 (level 25), fought as
waves (like every stop, so quitting can't bring the monsters back to farm): the cellar's own
monster types around the hero, then the Countess. The stop counts as cleared when she dies (the
plugin knows her by her own type, "targetclass" in d2rloader/loot_data.txt).

- Map: src/maps/countess.json, one of the cellar's two versions with its monster spots and
  chests left out; the level spawns no monsters of its own either.
- The cellar is reached by the route map and left by town portal. Its stairs up still work (a
  map with dead stairs crashes the game), but the floors above have no monsters.
- The Countess drops 5 runes from Ith to Amn, then 1 pick of her usual item loot (a monster
  drops 6 items at most). The odds between the runes are the game's own (its "Runes 6" table,
  Ith to Amn), except Amn, held to AMN_CHANCE.
- She gets her own monster type ("dd_countess", a copy of her base type) so her death can be
  told apart from the other rogues'.
"""

ARENA_ID = "25"                     # "Act 1 - Crypt 3 E", Tower Cellar level 5
MAP_FILE = "Act1/Crypt/darkest_countess.ds1"
COUNTESS = "The Countess"           # superuniques.txt row
COUNTESS_TYPE = "dd_countess"
RUNES = ["r06", "r07", "r08", "r09", "r10", "r11"]  # Ith .. Amn
AMN_CHANCE = 0.03
RUNE_DROPS = 5
UPPER_FLOORS = ["21", "22", "23", "24"]  # Tower Cellar levels 1-4
NO_MONSTERS = ("MonDen", "MonDen(N)", "MonDen(H)", "MonUMin", "MonUMax", "MonUMin(N)", "MonUMax(N)", "MonUMin(H)", "MonUMax(H)")


def rune_shares(t):
    """Each rune's share when the game's "Runes 6" table picks one, among Ith .. Amn only."""
    tcs = {r["Treasure Class"]: r for r in t.table("treasureclassex").rows}
    out = {}

    def walk(name, share):
        row = tcs[name]
        entries = [(row[f"Item{k}"], int(row[f"Prob{k}"])) for k in range(1, 11)
                   if row.get(f"Item{k}") and row.get(f"Prob{k}", "").isdigit()]
        total = sum(p for _, p in entries)
        for item, prob in entries:
            if item.startswith("Runes "):
                walk(item, share * prob / total)
            else:
                out[item] = out.get(item, 0.0) + share * prob / total
    walk("Runes 6", 1.0)
    shares = {r: out.get(r, 0.0) for r in RUNES}
    rest = sum(v for r, v in shares.items() if r != "r11")
    return {r: (AMN_CHANCE if r == "r11" else v / rest * (1 - AMN_CHANCE)) for r, v in shares.items()}


def patch(t):
    # The cellar's stairs up stay linked: its map has stair tiles, and a map whose stairs lead
    # nowhere crashes the game on arrival (BC_VERIFY ptWarp). Instead the floors above are empty,
    # so going up gains nothing.
    levels = t.table("levels")
    for level_id in UPPER_FLOORS + [ARENA_ID]:
        row = levels.first("Id", level_id)
        for col in NO_MONSTERS:
            row[col] = "0"

    prest = t.table("lvlprest").first("LevelId", ARENA_ID)
    prest["Files"], prest["File1"], prest["File2"] = "1", MAP_FILE, "0"

    tcs = t.table("treasureclassex")
    template = tcs.rows[0]

    def table(name, picks, entries):
        r = {k: "" for k in template if k != "__width__"}
        r.update({"Treasure Class": name, "Picks": str(picks), "NoDrop": "0"})
        for k, (item, prob) in enumerate(entries, 1):
            r[f"Item{k}"], r[f"Prob{k}"] = item, str(prob)
        tcs.rows.append(r)
        return name

    runes = table("DD Countess Runes", 1, [(r, round(share * 1000)) for r, share in rune_shares(t).items()])

    # Her own type, so the plugin sees her die
    stats = t.table("monstats")
    supers = t.table("superuniques")
    countess = supers.first("Superunique", COUNTESS)
    numbered = len([r for r in stats.rows if r.get("*hcIdx", "").strip().isdigit()])
    # Her minions stay her base type ("minion1"): left to her own type, they would be dd_countess
    # too, and the first one killed would end the stop.
    base = countess["Class"]
    stats.add_copy("Id", base, Id=COUNTESS_TYPE, NextInClass="", minion1=stats.first("Id", base).get("minion1") or base,
                   **{"*hcIdx": str(numbered)})
    models = t.data("hd/character/monsters.json")
    models[COUNTESS_TYPE] = models[base]
    countess["Class"] = COUNTESS_TYPE
    t.loot_lines.append(f"targetclass {ARENA_ID} {numbered}")

    for col, tag in (("TC", ""), ("TC(N)", " (N)"), ("TC(H)", " (H)")):
        item = f"Countess Item{tag}"
        countess[col] = table(f"DD Countess{tag}", -(RUNE_DROPS + 1), [(runes, RUNE_DROPS), (item, 1)])
