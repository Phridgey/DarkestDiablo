"""One more row in every hero's own inventory: 10x5 instead of 10x4.

Done the way expanded-inventory mods do it, in three parts:
- inventory.txt: each class's bag is 5 rows high (the game's own rule for what fits).
- The HD inventory panel (layouts): the grid is 5 cells high, the panel is one cell (98) taller,
  and the gold line below the grid moves down with it.
- The panel's background picture: the game's own picture with one grid row added, made here at
  build time from the player's game files, so no Blizzard art is in the repo.

Heroes keep their items when this is added. Taking the row away again later would lose (or
break) items lying in it, so never remove it from a released game.
Not changed: the old (legacy, "G" key) graphics and the controller layouts.
"""
import struct

ROWS = 5
CELL = 98            # HD cell size in the layouts and the picture
GRID_Y = 897         # top of the vanilla grid in the panel
CUT = 1240           # a line inside the grid's 4th row; one row's worth above it is repeated there
CLASSES = ("Amazon", "Sorceress", "Necromancer", "Paladin", "Barbarian", "Druid", "Assassin", "Warlock")
BACKGROUND = "hd/global/ui/panel/inventory/background{}.sprite"


def taller_sprite(data, cut, add):
    """A one-frame RGBA .sprite with the `add` pixel rows above `cut` repeated at `cut`."""
    if data[:4] != b"SpA1":
        raise RuntimeError("The inventory background is not a sprite file the build knows.")
    width, height, frames = struct.unpack_from("<I", data, 8)[0], struct.unpack_from("<I", data, 12)[0], struct.unpack_from("<I", data, 20)[0]
    row = width * 4
    if frames != 1 or len(data) != 40 + row * height:
        raise RuntimeError(f"Unexpected inventory background ({width}x{height}, {frames} frames); has the game changed?")
    pixels = data[40:]
    new = pixels[:cut * row] + pixels[(cut - add) * row:cut * row] + pixels[cut * row:]
    header = bytearray(data[:40])
    struct.pack_into("<I", header, 12, height + add)
    struct.pack_into("<I", header, 32, len(new))
    return bytes(header) + new


def patch(t):
    inv = t.table("inventory")
    changed = 0
    for row in inv.rows:
        if row["class"] in CLASSES or row["class"][:-1] in CLASSES:
            if row["gridY"] != "4":
                raise RuntimeError(f"inventory.txt: {row['class']} has {row['gridY']} rows, expected 4.")
            row["gridY"] = str(ROWS)
            row["gridBottom"] = str(int(row["gridBottom"]) + int(row["gridBoxHeight"]))
            changed += 1
    if changed != 2 * len(CLASSES):
        raise RuntimeError(f"inventory.txt: found {changed} hero rows, expected {2 * len(CLASSES)}.")

    # The picture, in both graphics qualities (the low one is half size).
    for suffix, scale in (("", 1), (".lowend", 2)):
        path = BACKGROUND.format(suffix)
        t.add_file(path, taller_sprite(t.game_file(path), CUT // scale, CELL // scale))

    profile = t.layout("_profilehd", change=False)
    panel = dict(profile["RightPanelRect"])
    catcher = dict(profile["PanelClickCatcherRect"])
    panel["height"] += CELL
    catcher["height"] += CELL
    for name in ("playerinventoryoriginallayouthd", "playerinventoryexpansionlayouthd"):
        layout = t.layout(name)
        layout.setdefault("fields", {})["rect"] = panel
        for widget, fields in (
            ("click_catcher", {"rect": catcher}),
            ("background", {"filename": "PANEL\\Inventory\\Background"}),  # classic heroes use it too
            ("grid", {"rect": {"x": 93, "y": GRID_Y}, "cellCount": {"x": 10, "y": ROWS}}),
            ("gold_amount", {"rect": {"x": 487, "y": 1346 + CELL, "width": 249, "height": 48}}),
            ("gold_button", {"rect": {"x": 429, "y": 1347 + CELL}}),
        ):
            w = t.widget(layout, widget)
            if w is None:
                raise RuntimeError(f"{name}: no widget {widget!r}; has the game changed?")
            w.setdefault("fields", {}).update(fields)
    print(f"  inventory row: {ROWS} rows for {len(CLASSES)} classes, panel {panel['height']} high")
