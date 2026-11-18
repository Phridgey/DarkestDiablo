"""Camp heroes: figures of the save's other heroes resting by the town fire (Sam, 2026-11-18).

Each is a new monster row that wears a playable class's own 3D body, in light, medium or heavy
armor (the same pieces the game puts on a hero for its armor weight). They only stand there:
no talking, no fighting, can't be clicked. Exact item looks and weapons can't be shown this way.

For each class and armor weight there is a monster row ddhero_<class>_<lit|med|hvy>, with its
own body row in monstats2 (which pieces it wears), and the HD look
data/hd/character/enemy/ddhero_<class>.json, a copy of the class's own look file. The plugin
spawns them (campfire.cpp); to test by hand: cheat mspawn ddhero_necromancer_med
"""
import copy

CLASSES = ("amazon", "assassin", "barbarian", "druid", "necromancer", "paladin", "sorceress", "warlock")
WEIGHTS = ("lit", "med", "hvy")
LOOK = "nihlathak"        # monstats row to copy: a town figure that stands still
BODY = "act2guard1"       # monstats2 row to copy: a town guard that can't be clicked or attacked
PIECES = ("TR", "LG", "RA", "LA", "S1", "S2")  # torso, legs, arms, shoulders: what armor weight changes
ALL_PIECES = ("HD", "TR", "LG", "RA", "LA", "RH", "LH", "SH", "S1", "S2", "S3", "S4", "S5", "S6", "S7", "S8")
STRING_KEY = "ddCampHero"
STRING_ID = 91001


def patch(t):
    monstats = t.table("monstats")
    monstats2 = t.table("monstats2")
    models = t.data("hd/character/monsters.json")
    for cls in CLASSES:
        look_path = f"hd/character/player/{cls}.json"
        look = copy.deepcopy(t.data(look_path))
        del t.files[look_path]  # read only, not shipped as itself
        look["name"] = f"ddhero_{cls}"
        t.add_data(f"hd/character/enemy/ddhero_{cls}.json", look)
        for weight in WEIGHTS:
            name = f"ddhero_{cls}_{weight}"
            if monstats.find("Id", name):
                raise RuntimeError(f"monstats: {name} exists already")
            body = monstats2.add_copy("Id", BODY)
            body.update({"Id": name, "*hcIdx": str(len(monstats2.rows) - 1), "TotalPieces": str(len(PIECES))})
            for piece in ALL_PIECES:
                body[piece] = "1" if piece in PIECES else ""
                body[piece.capitalize() + "v" if piece in ("RA", "LA") else piece + "v"] = weight if piece in PIECES else ""
            row = monstats.add_copy("Id", LOOK)
            row.update({
                "Id": name,
                "*hcIdx": str(len(monstats.rows) - 1),
                "BaseId": name,
                "NextInClass": "",
                "MonStatsEx": name,
                "NameStr": STRING_KEY,
                "AI": "Idle",
                "npc": "",
                "interact": "",
                "inventory": "",
                "isSpawn": "",
                "killable": "",
                "Velocity": "0",
                "Run": "0",
            })
            models[name] = f"ddhero_{cls}"

    if t.excel.name != "excel":  # the base copy only keeps tables; the string is shipped once
        return
    strings = t.data("local/lng/strings/monsters.json")
    if not any(e.get("Key") == STRING_KEY for e in strings):
        langs = [k for k in strings[0] if k not in ("id", "Key")]
        strings.append({"id": STRING_ID, "Key": STRING_KEY, **{lang: "Resting hero" for lang in langs}})
