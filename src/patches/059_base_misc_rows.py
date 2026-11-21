"""misc.txt: the base copy of the tables (global/excel/base) gets Blizzard's rows that only
global/excel has (one: the Crafted Sunder Charm, cs2, at the end), as placeholders that never drop.

The game mixes up the two copies' row numbers for items (CLAUDE.md, "Lessons learned"). Without
this, every item the mod adds to misc.txt (the Tower seals, Andariel's two keys) sat one row
earlier in the base copy than in global/excel, so it could show another item's picture or name.
Runs before every patch that adds misc rows.
"""


def patch(t):
    if t.excel.name == "excel":
        return
    main = type(t)()  # global/excel, read fresh
    misc = t.table("misc")
    have = [r.get("code") for r in misc.rows]
    wanted = [r.get("code") for r in main.table("misc").rows]
    if wanted[:len(have)] != have:
        raise RuntimeError("misc: the two copies differ in more than rows at the end")
    for row in main.table("misc").rows[len(have):]:
        placeholder = dict(row)
        placeholder["spawnable"] = "0"
        misc.rows.append(placeholder)
