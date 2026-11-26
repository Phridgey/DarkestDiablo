"""A hero's first Tree of Inifuss run: Rakanishu and Treehead each drop a sure unique (2026-11-25).

Later runs keep their set-or-unique (035_inifuss_loot.py). Each boss gets one more superunique
copy ("<name> First"), the same monster with his own loot table whose first, forced pick is a
unique-only bonus: only bases that have a unique at the arena's item level (a base with only a set
item is left out). The plugin spawns that copy instead when the hero has never cleared the Tree
("superfirst <row> <copy row>" in d2rloader/loot_data.txt). Runs after 998_superunique_copies.py.
"""
import importlib.util
from pathlib import Path

_spec = importlib.util.spec_from_file_location("inifuss_loot", Path(__file__).with_name("035_inifuss_loot.py"))
inifuss = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(inifuss)

BOSSES = ((inifuss.RAKANISHU, "Rak"), (inifuss.TREEHEAD, "Treehead"))


def unique_bases(t, item_level):
    quest = {r.get("code") for name in ("weapons", "armor", "misc") for r in t.table(name).rows
             if r.get("quest", "").strip() not in ("", "0")}
    codes = []
    for row in t.table("uniqueitems").rows:
        lvl = row.get("lvl", "").strip()
        if (lvl.isdigit() and int(lvl) <= item_level and row.get("code") and row.get("spawnable") == "1"
                and row["code"] not in quest and row.get("disabled", "") in ("", "0")
                and not row.get("firstLadderSeason", "").strip() and not row.get("carry1", "").strip()):
            row["nolimit"] = "1"
            if row["code"] not in codes:
                codes.append(row["code"])
    if not codes:
        raise RuntimeError(f"No unique can drop at item level {item_level} for the Tree's first run.")
    return codes


def patch(t):
    loot = inifuss.Loot(t.table("treasureclassex"))
    supers = t.table("superuniques")
    arena = t.table("levels").first("Id", inifuss.ARENA_ID)
    copies = {}
    for name, short in BOSSES:
        original = supers.first("Superunique", name)
        number = max(int(r["hcIdx"]) for r in supers.rows if r.get("hcIdx", "").strip().isdigit()) + 1
        supers.add_copy("Superunique", name, Superunique=f"{name} First", hcIdx=str(number))
        copies[name] = supers.first("Superunique", f"{name} First")
        t.loot_lines.append(f"superfirst {original['hcIdx']} {number}")
    for tc_col, lvl_col, tag in (("TC", "MonLvlEx", ""), ("TC(N)", "MonLvlEx(N)", " (N)"), ("TC(H)", "MonLvlEx(H)", " (H)")):
        item_level = int(arena[lvl_col]) + inifuss.SUPERUNIQUE_LEVEL_BONUS
        bonus = loot.code_tree(f"DD Inifuss First{tag}", unique_bases(t, item_level),
                               {q: "1024" for q in inifuss.QUALITIES})
        for name, short in BOSSES:
            row = copies[name]
            own = dict(loot.by_name[row[tc_col]])  # his table: the bonus first, then his drops
            own["Treasure Class"] = f"{row[tc_col]} 1st"
            if len(own["Treasure Class"]) > inifuss.MAX_NAME:
                raise RuntimeError(f"{own['Treasure Class']}: loot table name over {inifuss.MAX_NAME} characters")
            own["Item1"] = bonus
            loot.tcs.rows.append(own)
            loot.by_name[own["Treasure Class"]] = own
            row[tc_col] = own["Treasure Class"]
