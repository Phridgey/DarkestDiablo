"""No potions and no scrolls drop anywhere (decided 2026-11-23; health potions since 2026-11-19;
throwing potions and elixirs since 2026-11-24).

Every loot table entry for a potion (healing, mana, rejuvenation, antidote, stamina, thawing) or a
scroll (Town Portal, Identify) is taken out, and in tables that pick by chance its weight moves to
"NoDrop", so everything else in that table (gold, items, gems, runes) drops exactly as often as
before. Tables that drop every entry (negative Picks) just lose it.

Shops don't sell health potions: every vendor's stock columns for them in misc.txt are emptied.
The plugin hands out a few per cleared stop instead (the camp's potion supply), and the Town
Portal scroll that ends each stop still comes from the plugin.
"""

HEALTH = {"hp1", "hp2", "hp3", "hp4", "hp5"}
NO_DROP = HEALTH | {"mp1", "mp2", "mp3", "mp4", "mp5", "rvs", "rvl", "yps", "vps", "wms", "tsc", "isc",
                   # throwing potions (oil, exploding, gas) and elixirs too (2026-11-24)
                   "ops", "opm", "opl", "gps", "gpm", "gpl", "elx"}


def patch(t):
    for row in t.table("treasureclassex").rows:
        entries = [(row.get(f"Item{k}", ""), row.get(f"Prob{k}", "")) for k in range(1, 11)]
        if not any(item.split(",")[0] in NO_DROP for item, _ in entries):
            continue
        kept, dropped = [], 0
        for item, prob in entries:
            if not item:
                continue
            if item.split(",")[0] in NO_DROP:
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
