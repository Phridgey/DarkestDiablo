"""No keys from any loot table: monsters, chests, everything. Runs first, so every table the
later patches copy is already key-free. (Vendors' stock doesn't come from loot tables.)

A key's entry is taken out of its table; the other entries keep their chances relative to each
other, so the table's other items just drop a little more often.
"""


def patch(t):
    removed = 0
    for row in t.table("treasureclassex").rows:
        entries = [(row.get(f"Item{k}", ""), row.get(f"Prob{k}", "")) for k in range(1, 11)]
        kept = [(item, prob) for item, prob in entries if item and item.split(",")[0] != "key"]
        if len(kept) == len([e for e in entries if e[0]]):
            continue
        if not kept:
            raise RuntimeError(f"{row['Treasure Class']} drops only keys; give it something else to drop.")
        removed += len([e for e in entries if e[0]]) - len(kept)
        for k in range(1, 11):
            row[f"Item{k}"], row[f"Prob{k}"] = kept[k - 1] if k <= len(kept) else ("", "")
    if removed == 0:
        raise RuntimeError("No key entries found in treasureclassex.txt; has the key's code changed?")
    print(f"  no keys: {removed} key entries taken out of the loot tables")
