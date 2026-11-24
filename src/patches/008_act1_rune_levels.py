"""Act I's runes (El to Ral, Act I's highest) can be used in Act I (decided 2026-11-22).

Act I spans hero levels 1-18 (docs/flattening.md), but the game's requirements for these runes
are 11 to 19, so runes from the act's early stops waited in the stash and Ral could never be used
in Act I. Their level requirement is spread over the act instead. A runeword needs the highest
requirement of its base and its runes, so Act I runewords (Stealth, Steel, ...) come down with
them; the base item still sets its own floor. Later runes keep the game's requirements.
"""

LEVEL_REQ = {"r01": 3, "r02": 4, "r03": 6, "r04": 7, "r05": 9, "r06": 11, "r07": 13, "r08": 16}


def patch(t):
    misc = t.table("misc")
    for code, req in LEVEL_REQ.items():
        misc.first("code", code)["levelreq"] = str(req)
