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
