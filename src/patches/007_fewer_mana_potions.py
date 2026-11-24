"""Mana potions drop half as often (decided 2026-11-19: with no health potions, mana potions
crowded every drop). Since 2026-11-23 no mana potion drops at all (006_no_health_potions.py runs
first), so this finds nothing to change; kept for the record.

In every loot table that picks by chance and lists mana potions (mp1 .. mp5), the other entries
and "NoDrop" are doubled and the mana potions keep their weight, plus that weight again on
"NoDrop". So mana potions come out half as often, and everything else in the table (gold, items,
rejuvenation potions, nothing) exactly as often as before. Weights stay whole numbers.
"""

MANA = {"mp1", "mp2", "mp3", "mp4", "mp5"}


def patch(t):
    for row in t.table("treasureclassex").rows:
        picks = row.get("Picks", "").strip()
        if not picks.isdigit() or int(picks) <= 0:
            continue
        slots = [k for k in range(1, 11) if row.get(f"Item{k}", "")]
        mana = [k for k in slots if row[f"Item{k}"].split(",")[0] in MANA]
        if not mana:
            continue
        nodrop = int(row["NoDrop"]) if row.get("NoDrop", "").strip().isdigit() else 0
        mana_weight = 0
        for k in slots:
            w = int(row[f"Prob{k}"]) if row.get(f"Prob{k}", "").strip().isdigit() else 0
            if k in mana:
                mana_weight += w
            else:
                row[f"Prob{k}"] = str(w * 2)
        row["NoDrop"] = str(nodrop * 2 + mana_weight)
