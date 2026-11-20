"""A second copy of every superunique the plugin spawns (decided 2026-11-19).

The game lets each superunique appear only once per game. A fork stop can be picked twice in a
row, and a hero who stays in the same game found the second Rakanishu & Treehead run without
either of them: their spawn did nothing, so the run could never be cleared.

Each copy is the same monster (name, type, pack, loot) under its own row. The plugin spawns the
copy when the original already appeared in this game, from "superalt <row> <copy row>" lines in
d2rloader/loot_data.txt. Runs after every patch that changes these superuniques, so the copies
get their changes too.
"""

SPAWNED = ["Corpsefire", "Rakanishu", "Treehead WoodFist", "Griswold", "The Countess"]


def patch(t):
    supers = t.table("superuniques")
    for name in SPAWNED:
        original = supers.first("Superunique", name)
        row_number = supers.rows.index(original)
        copy_number = len(supers.rows)
        supers.add_copy("Superunique", name, Superunique=f"{name} 2", hcIdx=str(copy_number))
        t.loot_lines.append(f"superalt {row_number} {copy_number}")
