"""The traveler: a hooded figure that steps out of Cain's portal by the town fire when a traveler
waits (the plugin spawns it with `cheat mspawn ddtraveler`, see campfire.cpp).

It is a new monster row with Nihlathak's hooded look, but it only stands there: no talking, no
trading, no walking off, and it can't be attacked. It is gone once the hero leaves the game for
the Campfire, like anything spawned with the console.
"""
NAME = "ddtraveler"
LOOK = "nihlathak"        # monstats row whose figure and animations it borrows
STRING_KEY = "ddTraveler"
STRING_ID = 91000         # above the game's string ids and clear of 050_dodge_roll.py (90000+)


def patch(t):
    monstats = t.table("monstats")
    if monstats.find("Id", NAME):
        raise RuntimeError(f"monstats: {NAME} exists already")
    row = monstats.add_copy("Id", LOOK)
    row.update({
        "Id": NAME,
        "*hcIdx": str(len(monstats.rows) - 1),
        "BaseId": NAME,
        "NextInClass": "",
        "NameStr": STRING_KEY,
        "AI": "Idle",       # stands still
        "interact": "",     # no dialogue, no trade
        "inventory": "",
        "isSpawn": "",
        "killable": "",
        "Velocity": "0",
        "Run": "0",
    })

    if t.excel.name != "excel":  # the base copy only keeps tables; the string is shipped once
        return
    strings = t.data("local/lng/strings/monsters.json")
    if not any(e.get("Key") == STRING_KEY for e in strings):
        langs = [k for k in strings[0] if k not in ("id", "Key")]
        strings.append({"id": STRING_ID, "Key": STRING_KEY, **{lang: "Traveler" for lang in langs}})
