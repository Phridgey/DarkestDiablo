"""The Campfire outside the plugin: the town bonfire, the camp screen, the stash button.

- Bonfire: the fire's own table row (RogueBonfire) takes the stash chest's "use me" values, so
  clicking the fire opens the stash. The plugin can't see a click on a world object, only UI
  messages, so the fire borrows the one town object whose window we can put a button on. The
  fire of Harrogath is the same object and changes with it. The game reads the second copy of
  the tables (global/excel/base) for our heroes; until tools/build.py patched that copy too,
  none of the tries showed in game (see docs/campfire.md).
- Stash window: a Campfire button that opens the plugin's roster panel.
- Hero screen: new title. It is the Campfire: the game's own figures are moved out of sight and
  the plugin draws the camp's figures (src/plugin/campfire.cpp).
"""
import copy
import struct

CHEST_PRESET = 102  # objpreset.txt, Act 1: Bank (the stash chest)


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


def without_object(ds1, preset_id):
    """A town map (.ds1, version 18) without its objects of preset `preset_id`. Only the object
    list changes; the rest is kept byte for byte. Returns (new map, how many were removed)."""
    i32 = lambda at: struct.unpack_from("<i", ds1, at)[0]
    if i32(0) != 18:
        raise ValueError(f"town map version {i32(0)}, expected 18")
    width, height, tag_type, at = i32(4) + 1, i32(8) + 1, i32(16), 24
    for _ in range(i32(20)):                 # names of the tile files
        at = ds1.index(b"\0", at) + 1
    layers = i32(at) * 2 + i32(at + 4) + 1 + (1 if tag_type in (1, 2) else 0)   # walls+orients, floors, shadow, tag
    count_at = at + 8 + layers * width * height * 4
    count = i32(count_at)
    records = [ds1[count_at + 4 + n * 20:count_at + 24 + n * 20] for n in range(count)]
    kept = [r for r in records if struct.unpack_from("<2i", r) != (2, preset_id)]
    end = count_at + 4 + count * 20
    return ds1[:count_at] + struct.pack("<i", len(kept)) + b"".join(kept) + ds1[end:], count - len(kept)


def patch(t):
    # The fire works like the stash chest: clicking it opens the stash, whose Campfire button
    # opens the roster. Only the columns that make the chest work are copied; the rest of the
    # fire's row (flames, light, size, no map mark) stays, so it still looks and lights like the
    # fire. It keeps its size (3 by 3), so it can be used from the edge of the fire pit.
    # This runs on both copies of the tables (global/excel and global/excel/base): the game read
    # only the base copy for our heroes, which is why every earlier try seemed to do nothing.
    objects = t.table("objects")
    chest_row = objects.first("Class", "Bank")
    fire_row = objects.first("Class", "RogueBonfire")
    for column in ("Selectable0", "Parm0", "Sync", "OperateFn", "PopulateFn"):
        fire_row[column] = chest_row.get(column, "")
    # A bigger click box than the fire's own picture, so it can be used from any side of the
    # pit. With CollisionSubst 1 the game uses Left/Top/Width/Height (screen pixels around the
    # fire's foot) as the click box, like the town portal and the Inifuss tree.
    fire_row.update({"CollisionSubst": "1", "Left": "-90", "Top": "-100", "Width": "180", "Height": "140"})

    # The fire is the camp's stash, so the Rogue Encampment's stash chest is taken out of the
    # four town maps. The game ignores changed maps shipped under the game's own file names, so
    # they get new names (darkest_town*.ds1, with a copy of the town's HD look under the same
    # name) and the town's row in lvlprest.txt points at them. When the stash opens in this town,
    # the plugin opens the Campfire roster with it (campfire.cpp).
    town_row = t.table("lvlprest").first("Name", "Act 1 - Town 1")
    for n, town in enumerate(("townn1", "towne1", "towns1", "townw1"), start=1):
        new = f"darkest_{town}"
        ds1, removed = without_object(t.raw(f"global/tiles/act1/town/{town}.ds1"), CHEST_PRESET)
        if removed != 1:
            raise ValueError(f"{town}.ds1 has {removed} stash chests, expected 1")
        t.add_raw(f"global/tiles/act1/town/{new}.ds1", ds1)
        look_path = f"hd/env/preset/act1/town/{town}.json"
        t.add_data(f"hd/env/preset/act1/town/{new}.json", copy.deepcopy(t.data(look_path)))
        del t.files[look_path]  # read only, not shipped
        town_row[f"File{n}"] = f"Act1/Town/{new}.ds1"

    # The fire ring's 3D scenery carries collision shapes that could keep the mouse off the fire.
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
