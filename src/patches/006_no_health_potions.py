"""No health potions drop anywhere (decided 2026-11-19). Healing comes from elsewhere; this also
shows what drop rates are really like without the potion filler.

Every loot table entry for a healing potion (hp1 .. hp5) is taken out, and in tables that pick
by chance its weight moves to "NoDrop", so everything else in that table (mana and rejuvenation
potions, gold, items) drops exactly as often as before. Tables that drop every entry (negative
Picks) just lose it.

Shops don't sell them either: every vendor's stock columns for them in misc.txt are emptied. The
plugin hands out a few per cleared stop instead (the camp's potion supply).
"""

HEALTH = {"hp1", "hp2", "hp3", "hp4", "hp5"}


def patch(t):
    for row in t.table("treasureclassex").rows:
        entries = [(row.get(f"Item{k}", ""), row.get(f"Prob{k}", "")) for k in range(1, 11)]
        if not any(item.split(",")[0] in HEALTH for item, _ in entries):
            continue
        kept, dropped = [], 0
        for item, prob in entries:
            if not item:
                continue
            if item.split(",")[0] in HEALTH:
                dropped += int(prob) if prob.strip().isdigit() else 0
            else:
                kept.append((item, prob))
        picks = int(row["Picks"]) if row.get("Picks", "").strip().lstrip("-").isdigit() else 1
        if picks > 0:
            nodrop = int(row["NoDrop"]) if row.get("NoDrop", "").strip().isdigit() else 0
            row["NoDrop"] = str(nodrop + dropped)
        for k in range(1, 11):
            row[f"Item{k}"], row[f"Prob{k}"] = kept[k - 1] if k <= len(kept) else ("", "")

    misc = t.table("misc")
    vendor_columns = [c for c in misc.columns if (c.endswith("Min") or c.endswith("Max")) and not c.startswith("TMog")]
    for row in misc.rows:
        if row.get("code") in HEALTH:
            for c in vendor_columns:
                row[c] = ""
