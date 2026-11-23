"""Gambling (decided 2026-11-22): a gambled item is 3 times as likely to be rare and 10 times as
likely to be unique, in every difficulty.

difficultylevels.txt gives each quality's share out of 100,000: rare 10,000 (10%) -> 30,000
(30%), unique 50 (0.05%) -> 500 (0.5%). Sets (100, 0.1%) and the chances of exceptional and
elite bases stay as they are; magic items take what is left (about 69.4%).
"""

RARE_FACTOR = 3
UNIQUE_FACTOR = 10


def patch(t):
    for row in t.table("difficultylevels").rows:
        row["GambleRare"] = str(int(row["GambleRare"]) * RARE_FACTOR)
        row["GambleUnique"] = str(int(row["GambleUnique"]) * UNIQUE_FACTOR)
