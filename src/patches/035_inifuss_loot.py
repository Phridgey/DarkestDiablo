"""Tree of Inifuss loot (the MF run): every drop is 1.4 times as likely to be magic, rare, set
or unique, on top of (multiplied with) the player's own magic find. No keys, and Treehead
WoodFist always drops one extra set or unique item.

This lives in the loot tables. A treasure class's Unique/Set/Rare/Magic values ("quality
ratios", out of 1024) shrink the quality roll's range to (1024 - ratio) / 1024, separately from
the player's magic find, so the chance of that quality is multiplied by 1024 / (1024 - ratio).
For 1.4 times the chance, the new ratio is 1024 - (1024 - old ratio) / 1.4: 0 becomes 293 and
800 becomes 864 (a chance already at 100% stays there). When a table calls other tables, the
game uses the highest ratio along the way, so each copy starts from that highest ratio.

Every monster in the arena is a copy of its normal type ("dd_mf_<id>") that drops from these
copied tables, so nothing outside the arena changes. Treehead is his own type (dd_treehead) so
the plugin can see him die (the arena waits for him); his own loot table adds the extra set or
unique (see the end of patch()). Every new monster Id is mapped to its model in monsters.json.
"""

CHANCE_FACTOR = 1.4                 # magic/rare/set/unique chance multiplier for the whole run
CHARM_FACTOR = 3                    # charms 3 times as likely (+200%) in MF runs (more_charms)
CHARM_SHARE = 6 / 20                # charms' share of the game's Jewelry tables
ARENA_ID = "11"                     # Tree of Inifuss arena (030_inifuss.py)
TREEHEAD_TYPE = "dd_treehead"
TREEHEAD_CLASS = 752                # TreeheadClass in src/plugin/darkestdiablo_plugin.cpp
RAKANISHU_TYPE = "dd_rakanishu"
RAKANISHU_CLASS = 759               # RakanishuClass in the plugin (after the 6 dd_mf_ copies)
SUPERUNIQUE_LEVEL_BONUS = 0         # a superunique spawned by the plugin is at the area's level (as the
                                    # Pit's uniques, 090_pit.py): with +3 a picked base's unique could be
                                    # above his item level and fall back to a rare (2026-11-21)
# Normal monster types in the arena's waves, and Rakanishu's (fallen2). Treehead is a brute2.
WAVE_TYPES = ["fallen1", "fallenshaman1", "cr_lancer1", "brute2", "quillrat1", "fallen2"]
RAKANISHU, TREEHEAD = "Rakanishu", "Treehead WoodFist"
QUALITIES = ("Unique", "Set", "Rare", "Magic")
MAX_NAME = 44  # longest loot table name we make; "DD Gold3 DD Charm Act 2 Worldstone Shard Parent
               # Citem A" (55) failed to load: "Couldn't parse treasure class item" (2026-11-21)


def ratio_times(ratio, factor=CHANCE_FACTOR):
    """The quality ratio (0-1024) that makes this quality `factor` times as likely as `ratio` does."""
    return min(1024, round(1024 - (1024 - ratio) / factor))


class Loot:
    def __init__(self, tcs):
        self.tcs = tcs
        self.by_name = {r["Treasure Class"]: r for r in tcs.rows}
        self.copies = {}

    def items(self, row):
        return [(k, row.get(f"Item{k}", "")) for k in range(1, 11) if row.get(f"Item{k}")]

    def is_tc(self, item):
        return item.split(",")[0] in self.by_name

    def reaches(self, name, item_code, seen=None):
        """Does this treasure class (or one it calls) drop `item_code`?"""
        seen = seen if seen is not None else set()
        if name in seen or name not in self.by_name:
            return False
        seen.add(name)
        for _, item in self.items(self.by_name[name]):
            base = item.split(",")[0]
            if base == item_code or (self.is_tc(base) and self.reaches(base, item_code, seen)):
                return True
        return False

    def highest(self, name, quality, seen=None):
        """The highest quality ratio along every path through this treasure class."""
        seen = seen if seen is not None else set()
        if name in seen or name not in self.by_name:
            return 0
        seen.add(name)
        row = self.by_name[name]
        best = int(row.get(quality) or 0)
        for _, item in self.items(row):
            if self.is_tc(item):
                best = max(best, self.highest(item.split(",")[0], quality, seen))
        return best

    def copy(self, name, prefix, change):
        key = (prefix, name)
        if key in self.copies:
            return self.copies[key]
        row = dict(self.by_name[name])
        row["Treasure Class"] = f"{prefix} {name}"
        if len(row["Treasure Class"]) > MAX_NAME:  # the game can't read longer names (a copy of a copy)
            row["Treasure Class"] = f"{prefix} #{len(self.copies)}"
        self.copies[key] = row["Treasure Class"]
        # Out of the game's level groups: a copy left in its original's group could be picked for
        # other monsters when the game moves a table up by monster level.
        row["group"], row["level"] = "", ""
        kept = []
        for k, item in self.items(row):
            base = item.split(",")[0]
            if base == "key":
                continue  # no keys anywhere
            if self.is_tc(base) and self.reaches(base, "key"):
                item = self.without_keys(base) + item[len(base):]
            kept.append((item, row.get(f"Prob{k}", "")))
        for k in range(1, 11):
            row[f"Item{k}"], row[f"Prob{k}"] = kept[k - 1] if k <= len(kept) else ("", "")
        change(row)
        self.tcs.rows.append(row)
        self.by_name[row["Treasure Class"]] = row
        return row["Treasure Class"]

    def code_tree(self, name, codes, ratios):
        """A table that drops one of `codes`, each equally likely. A table holds 10 entries, so
        longer lists become sub-tables ("<name> 1", ...) weighted by how many codes they hold."""
        if not codes:
            raise RuntimeError(f"{name}: no item codes to drop")
        template = next(iter(self.by_name.values()))
        row = {k: "" for k in template if k != "__width__"}  # __width__ is the build's own bookkeeping
        row.update({"Treasure Class": name, "Picks": "1", "NoDrop": "0", **ratios})
        if len(codes) <= 10:
            entries = [(c, 1) for c in codes]
        else:
            size = -(-len(codes) // 10)  # ceil: at most 10 groups
            groups = [codes[i:i + size] for i in range(0, len(codes), size)]
            # The sub-tables carry the same quality chances: the game may take them from the table
            # the item is picked in (2026-11-25: 1 unique from 2 runs' 4 bonus picks).
            entries = [(self.code_tree(f"{name} {n}", g, ratios), len(g)) for n, g in enumerate(groups, 1)]
        for k, (item, prob) in enumerate(entries, 1):
            row[f"Item{k}"], row[f"Prob{k}"] = item, str(prob)
        self.tcs.rows.append(row)
        self.by_name[name] = row
        return name

    def without_keys(self, name):
        return self.copy(name, "DD NoKey", lambda row: None)

    def charms_table(self):
        """One charm (small, large or grand, equally likely): the extra charm pick of MF runs."""
        if "DD Charms" not in self.by_name:
            self.code_tree("DD Charms", ["cm1", "cm2", "cm3"], {})
        return "DD Charms"

    def more_charms(self, row):
        """MF runs: charms CHARM_FACTOR times as likely. Charms only come from the "Jewelry"
        tables (6 of their 20 weight: rings, amulets and jewels are the rest). Next to each Jewelry
        entry goes an extra charms entry worth (CHARM_FACTOR - 1) times the Jewelry entry's charm
        share, so charms drop CHARM_FACTOR times as often while rings, amulets, jewels and the
        table's other drops stay close to their old odds. Tables on the way to Jewelry are copied
        ("DD Charm ...") so the change stays in the run."""
        entries = self.items(row)
        jewelry = [(k, item) for k, item in entries if item.split(",")[0].startswith("Jewelry")]
        for k, item in entries:
            base = item.split(",")[0]
            if not base.startswith("Jewelry") and self.is_tc(base) and self.reaches(base, "cm1"):
                row[f"Item{k}"] = self.with_charms(base) + item[len(base):]
        if not jewelry:
            return
        if len(entries) + len(jewelry) > 10:
            raise RuntimeError(f"{row['Treasure Class']}: no room for the extra charm entry")
        scale = 5  # weights x5 so the extra entry is a whole number
        for k, _ in entries:
            if row.get(f"Prob{k}", "").isdigit():
                row[f"Prob{k}"] = str(int(row[f"Prob{k}"]) * scale)
        if row.get("NoDrop", "").isdigit():
            row["NoDrop"] = str(int(row["NoDrop"]) * scale)
        slot = len(entries) + 1
        for k, _ in jewelry:
            extra = round(int(row[f"Prob{k}"]) * CHARM_SHARE * (CHARM_FACTOR - 1))
            row[f"Item{slot}"], row[f"Prob{slot}"] = self.charms_table(), str(extra)
            slot += 1

    def with_charms(self, name):
        if not name or name not in self.by_name or not self.reaches(name, "cm1"):
            return name
        return self.copy(name, "DD Charm", self.more_charms)

    def with_mf(self, name):
        if not name or name not in self.by_name:
            return name
        ratios = {q: ratio_times(self.highest(name, q)) for q in QUALITIES}

        def boost(row):
            for q, value in ratios.items():
                row[q] = str(value)
            self.more_charms(row)
        return self.copy(name, "DD MF", boost)


def patch(t):
    stats = t.table("monstats")
    loot = Loot(t.table("treasureclassex"))
    tc_columns = [c for c in stats.columns if c.startswith("TreasureClass")]

    models = t.data("hd/character/monsters.json")  # D2R's Id -> 3D model map; new Ids need an entry

    def mf_type(source, new_id, **changes):
        row = stats.add_copy("Id", source, Id=new_id, NextInClass="", **changes)
        for c in tc_columns:
            row[c] = loot.with_mf(row[c])
        models[new_id] = models[source]  # without this the monster is invisible
        return row

    numbered = [r for r in stats.rows if r.get("*hcIdx", "").strip().isdigit()]
    if len(numbered) != TREEHEAD_CLASS:
        raise RuntimeError(f"monstats has {len(numbered)} rows before Treehead's, so his type would be row "
                           f"{len(numbered)}, not {TREEHEAD_CLASS}. Update TREEHEAD_CLASS here and in the plugin.")
    mf_type("brute2", TREEHEAD_TYPE, minion1="dd_mf_brute2", **{"*hcIdx": str(TREEHEAD_CLASS)})
    for source in WAVE_TYPES:
        minion = stats.first("Id", source).get("minion1", "")
        mf_type(source, f"dd_mf_{source}", minion1=f"dd_mf_{minion}" if minion in WAVE_TYPES else minion)
    # Rakanishu gets a type of his own too (his Carvers stay dd_mf_fallen2): the arena is won only
    # when both he and Treehead are dead, so the plugin must see him die.
    numbered = [r for r in stats.rows if r.get("*hcIdx", "").strip().isdigit()]
    if len(numbered) != RAKANISHU_CLASS:
        raise RuntimeError(f"Rakanishu's type would be row {len(numbered)}, not {RAKANISHU_CLASS}. "
                           "Update RAKANISHU_CLASS here and in the plugin.")
    mf_type("fallen2", RAKANISHU_TYPE, minion1="dd_mf_fallen2", **{"*hcIdx": str(RAKANISHU_CLASS)})

    supers = t.table("superuniques")
    for name, cls in ((RAKANISHU, RAKANISHU_TYPE), (TREEHEAD, TREEHEAD_TYPE)):
        row = supers.first("Superunique", name)
        row["Class"] = cls
        for c in supers.columns:
            if c.startswith("TC"):
                row[c] = loot.with_mf(row[c])

    # Treehead's and Rakanishu's extra set or unique (both, decided 2026-11-22), each from his own
    # loot table (the game's normal drop path; making it in the plugin crashed the game). For each
    # difficulty his table is a copy of the shared Rakanishu/Treehead table plus one forced pick
    # from a bonus table: every base item with a spawnable unique or set at or below the arena's
    # item level, unique and set chance at the maximum. Those uniques may drop again in the same
    # game (nolimit), so the pick is a unique, or a set for a base with only a set item
    # (bonus_bases); never the game's rare fallback.
    arena = t.table("levels").first("Id", ARENA_ID)
    for tc_col, lvl_col, tag in (("TC", "MonLvlEx", ""), ("TC(N)", "MonLvlEx(N)", " (N)"), ("TC(H)", "MonLvlEx(H)", " (H)")):
        item_level = int(arena[lvl_col]) + SUPERUNIQUE_LEVEL_BONUS
        bonus = loot.code_tree(f"DD Treehead Bonus{tag}", bonus_bases(t, item_level),
                               {q: "1024" for q in QUALITIES})
        for name, own_type, short in ((TREEHEAD, TREEHEAD_TYPE, "Treehead"), (RAKANISHU, RAKANISHU_TYPE, "Rak")):
            boss = supers.first("Superunique", name)
            own = dict(loot.by_name[boss[tc_col]])
            own["Treasure Class"] = f"{boss[tc_col]} {short}"
            if len(own["Treasure Class"]) > MAX_NAME:
                raise RuntimeError(f"{own['Treasure Class']}: loot table name over {MAX_NAME} characters")
            picks = int(own["Picks"])
            if picks >= 0:
                raise RuntimeError(f"{boss[tc_col]} has Picks {picks}; the forced extra pick needs a negative value.")
            own["Picks"] = str(picks - 1)  # negative Picks: every listed item drops Prob times
            put_first(own, bonus, "1")  # first, so the 6-item drop cap can't cut it
            loot.tcs.rows.append(own)
            loot.by_name[own["Treasure Class"]] = own
            boss[tc_col] = own["Treasure Class"]
            # A superunique spawned by the plugin ("cheat monster super:4") may drop from its monster
            # type's columns rather than this row, so his type (his alone) drops the same table.
            suffix = tag.strip()
            for c in tc_columns:
                if c.endswith(suffix) if suffix else not c.endswith(")"):
                    stats.first("Id", own_type)[c] = own["Treasure Class"]

def bonus_bases(t, item_level):
    """Item codes of base items that have a unique or a set item at or below his item level.
    The pick is a unique when the base has one: those uniques get "nolimit" = 1, so they drop again
    in a game where one already dropped (without it the game falls back to a set, then rare). A
    base with only a set item gives the set. Quest items (Horadric Staff, Khalim's Flail, ...) and
    ladder-only or charm-like ("carry1") items are left out."""
    quest_bases = {r.get("code") for name in ("weapons", "armor", "misc") for r in t.table(name).rows
                   if r.get("quest", "").strip() not in ("", "0")}

    def droppable(row, code_col):
        lvl = row.get("lvl", "").strip()
        return (lvl.isdigit() and int(lvl) <= item_level and row.get(code_col) and row.get("spawnable") == "1"
                and row.get(code_col) not in quest_bases and row.get("disabled", "") in ("", "0")
                and not row.get("firstLadderSeason", "").strip() and not row.get("carry1", "").strip())

    codes = []
    for row in t.table("uniqueitems").rows:
        if droppable(row, "code"):
            row["nolimit"] = "1"
            if row["code"] not in codes:
                codes.append(row["code"])
    for row in t.table("setitems").rows:
        if droppable(row, "item") and row["item"] not in codes:
            codes.append(row["item"])
    if not codes:
        raise RuntimeError(f"No unique or set can drop at item level {item_level}; Treehead's bonus pick would fail.")
    return codes


def put_first(row, item, prob):
    """Insert a loot table entry at the top, moving the others down. A monster drops at most 6
    items per death, and with negative Picks the entries drop in order: an entry listed last
    after 6 items' worth (2 item picks + 2 picks of 2 potions) never drops at all."""
    entries = [(row[f"Item{k}"], row[f"Prob{k}"]) for k in range(1, 11) if row.get(f"Item{k}")]
    entries.insert(0, (item, prob))
    if len(entries) > 10:
        raise RuntimeError(f"{row['Treasure Class']}: more than 10 entries")
    for k in range(1, 11):
        row[f"Item{k}"], row[f"Prob{k}"] = entries[k - 1] if k <= len(entries) else ("", "")
