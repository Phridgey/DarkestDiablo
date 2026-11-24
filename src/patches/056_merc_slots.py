"""Mercenaries wear everything a hero does (decided 2026-11-23): amulet, two rings, belt, boots and
gloves, on top of helm, armor and weapons.

Done the way full-gear mercenary mods do it:
- inventory.txt: the Hireling and Hireling2 rows get the slots the heroes' rows have (their
  places matter only to the old graphics).
- The HD mercenary panel (hirelinginventorypanelhd): six more equipment slots, in the free space
  around the paperdoll: gloves and boots above the weapons, the amulet beside the helm, the rings
  beside the armor, the belt below the left-hand weapon.

Equipped items stay with the mercenary in the save, so never remove these slots from a released
game: a mercenary wearing a ring would not load.
Not changed: the controller layout.
"""

SLOTS = ("neck", "rHand", "lHand", "belt", "feet", "gloves")
PARTS = ("Left", "Right", "Top", "Bottom", "Width", "Height")

# location, background picture, x, y, width, height (HD panel pixels)
WIDGETS = [
    ("gloves", "Inventory_Paperdoll_Glove", 108, 130, 196, 196),
    ("feet", "Inventory_Paperdoll_Boots", 861, 130, 196, 196),
    ("neck", "Inventory_Paperdoll_Amulet", 718, 193, 98, 98),
    ("right_hand", "Inventory_Paperdoll_Ring", 344, 630, 98, 98),
    ("left_hand", "Inventory_Paperdoll_Ring", 718, 630, 98, 98),
    ("belt", "Inventory_Paperdoll_Belt", 861, 733, 196, 98),
]


def patch(t):
    inv = t.table("inventory")
    for merc, hero in (("Hireling", "Amazon"), ("Hireling2", "Amazon2")):
        row, source = inv.first("class", merc), inv.first("class", hero)
        for slot in SLOTS:
            for part in PARTS:
                col = slot + part
                if col in row and col in source:
                    row[col] = source[col]
    panel = t.layout("hirelinginventorypanelhd")
    children = panel["children"]
    names = {c.get("name") for c in children}
    at = next(i for i, c in enumerate(children) if c.get("name") == "slot_left_arm") + 1
    for location, picture, x, y, w, h in WIDGETS:
        name = f"slot_{location}"
        if name in names:
            continue
        children.insert(at, {
            "type": "InventorySlotWidget", "name": name,
            "fields": {
                "rect": {"x": x, "y": y, "width": w, "height": h},
                "cellSize": "$ItemCellSize",
                "location": location,
                "gemSocketFilename": "PANEL\\gemsocket",
                "backgroundFilename": f"PANEL\\Inventory\\{picture}",
                "isHireable": True,
            },
        })
        at += 1
