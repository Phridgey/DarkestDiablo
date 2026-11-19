"""Runs last: tells the plugin the class number of every monster type the mod adds ("dd_..."),
as "monclass <Id> <class>" lines in d2rloader/loot_data.txt. The console spawns a unique
monster by class number ("cheat monster unique:<class>"); a type's Id there does nothing.
A type's class is its place among monstats.txt's numbered rows ("*hcIdx").
"""


def patch(t):
    stats = t.table("monstats")
    numbered = 0
    for row in stats.rows:
        if not row.get("*hcIdx", "").strip().isdigit():
            continue
        if row.get("Id", "").startswith("dd_"):
            t.loot_lines.append(f"monclass {row['Id']} {numbered}")
        numbered += 1
