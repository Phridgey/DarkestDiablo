"""The Campfire outside the plugin: the town bonfire, the camp screen, the stash button.

- Bonfire: the Rogue Encampment's fire becomes clickable and opens the stash, like the stash
  chest (OperateFn 32). The plugin can't see a click on a world object, only UI messages, so the
  bonfire borrows the one town object whose window we can put a button on.
- Stash window: a Campfire button that opens the plugin's roster panel.
- Main menu (the character list): staged at the campfire instead of the five act backdrops, so
  the selected hero stands by the fire in the gear it really wears.
- Hero screen: new title; the game's own campfire with the eight classes.
"""
# Where the selected hero stands: between the Barbarian and the Paladin, a step toward the fire.
HERO_SPOT = {"position": [1051.5, 0.1, 115.5], "yawDegrees": 40}


def patch(t):
    fire = t.table("objects").first("Class", "RogueBonfire")
    for mode in ("Selectable0", "Selectable1", "Selectable2"):  # the fire burns in modes 1 and 2
        fire[mode] = "1"
    fire["OperateFn"] = "32"

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

    # The hero screen's camera looks at the campfire; give the same view to the character list.
    create = t.layout("charactercreatepanelhd")
    fire_camera = t.widget(create, "Camera")["fields"]
    menu = t.layout("mainmenupanelhd")
    for act in range(1, 6):
        camera = t.widget(menu, f"Act{act}Camera")["fields"]
        keep = {k: camera[k] for k in ("activateOnMessage", "activateOnOpened") if k in camera}
        camera.clear()
        camera.update({k: v for k, v in fire_camera.items() if k != "activateOnOpened"})
        camera.update(keep)
        t.widget(menu, f"Act{act}Player")["fields"].update(HERO_SPOT)
    t.widget(menu, "Player")["fields"].update(HERO_SPOT)

    t.widget(create, "Title")["fields"]["text"] = "Who joins the fire?"
    select = t.layout("characterselectpanelhd")
    t.widget(select, "CreateCharacter")["fields"]["textString"] = "Recruit"
