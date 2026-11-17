"""The Campfire outside the plugin: the town bonfire, the camp screen, the stash button.

- Bonfire: the Rogue Encampment's fire becomes clickable and opens the stash, like the stash
  chest (OperateFn 32). The plugin can't see a click on a world object, only UI messages, so the
  bonfire borrows the one town object whose window we can put a button on.
- Stash window: a Campfire button that opens the plugin's roster panel.
- Hero screen: new title. It is the Campfire: the plugin adds the camp's heroes to it
  (src/plugin/campfire.cpp), so the game's character list is no longer used.
"""


def patch(t):
    fire = t.table("objects").first("Class", "RogueBonfire")
    for mode in range(8):  # whichever state the town's fire is in
        fire[f"Selectable{mode}"] = "1"
    fire["OperateFn"] = "32"
    # A click box around the fire ring, like the Tree of Inifuss has. Without one the game
    # tests the mouse against the old flame picture, and the fire could not be clicked.
    fire.update({"Left": "-70", "Top": "-70", "Width": "140", "Height": "100"})

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
