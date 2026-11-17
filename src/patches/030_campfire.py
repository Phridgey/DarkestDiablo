"""The Campfire outside the plugin: the town bonfire, the camp screen, the stash button.

- Bonfire: the Rogue Encampment's fire is swapped for a new object, CampBonfire. It is the
  stash chest in every way the game cares about (a copy of the chest's row), so it can be
  clicked and opens the stash, but it wears the bonfire's look. The plugin can't see a click
  on a world object, only UI messages, so the bonfire borrows the one town object whose window
  we can put a button on. Changing the old fire's own row never made it clickable.
- Stash window: a Campfire button that opens the plugin's roster panel.
- Hero screen: new title. It is the Campfire: the game's own figures are moved out of sight and
  the plugin draws the camp's figures (src/plugin/campfire.cpp).
"""
import copy


def patch(t):
    # The new object: the stash chest's row under a new name. No stash mark on the map.
    t.table("objects").add_copy("Class", "Bank", **{
        "Class": "CampBonfire", "Name": "fire", "*Description": "Camp bonfire (opens the stash)",
        "*ID": str(len(t.table("objects").rows) - 1), "AutoMap": "0"})
    # Its look in the game's new graphics: the list of looks follows the table's row order, so
    # the new one goes last, and it uses the old bonfire's model, flames and sound.
    looks = t.data("hd/objects/objects.json")
    looks["camp_bonfire"] = {"asset_path": "env_wood"}
    look = copy.deepcopy(t.data("hd/objects/env_wood/rogue_bonfire.json"))
    look["name"] = "camp_bonfire"
    t.add_data("hd/objects/env_wood/camp_bonfire.json", look)
    del t.files["hd/objects/env_wood/rogue_bonfire.json"]  # unchanged, not shipped
    # The town's maps place "object 2 of Act 1" at the fire. That is now the new object.
    for row in t.table("objpreset").rows:
        if row.get("Act") == "1" and row.get("ObjectClass") == "RogueBonfire":
            row["ObjectClass"] = "CampBonfire"

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
