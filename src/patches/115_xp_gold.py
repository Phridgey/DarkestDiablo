"""XP grinds drop three times the gold (+200%, decided 2026-11-20): the Crypt (070_crypt.py,
"dd_xp_<id>") and the Malus (110_malus.py, "dd_malus_<id>").

Each of their loot tables that can drop gold is copied, with every gold pile worth 3 times as much
("gld" -> "gld,mul=768", "gld,mul=1280" -> "gld,mul=3840"; mul counts in 256ths, so 256 is the normal amount). How often gold drops,
and everything else they drop, is unchanged.
"""
import importlib.util
from pathlib import Path

GOLD_FACTOR = 3
PREFIXES = ("dd_xp_", "dd_malus_")

_spec = importlib.util.spec_from_file_location("inifuss_loot", Path(__file__).with_name("035_inifuss_loot.py"))
inifuss = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(inifuss)


def plain(item):
    """Entries with a comma are quoted in the game's files ('"gld,mul=1280"')."""
    return item.strip('"')


def more_gold(item):
    parts = plain(item).split(",")
    mul = 256
    rest = []
    for p in parts[1:]:
        if p.startswith("mul="):
            mul = int(p[4:])
        else:
            rest.append(p)
    return '"' + ",".join([parts[0], *rest, f"mul={mul * GOLD_FACTOR}"]) + '"'


def patch(t):
    stats = t.table("monstats")
    loot = inifuss.Loot(t.table("treasureclassex"))

    def has_gold(name, seen=None):
        seen = seen if seen is not None else set()
        if name in seen or name not in loot.by_name:
            return False
        seen.add(name)
        for _, item in loot.items(loot.by_name[name]):
            base = plain(item).split(",")[0]
            if base == "gld" or (loot.is_tc(base) and has_gold(base, seen)):
                return True
        return False

    def gold_copy(name):
        if not has_gold(name):
            return name

        def change(row):
            for k, item in loot.items(row):
                base = plain(item).split(",")[0]
                if base == "gld":
                    row[f"Item{k}"] = more_gold(item)
                elif loot.is_tc(base) and has_gold(base):
                    row[f"Item{k}"] = gold_copy(base) + item[len(base):]

        return loot.copy(name, "DD Gold3", change)

    columns = [c for c in stats.columns if c.startswith("TreasureClass")]
    for row in stats.rows:
        if row.get("Id", "").startswith(PREFIXES):
            for c in columns:
                if row.get(c) and row[c] in loot.by_name:
                    row[c] = gold_copy(row[c])
