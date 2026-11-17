"""The Campfire's own panels (save slots, traveler, roster). tools/build.py writes them into the
mod as data/global/ui/layouts/darkestdiablo/<Name>hd.json; the plugin opens them by name.

The plugin can show and hide widgets by name but can't change their text, so anything that
varies (which class is offered, who is by the fire) is a set of ready-made widgets of which the
plugin shows the right ones. Buttons talk to the plugin with a made-up panel name:
PanelManager:OpenPanel:darkestdiablo/<action> (see campfire.cpp, OnAction).
"""
CLASSES = ["Amazon", "Sorceress", "Necromancer", "Paladin", "Barbarian", "Druid", "Assassin", "Warlock"]
FRAME = "Panel\\Modals\\Modal_No_Title_BG"   # about 1254 wide; buttons are 300 wide


def button(name, x, y, text, action=None, message=None, disabled=False, color="$FontColorWhite"):
    fields = {
        "rect": {"x": x, "y": y},
        "filename": "Panel\\Modals\\ModalButton",
        "focusIndicatorFilename": "Controller/HoverImages/ModalButton_Hover",
        "pressedFrame": 1,
        "disabledFrame": 2,
        "hoveredFrame": 3,
        "textString": text,
        "pointSize": "$MediumFontSize",
        "textColor": color,
    }
    if action:
        fields["onClickMessage"] = f"PanelManager:OpenPanel:darkestdiablo/{action}"
    if message:
        fields["onClickMessage"] = message
    if disabled:
        fields["disabled"] = True
    return {"type": "ButtonWidget", "name": name, "fields": fields}


def text(name, x, y, width, height, value, style="$StyleModalDialogDescription"):
    return {"type": "TextBoxWidget", "name": name,
            "fields": {"rect": {"x": x, "y": y, "width": width, "height": height}, "text": value, "style": style}}


def panel(name, children, frame_y=-270, backdrop=False):
    kids = []
    if backdrop:  # covers the menu behind and swallows clicks meant for it
        kids.append({"type": "RectangleWidget", "name": "Backdrop",
                     "fields": {"anchor": {"x": -0.5, "y": -0.5}, "fitToScreen": True, "color": [0.0, 0.0, 0.0, 0.9]},
                     "children": [{"type": "ClickCatcherWidget", "name": "catcher", "fields": {"fitToParent": True}}]})
    kids.append({"type": "ImageWidget", "name": "Frame",
                 "fields": {"rect": {"x": -627, "y": frame_y}, "filename": FRAME}, "children": children})
    return {"type": "Panel", "name": f"darkestdiablo/{name}",
            "fields": {"anchor": {"x": 0.5, "y": 0.5}, "priority": 9000}, "children": kids}


GOLD_TITLE = {"fontColor": "$FontColorGoldYellow", "spacing": "$ReducedSpacing", "alignment": {"h": "center"},
              "pointSize": "$XExtraLargeFontSize", "dropShadow": "$DefaultDropShadow"}


def line(name, y, value, color="$FontColorLightGray", size="$LargeFontSize"):
    """A centered line of text across the middle of the screen (main menu look)."""
    return {"type": "TextBoxWidget", "name": name,
            "fields": {"rect": {"x": -900, "y": y, "width": 1800, "height": 70}, "text": value,
                       "style": {"fontColor": color, "alignment": {"h": "center"}, "pointSize": size,
                                 "dropShadow": "$DefaultDropShadow"}}}


def menu_button(name, x, y, text, message, large=False):
    """A button in the main menu's own style (the game's Play and Options buttons)."""
    art = "FrontEnd_ButtonLarge" if large else "FrontEnd_ButtonMed"
    return {"type": "ButtonWidget", "name": name,
            "fields": {"rect": {"x": x, "y": y}, "filename": f"FrontEnd\\HD\\Final\\{art}",
                       "textString": text, "onClickMessage": message,
                       "pressedFrame": 1, "hoveredFrame": 3, "disabledFrame": 2,
                       "text/style": "$StyleFEButtonText",
                       "pointSize": "$ExtraLargeFontSize" if large else "$LargeFontSize",
                       "textColor": "$LightButtonTextColor"}}


def save_slots():
    """The first screen: the game's logo look. A dark screen, a gold title, and three of the main
    menu's large buttons, each with a line saying who is by that save's fire."""
    kids = [{"type": "RectangleWidget", "name": "Backdrop",
             "fields": {"anchor": {"x": -0.5}, "fitToScreen": True, "color": [0.0, 0.0, 0.0, 0.93]},
             "children": [{"type": "ClickCatcherWidget", "name": "catcher", "fields": {"fitToParent": True}}]},
            {"type": "TextBoxWidget", "name": "Heading",
             "fields": {"rect": {"x": -900, "y": 430, "width": 1800, "height": 90}, "fontType": "30pt",
                        "text": "Choose your save", "style": GOLD_TITLE}},
            line("Sub", 540, "Each save is its own camp, with its own heroes and stash.")]
    for slot in (1, 2, 3):
        y = 720 + (slot - 1) * 330
        kids.append(menu_button(f"Slot{slot}", -305, y, f"Save {slot}",
                                f"PanelManager:OpenPanel:darkestdiablo/slot-{slot}", large=True))
        kids.append(line(f"Slot{slot}Summary", y + 150, f"SLOT{slot}SUMMARY", "$FontColorWhite"))
        kids.append(line(f"Slot{slot}Last", y + 210, "Last played", "$FontColorGoldYellow", "$MediumFontSize"))
    kids.append(line("RestartNotice", 1740, "This save is ready. Close the game and start it again to play it.", "$FontColorWhite"))
    kids.append(menu_button("Exit", -235, 1840, "@d2r:strexitentiregame", "FrontEndNavigation:ToExit"))
    return {"type": "Panel", "name": "darkestdiablo/SaveSlots",
            "fields": {"anchor": {"x": 0.5}, "fitToParent": True, "priority": 9000}, "children": kids}


def traveler():
    kids = [text("Heading", 78, 60, 1076, 70, "A traveler joins your camp", "$StyleSettingsTitle"),
            text("Help", 150, 140, 956, 110, "Choose who stays by the fire. They wait at the Campfire as a new hero.")]
    for place, x in enumerate((120, 477, 834)):
        for cls in CLASSES:
            kids.append(button(f"Offer{place}{cls}", x, 275, cls, action=f"recruit-{cls}"))
    kids.append(button("NotNow", 477, 385, "Not now", action="traveler-later"))
    return panel("Traveler", kids)


def campfire():
    kids = [text("Heading", 78, 50, 1076, 70, "The Campfire", "$StyleSettingsTitle"),
            text("Help", 150, 120, 956, 80, "By the fire, waiting to join (new), or not in camp (grey).")]
    for i, cls in enumerate(CLASSES):
        x, y = 27 + (i % 4) * 300, 205 + (i // 4) * 85
        kids.append(button(f"Hero{cls}", x, y, cls))
        kids.append(button(f"Recruit{cls}", x, y, f"{cls} (new)", color="$FontColorGoldYellow"))
        kids.append(button(f"Locked{cls}", x, y, cls, disabled=True, color="$FontColorLightGray"))
    kids.append(button("ToCampfire", 327, 390, "Go to the fire", action="to-campfire"))
    kids.append(button("Close", 627, 390, "@d2r:strClose", message="PanelManager:ClosePanel:darkestdiablo/Campfire"))
    return panel("Campfire", kids)


LAYOUTS = {"Traveler": traveler, "Campfire": campfire}  # save_slots goes out as a template (tools/build.py)
