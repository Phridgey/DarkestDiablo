"""Den of Evil: its monsters have 55% less defense (2026-11-23).

The plugin spawns the Den's waves as copies of their own ("dd_den_<type>"), so the same types
elsewhere (Tristram, the Tree of Inifuss) keep theirs. Corpsefire (his own type, dd_corpsefire,
010_den_of_evil.py) gets the same cut, and his minions are the Den's zombies.

Runs late: new monster rows go after every row whose number the plugin knows (Corpsefire 751,
Treehead 752, ...), so those numbers don't move.
"""

DEN_TYPES = ("fallen1", "zombie1", "fallenshaman1")  # the wave monsters (DenWaves in the plugin)
DEN_PREFIX = "dd_den_"
DEN_DEFENSE = 0.45                                    # 45% of the game's defense: 55% less


def patch(t):
    stats = t.table("monstats")
    models = t.data("hd/character/monsters.json")
    for source in DEN_TYPES:
        stats.add_copy("Id", source, Id=DEN_PREFIX + source, NextInClass="")
        models[DEN_PREFIX + source] = models[source]  # without this the monster is invisible
    for row in [stats.first("Id", DEN_PREFIX + s) for s in DEN_TYPES] + [stats.first("Id", "dd_corpsefire")]:
        for col in ("minion1", "minion2"):
            if row.get(col) in DEN_TYPES:
                row[col] = DEN_PREFIX + row[col]
        for col in ("AC", "AC(N)", "AC(H)"):
            if row.get(col, "").strip().isdigit():
                row[col] = str(round(int(row[col]) * DEN_DEFENSE))
