"""Runs last: tells the plugin how items are stored in save files, as lines in
d2rloader/loot_data.txt, so Ascend can lift an ascended hero's items out of their save (ascend.h):
"isave <code> <a|w|m> <stackable> <book> <width> <height>" and "ssave <stat> <bits> <param bits>".
The lines kept are those of global/excel; excel/base has the same item rows and stat sizes (compared 2026-11-24).
"""
LISTS = (("armor", "a"), ("weapons", "w"), ("misc", "m"))


def patch(t):
    if t.excel.name != "excel":
        return
    for table, kind in LISTS:
        for r in t.table(table).rows:
            if r.get("code"):
                t.loot_lines.append(f"isave {r['code']} {kind} {int(r.get('stackable') == '1')} {int(r['code'] in ('tbk', 'ibk'))} "
                                    f"{r.get('invwidth') or 1} {r.get('invheight') or 1}")
    for r in t.table("itemstatcost").rows:
        if r.get("*ID", "").isdigit() and r.get("Save Bits"):
            t.loot_lines.append(f"ssave {r['*ID']} {r['Save Bits']} {r.get('Save Param Bits') or 0}")
