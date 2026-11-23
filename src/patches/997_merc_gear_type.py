"""The "merc" item type (Mercenary Gear), for mercenary full gear (decided 2026-11-23,
src/plugin/mercgear.cpp, 056_merc_slots.py).

A parent type of body armor, rings, amulets, gloves, boots and belts (their Equiv2). No item has
it as its own type, and nothing else names it, so no other type test changes. The plugin points
the game's "may a mercenary wear this" body armor test at it, by its number, which goes to
loot_data.txt as "merctype <n>". The game numbers item types by row, skipping the "Expansion"
divider; the row goes last in both copies of the table (after every other patch's new rows), so
it has the same number in both.
"""

PARENTS = ("tors", "ring", "amul", "glov", "boot", "belt")


def patch(t):
    types = t.table("itemtypes")
    if any(r.get("Code") == "merc" for r in types.rows):
        return
    template = next(r for r in types.rows if r.get("Code") == "tors")
    row = {k: "" for k in template if k != "__width__"}
    row.update({"ItemType": "Mercenary Gear", "Code": "merc", "Equiv1": "", "Equiv2": "",
                "Repair": "0", "Body": "0", "Throwable": "0", "Reload": "0", "ReEquip": "0", "AutoStack": "0",
                "Magic": "0", "Rare": "0", "Normal": "0", "Beltable": "0", "MaxSockets1": "0", "MaxSockets2": "0",
                "MaxSockets3": "0", "TreasureClass": "0", "Rarity": "0", "StaffMods": "", "Class": "",
                "VarInvGfx": "0", "StorePage": "", "Shoots": "", "Quiver": ""})
    for col in ("Body", "BodyLoc1", "BodyLoc2"):
        if col in row:
            row[col] = "" if col != "Body" else "0"
    types.rows.append(row)
    for r in types.rows:
        if r.get("Code") in PARENTS:
            if r.get("Equiv2", "").strip():
                raise RuntimeError(f"itemtypes {r['Code']} already has Equiv2 {r['Equiv2']}")
            r["Equiv2"] = "merc"
    numbered = [r for r in types.rows if r.get("ItemType") != "Expansion"]
    number = numbered.index(row)
    if number > 255:
        raise RuntimeError(f"the merc item type would be number {number}; the plugin's patch holds one byte")
    if not any(line.startswith("merctype ") for line in t.loot_lines):
        t.loot_lines.append(f"merctype {number}")
