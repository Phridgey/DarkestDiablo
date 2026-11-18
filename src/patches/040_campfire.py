"""The Campfire outside the plugin: the town bonfire and the camp screen.

- Bonfire: the fire's own table row (RogueBonfire) takes the stash chest's "use me" values, so
  clicking the fire opens the stash window. The plugin can't see a click on a world object, only
  UI messages, so the fire borrows the stash's message and the plugin checks that the hero
  stands at the fire. The fire of Harrogath is the same object and changes with it. The game reads the second copy of
  the tables (global/excel/base) for our heroes; until tools/build.py patched that copy too,
  none of the tries showed in game (see docs/campfire.md).
- Hero screen: new title. It is the Campfire: the game's own figures are moved out of sight and
  the plugin draws the camp's figures (src/plugin/campfire.cpp).
"""


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
    # fire's foot) as the click box, like the town portal and the Inifuss tree. That box only
    # counts in the old graphics: in the new ones the game tests the mouse against the fire's 3D
    # model (0.11.52: a 40% bigger box changed nothing in game), see the HD part below.
    # 0.11.56: the reach Sam saw with 0.11.53/0.11.55 (about 300 screen pixels at his resolution
    # from the fire's middle) matches this box, not the hidden model's size (changing the model
    # from 1.4 to 1.15 changed nothing), so the box is cut to end at the stones' outer edge
    # (about 0.6 of 252 x 196), around the same middle.
    fire_row.update({"CollisionSubst": "1", "Left": "-75", "Top": "-88", "Width": "150", "Height": "116"})

    # New graphics: a hidden copy of the fire's own model, bigger, under the real one, so the
    # mouse finds the fire over a wider area while it looks the same. Hidden models do take clicks
    # (0.11.53). At 1.4 times the size the click area reached about a hero's length past the
    # stones; Sam wants it to end at the outer edge of the stones (0.11.55: 1.15).
    click_scale = 1.15
    bonfire_path = "hd/objects/env_wood/rogue_bonfire.json"
    bonfire = t.data(bonfire_path)
    torso = next(e for e in bonfire["entities"] if e["name"] == "entity_torso")
    model = next(c for c in torso["components"] if c["type"] == "ModelDefinitionComponent")
    bonfire["entities"].append({
        "type": "Entity", "name": "entity_clickarea", "id": 1585045271,
        "components": [
            {"type": "UnitPartComponent", "name": "component_part_clickarea", "part": "torso", "variant": "lit"},
            dict(model, name="component_model_clickarea", visibleLayers=0, lightMask=0, shadowMask=0),
            {"type": "TransformDefinitionComponent", "name": "entity_clickarea_TransformDefinition",
             "position": {"x": 0.0, "y": 0.0, "z": 0.0}, "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
             "scale": {"x": click_scale, "y": click_scale, "z": click_scale}, "inheritOnlyPosition": False},
        ],
    })

    # The town's own stash chest stays exactly as it is (Sam: "don't touch the original stash").
    # Fire and chest both open the stash window, so the plugin tells them apart by where the
    # hero stands: next to the fire it goes to the Campfire, anywhere else the stash opens as
    # usual (campfire.cpp, positions read as in docs/game-memory.md).

    # The fire ring's 3D scenery carries collision shapes that could keep the mouse off the fire.
    prefab_path = "hd/env/model/global/prop/act1/town/prefab/pf_town_campfire01.json"
    prefab = t.data(prefab_path)
    removed = strip_physics(prefab)
    print(f"campfire scenery: {removed} collision shape(s) removed")
    if not removed:
        del t.files[prefab_path]  # nothing to change: not shipped

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
