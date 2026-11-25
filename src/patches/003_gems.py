"""Gems (decided 2026-11-22): gems drop far more often, and the better gems come in early.

Target: Flawless gems show up by hero level 24 (Act II's Maggot Lair and Claw Viper Temple, area
level 27), about one per two runs that aren't crafting hunts; Chipped gems always keep a small
lead over each other grade.

How:
- Every gem drop goes through one table per monster level, "DD Gems L<level>", whose grades are
  weighted by GRADES: Chipped 12; each better grade grows from 0 to 10 between the two levels
  given (so Flawless reaches its full share at level 25). Perfect gems start at 42.
- The loot tables that monsters and chests drop from (one per level; "H2H", "Cast", "Miss",
  "Wraith" for ordinary monsters, "Citem" for champions, "Uitem" for uniques, "Chest") each get one
  gem entry for their level, weighted so a pick drops a gem with the chance in RATE.
- The old gem entries in the "Good" tables (a few per cent of all drops) are folded into one
  entry of that table's lowest level, so they follow the same grades.
Runs before the other patches, so the copies they make (magic find, experience, crafting) carry it.
"""
import re

# grade, item table, level it starts, level it reaches its full weight (Chipped: always full)
GRADES = [("Chipped Gem", None, None), ("Flawed Gem", 4, 8), ("Normal Gem", 10, 16),
          ("Flawless Gem", 19, 25), ("Perfect Gem", 42, 50)]
FULL, CHIPPED = 10, 12

# Chance that one pick of the table drops a gem, by the table's kind. Halved on 2026-11-25 (too many
# gems everywhere), from 0.06 / 0.2 / 0.35 / 0.10; GOOD_SHARE halves the "Good" tables' gems too.
RATE = {"H2H": 0.03, "Cast": 0.03, "Miss": 0.03, "Wraith": 0.03, "Citem": 0.1, "Uitem": 0.175, "Chest": 0.05}
GOOD_SHARE = 0.5
GOOD = re.compile(r"Act \d( \([NH]\))? Good")


def weights(level):
    out = []
    for name, start, full in GRADES:
        if start is None:
            out.append((name, CHIPPED))
            continue
        share = max(0.0, min(1.0, (level - start) / (full - start)))
        weight = round(FULL * share)
        if weight > 0:
            out.append((name, weight))
    return out


def entries(row):
    return [(row[f"Item{k}"], row[f"Prob{k}"]) for k in range(1, 11) if row.get(f"Item{k}")]


def set_entries(row, items):
    if len(items) > 10:
        raise RuntimeError(f"{row['Treasure Class']}: more than 10 entries")
    for k in range(1, 11):
        row[f"Item{k}"], row[f"Prob{k}"] = items[k - 1] if k <= len(items) else ("", "")


def patch(t):
    tcs = t.table("treasureclassex")
    template = tcs.rows[0]
    made = {}

    def gem_table(level):
        name = f"DD Gems L{level}"
        if name not in made:
            row = {k: "" for k in template if k != "__width__"}
            row.update({"Treasure Class": name, "Picks": "1", "NoDrop": "0"})
            set_entries(row, [(g, str(w)) for g, w in weights(level)])
            made[name] = row
        return name

    good_level = {}
    for row in list(tcs.rows):
        level = row.get("level", "").strip()
        if not row.get("group", "").strip() or not level.isdigit():
            continue
        items = entries(row)
        for item, _ in items:
            if GOOD.fullmatch(item):
                good_level[item] = min(good_level.get(item, 999), int(level))
        kind = re.sub(r"^Act \d( \([NH]\))? ", "", row["Treasure Class"])
        kind = re.sub(r" [A-C]$", "", kind)
        rate = RATE.get(kind)
        if rate is None or not any(GOOD.fullmatch(i) for i, _ in items) or int(row["Picks"] or 1) < 1:
            continue
        total = sum(int(p or 0) for _, p in items) + int(row.get("NoDrop") or 0)
        items.append((gem_table(int(level)), str(max(1, round(rate * total / (1 - rate))))))
        set_entries(row, items)

    grades = {g for g, _, _ in GRADES}
    for row in tcs.rows:
        if row["Treasure Class"] in good_level:
            items = entries(row)
            gems = sum(int(p or 0) for i, p in items if i in grades)
            kept = [(i, p) for i, p in items if i not in grades]
            if gems:
                kept.insert(1, (gem_table(good_level[row["Treasure Class"]]), str(max(1, round(gems * GOOD_SHARE)))))
                if int(row.get("Picks") or 1) > 0:  # the rest of their weight drops nothing
                    row["NoDrop"] = str(int(row.get("NoDrop") or 0) + gems - max(1, round(gems * GOOD_SHARE)))
            set_entries(row, kept)
    # The game reads the tables top to bottom and can't find one that comes later ("Couldn't parse
    # treasure class item", 2026-11-25), so these go right after the gem grades they are made of.
    at = 1 + max(i for i, row in enumerate(tcs.rows) if row["Treasure Class"] in grades)
    tcs.rows[at:at] = made.values()
