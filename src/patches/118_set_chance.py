"""Set items 25% more frequent everywhere (decided 2026-11-22): monster drops, chests, and
gambling.

A dropped item becomes a set when a roll against (Set - (monster level - item level) / SetDivisor)
* 128, made smaller by magic find, with SetMin as the smallest it can get, comes up (itemratio.txt):
the smaller that number, the likelier the set. Set and SetMin are divided by 1.25, so before
rounding every set chance is 1.25 times what it was. SetDivisor stays as it is (it must be a whole
number), so for a monster above the item's level the gain is slightly more than 1.25 times.
Gambling's set share goes from 100 to 125 in 100,000 (difficultylevels.txt).
"""

FACTOR = 1.25


def patch(t):
    for row in t.table("itemratio").rows:
        for col in ("Set", "SetMin"):
            row[col] = str(round(int(row[col]) / FACTOR))
    for row in t.table("difficultylevels").rows:
        row["GambleSet"] = str(round(int(row["GambleSet"]) * FACTOR))
