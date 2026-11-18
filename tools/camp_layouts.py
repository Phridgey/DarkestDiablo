"""The Campfire's own panels (save slots, traveler, roster). tools/build.py writes them into the
mod as data/global/ui/layouts/darkestdiablo/<Name>hd.json; the plugin opens them by name.

The plugin can show and hide widgets by name but can't change their text, so anything that
varies (which class is offered, who is by the fire) is a set of ready-made widgets of which the
plugin shows the right ones. Buttons talk to the plugin with a made-up panel name:
PanelManager:OpenPanel:darkestdiablo/<action> (see campfire.cpp, OnAction).
"""
CLASSES = ["Amazon", "Sorceress", "Necromancer", "Paladin", "Barbarian", "Druid", "Assassin", "Warlock"]
FRAME = "Panel\\Modals\\Modal_No_Title_BG"   # about 1226 wide and 428 tall
BUTTON_W, BUTTON_H = 513, 92                # the ModalButton picture at full size (measured in game)


def button(name, x, y, text, action=None, message=None, disabled=False, color="$FontColorWhite", scale=1.0):
    """`scale` shrinks the whole button, text included, so several fit side by side in the frame."""
    fields = {
        "rect": {"x": x, "y": y, "scale": scale} if scale != 1.0 else {"x": x, "y": y},
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
    """The first screen, and it fills the whole screen: nothing of the game's own menu may show
    behind it. A black screen, a gold title, and three of the main
    menu's large buttons, each with a line saying who is by that save's fire."""
    kids = [{"type": "RectangleWidget", "name": "Backdrop",
             "fields": {"anchor": {"x": -0.5}, "fitToScreen": True, "color": [0.0, 0.0, 0.0, 1.0]},
             "children": [{"type": "ClickCatcherWidget", "name": "catcher", "fields": {"fitToParent": True}}]},
            {"type": "TextBoxWidget", "name": "Heading",
             "fields": {"rect": {"x": -900, "y": 430, "width": 1800, "height": 90}, "fontType": "30pt",
                        "text": "Choose your save", "style": GOLD_TITLE}},
            line("Sub", 540, "Each save is its own camp, with its own heroes and stash.")]
    act = "PanelManager:OpenPanel:darkestdiablo/"
    for slot in (1, 2, 3):
        y = 720 + (slot - 1) * 330
        kids.append(menu_button(f"Slot{slot}", -305, y, f"Save {slot}",
                                f"PanelManager:OpenPanel:darkestdiablo/slot-{slot}", large=True))
        kids.append(line(f"Slot{slot}Summary", y + 150, f"SLOT{slot}SUMMARY", "$FontColorWhite"))
        kids.append(line(f"Slot{slot}Last", y + 210, "Last played", "$FontColorGoldYellow", "$MediumFontSize"))
        kids.append(menu_button(f"Slot{slot}Delete", 345, y + 22, "Delete", act + f"slot-delete-{slot}"))
    # Deleting a save is asked twice. Each question's words are a ready-made group per slot that
    # the plugin shows. A hidden button still takes clicks in this game, so no two buttons may
    # share a spot: the four answer buttons are made once (not per slot) and stand side by side
    # in one row, low on the screen where no slot button is. The second question's buttons are
    # to the right of the first's and swap order, so a double click can't answer both.
    for slot in (1, 2, 3):
        kids.append({"type": "Widget", "name": f"Confirm{slot}A", "fields": {"rect": {"x": 0, "y": 0}}, "children": [
            line("Title", 640, f"Delete Save {slot}?", "$FontColorGoldYellow", "$XExtraLargeFontSize"),
            line("Text", 780, "Its heroes, everything they carry and its stash will be gone."),
            line("Text2", 850, "The other saves are not touched.")]})
        kids.append({"type": "Widget", "name": f"Confirm{slot}B", "fields": {"rect": {"x": 0, "y": 0}}, "children": [
            line("Title", 640, "Are you sure?", "$FontColorRed", "$XExtraLargeFontSize"),
            line("Text", 780, f"Save {slot} can't be brought back. You won't be asked again.")]})
    kids.append(menu_button("AskYes", -1060, 1650, "Delete it", act + "slot-delete-sure"))
    kids.append(menu_button("AskNo", -540, 1650, "Keep it", act + "slot-delete-cancel"))
    kids.append(menu_button("SureNo", -20, 1650, "Keep it", act + "slot-delete-cancel"))
    kids.append(menu_button("SureYes", 500, 1650, "Yes, delete it", act + "slot-delete-yes"))
    kids.append(line("RestartNotice", 1740, "This save is ready. Close the game and start it again to play it.", "$FontColorWhite"))
    kids.append(menu_button("Exit", -235, 1840, "@d2r:strexitentiregame", "FrontEndNavigation:ToExit"))
    return {"type": "Panel", "name": "darkestdiablo/SaveSlots",
            "fields": {"anchor": {"x": 0.5}, "fitToParent": True, "priority": 9000}, "children": kids}


def traveler():
    # Three offers side by side: each button at 0.7 size is about 360 wide and 64 tall. A hidden
    # button still takes clicks, so each place has ONE button (recruit-place-<n>; the plugin knows
    # which class is offered there) and only the class names on it are stacked and shown by name.
    scale = 0.7
    w, h = round(BUTTON_W * scale), round(BUTTON_H * scale)
    kids = [text("Heading", 78, 50, 1076, 70, "A traveler joins your camp", "$StyleSettingsTitle"),
            text("Help", 135, 125, 956, 90, "Choose who stays by the fire. They wait at the Campfire as a new hero.")]
    label = {"fontColor": "$FontColorWhite", "alignment": {"h": "center", "v": "center"}, "pointSize": "$MediumFontSize"}
    for place, x in enumerate((55, 433, 811)):
        kids.append(button(f"Offer{place}", x, 225, "", action=f"recruit-place-{place}", scale=scale))
        for cls in CLASSES:
            kids.append(text(f"Offer{place}{cls}", x, 225, w, h, cls, label))
    kids.append(button("NotNow", 433, 320, "Not now", action="traveler-later", scale=scale))
    return panel("Traveler", kids)


def campfire():
    # Opens with the stash when the fire is used: a small window at the top of the screen, clear
    # of the stash and the inventory, that takes the hero back to the Campfire scene.
    scale = 0.7
    kids = [text("Heading", 78, 40, 1076, 70, "The Campfire", "$StyleSettingsTitle"),
            text("Help", 135, 115, 956, 90, "Rest by the fire to save and return to the Campfire, where your heroes wait.")]
    kids.append(button("ToCampfire", 255, 300, "Rest at the Campfire", action="to-campfire", scale=scale))
    kids.append(button("Close", 625, 300, "@d2r:strClose", message="PanelManager:ClosePanel:darkestdiablo/Campfire", scale=scale))
    return panel("Campfire", kids, frame_y=-1000)


LAYOUTS = {"Traveler": traveler, "Campfire": campfire}  # save_slots goes out as a template (tools/build.py)
