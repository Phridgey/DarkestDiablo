"""The Campfire outside the plugin: the town bonfire, the camp screen, the stash button.

- Bonfire: the fire's own table row (RogueBonfire) becomes the stash chest in every way the
  game cares about: every value is copied from the chest's row, only its name stays. So it
  should be clickable and open the stash. The plugin can't see a click on a world object,
  only UI messages, so the bonfire borrows the one town object whose window we can put a
  button on. The fire of Harrogath is the same object and changes with it.
  What did not work: setting only the "clickable" values on the fire's row, and a new object
  added as a new last row (the game never showed it; see docs/campfire.md).
- Stash window: a Campfire button that opens the plugin's roster panel.
- Hero screen: new title. It is the Campfire: the game's own figures are moved out of sight and
  the plugin draws the camp's figures (src/plugin/campfire.cpp).
"""
import copy
import struct


FIRE_PRESET = 2     # objpreset.txt, Act 1: RogueBonfire
CHEST_PRESET = 102  # objpreset.txt, Act 1: Bank (the stash chest)


def add_object_near(ds1, near_id, new_id, dx, dy):
    """A town map (.ds1) with one more object: preset `new_id`, placed (dx, dy) sub-tiles from
    the first object with preset `near_id`. Only the object list changes; the rest is kept
    byte for byte."""
    i32 = lambda at: struct.unpack_from("<i", ds1, at)[0]
    version, width, height = i32(0), i32(4) + 1, i32(8) + 1
    if version != 18:
        raise ValueError(f"town map version {version}, expected 18")
    tag_type, at = i32(16), 24
    for _ in range(i32(20)):                 # names of the tile files
        at = ds1.index(b"\0", at) + 1
    walls, floors = i32(at), i32(at + 4)
    layers = walls * 2 + floors + 1 + (1 if tag_type in (1, 2) else 0)   # + shadow, + tag layer
    count_at = at + 8 + layers * width * height * 4
    count = i32(count_at)
    records = [struct.unpack_from("<5i", ds1, count_at + 4 + n * 20) for n in range(count)]
    fire = next((r for r in records if r[0] == 2 and r[1] == near_id), None)
    if fire is None:
        raise ValueError("no bonfire in this town map")
    end = count_at + 4 + count * 20
    new = struct.pack("<5i", 2, new_id, fire[2] + dx, fire[3] + dy, 0)
    return ds1[:count_at] + struct.pack("<i", count + 1) + ds1[count_at + 4:end] + new + ds1[end:]


def strip_physics(node):
    """Removes every collision shape from a scenery file (parsed JSON). Returns how many."""
    removed = 0
    if isinstance(node, dict):
        for key, value in list(node.items()):
            if isinstance(value, list):
                kept = [v for v in value if not (isinstance(v, dict) and "Physics" in str(v.get("type", "")))]
                removed += len(value) - len(kept)
                node[key] = kept
            removed += strip_physics(node[key])
    elif isinstance(node, list):
        for value in node:
            removed += strip_physics(value)
    return removed


def patch(t):
    objects = t.table("objects")
    chest_row = objects.first("Class", "Bank")
    fire_row = objects.first("Class", "RogueBonfire")
    for column in objects.columns:
        if column not in ("Class", "Name", "*Description", "*ID", "AutoMap"):  # no stash mark on the map
            fire_row[column] = chest_row.get(column, "")
    fire_row["__width__"] = max(fire_row["__width__"], chest_row["__width__"])
    # TEST LOOK: the fire's model file is replaced by the stash chest's model standing in the
    # bonfire's flames. The game seems to test the mouse against the model, and the bonfire's
    # own model could not be pointed at. If the chest can be clicked here, the next step is a
    # model that suits the fire. The file keeps its name and place, so the game finds it the
    # way it always did.
    chest = t.data("hd/objects/chests/bank.json")
    fire = t.data("hd/objects/env_wood/rogue_bonfire.json")
    look = copy.deepcopy(chest)
    look["name"] = fire["name"]
    look["entities"] += [copy.deepcopy(e) for e in fire["entities"] if e["name"] == "entity_vfx"]
    for kind, paths in fire["dependencies"].items():
        if kind not in ("skeletons", "json"):
            have = look["dependencies"].setdefault(kind, [])
            have += [p for p in paths if p not in have]
    t.add_data("hd/objects/env_wood/rogue_bonfire.json", look)
    del t.files["hd/objects/chests/bank.json"]  # read only, not shipped

    # TEST: a second, ordinary stash chest a few steps from the fire, added to the four town
    # maps. If this one can be clicked and the chest in the fire can't, the spot is the problem.
    for town in ("townn1", "towne1", "towns1", "townw1"):
        path = f"global/tiles/act1/town/{town}.ds1"
        t.add_raw(path, add_object_near(t.raw(path), near_id=FIRE_PRESET, new_id=CHEST_PRESET, dx=9, dy=4))
    # TEST: the fire ring's scenery carries a collision shape. If that shape is what swallows the
    # mouse, the chest in the fire becomes clickable once the shape is gone.
    prefab_path = "hd/env/model/global/prop/act1/town/prefab/pf_town_campfire01.json"
    prefab = t.data(prefab_path)
    removed = strip_physics(prefab)
    print(f"campfire scenery: {removed} collision shape(s) removed")
    if not removed:
        del t.files[prefab_path]  # nothing to change: not shipped

    bank = t.layout("bankexpansionlayouthd")
    bank["children"].append({
        "type": "ButtonWidget", "name": "Campfire",
        "fields": {
            "rect": {"x": 431, "y": 1420},
            "filename": "Panel\\Modals\\ModalButton",
            "focusIndicatorFilename": "Controller/HoverImages/ModalButton_Hover",
            "pressedFrame": 1, "disabledFrame": 2, "hoveredFrame": 3,
            "textString": "Campfire",
            "pointSize": "$MediumFontSize",
            "textColor": "$FontColorWhite",
            "onClickMessage": "PanelManager:OpenPanel:darkestdiablo/open-campfire",
        },
    })

    # The hero screen is the Campfire: the plugin draws the camp's heroes on it.
    create = t.layout("charactercreatepanelhd")
    t.widget(create, "Title")["fields"]["text"] = "The Campfire"
    t.widget(create, "ToMainMenu")["fields"]["textString"] = "Change save"
    # The game's own eight figures can't be hidden or changed while the game runs, so they are
    # moved out of sight (under the ground, off the screen). The plugin's Fire panel draws the
    # figures that belong at this camp's fire. tools/build.py notes their spots and looks first.
    for child in create["children"]:
        if child.get("type") == "HDCharacterSelectorWidget":
            fields = child["fields"]
            fields["rect"] = dict(fields["rect"], x=fields["rect"]["x"] + 40000)
            x, _, z = fields["player/position"]
            fields["player/position"] = [x, -300.0, z]
