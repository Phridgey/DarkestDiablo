"""Healing potions get slots in the shared stash's Materials tab, like the game's own slots for
Rejuvenation and Full Rejuvenation Potions (decided 2026-11-24).

The same two parts the game uses for those:
- misc.txt: AdvancedStashStackable = 1, so a healing potion stacks in its Materials slot.
  It only changes the stash tab: in the belt and bags they stay single, as Rejuvenations do
  (their "stackable" column stays 0, so they are saved the same way as before).
- The HD stash layout: one AdvancedStashSlotWidget per potion, with its item code, in the free
  space under the two Rejuvenation slots (left of the area the game keeps clear).
Not changed: the old (legacy, "G" key) graphics and the controller layouts.
"""

POTIONS = ("hp1", "hp2", "hp3", "hp4", "hp5")
SIZE = 98
# Under the Rejuvenation slots (x 102 and 206, y 596), on the game's 104-pixel slot spacing.
SPOTS = ((102, 712), (206, 712), (310, 712), (102, 816), (206, 816))


def patch(t):
    misc = t.table("misc")
    for code in POTIONS:
        misc.first("code", code)["AdvancedStashStackable"] = "1"
    if t.excel.name != "excel":  # the base copy only keeps tables; the layout is shipped once
        return
    layout = t.layout("bankexpansionlayouthd")
    materials = t.widget(layout, "advancedstash_materials")
    if materials is None:
        raise RuntimeError("bankexpansionlayouthd has no advancedstash_materials; has the game changed?")
    taken = {(c["fields"]["rect"]["x"], c["fields"]["rect"]["y"]) for c in materials["children"]
             if "rect" in c.get("fields", {})}
    for code, (x, y) in zip(POTIONS, SPOTS):
        if any(c.get("name") == code for c in materials["children"]):
            continue
        if (x, y) in taken:
            raise RuntimeError(f"the Materials tab already has a slot at {x},{y}")
        materials["children"].append({
            "type": "AdvancedStashSlotWidget", "name": code,
            "fields": {"rect": {"width": SIZE, "height": SIZE, "x": x, "y": y}, "itemCode": code},
        })
