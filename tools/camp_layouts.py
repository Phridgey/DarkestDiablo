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


FE = "FrontEnd\\HD\\Final\\"
PANEL_SCALE = 0.8                       # FrontEnd_RightPanel is 803 x 2160 (the hero list's frame)
PANEL_W, PANEL_H = int(803 * PANEL_SCALE), int(2160 * PANEL_SCALE)
PANEL_Y = 40
PANEL_X = [-1023, -321, 381]            # three frames side by side, 60 apart, centred
HERO_LINES = 8                          # one hero per class at most


def styled(name, x, y, width, height, value, color, size, align="left", font="16pt"):
    """A text line in the hero list's own style (character select: gold, drop shadow)."""
    return {"type": "TextBoxWidget", "name": name,
            "fields": {"rect": {"x": x, "y": y, "width": width, "height": height}, "fontType": font, "text": value,
                       "style": {"fontColor": color, "pointSize": size, "spacing": "$ReducedSpacing",
                                 "alignment": {"h": align, "v": "center"}, "dropShadow": "$DefaultDropShadow"}}}


def fe_button(name, x, y, text, message, art="FrontEnd_ButtonMed"):
    return {"type": "ButtonWidget", "name": name,
            "fields": {"rect": {"x": x, "y": y}, "filename": FE + art, "textString": text, "onClickMessage": message,
                       "pressedFrame": 1, "hoveredFrame": 3, "disabledFrame": 2, "text/style": "$StyleFEButtonText"}}


def delve(slot):
    """One save as the game's hero list frame: "Delve N", its living heroes as "level class",
    and the difficulty it plays on, large at the bottom. The plugin writes the @@...@@ marks
    (campfire.cpp, RegisterSlotsLayout); an empty save has no heroes and no difficulty."""
    x = PANEL_X[slot - 1]
    m = f"@@S{slot}"
    kids = [{"type": "ImageWidget", "name": "Frame",
             "fields": {"rect": {"x": x, "y": PANEL_Y, "scale": PANEL_SCALE}, "filename": FE + "FrontEnd_RightPanel"}},
            styled("Title", x + 70, PANEL_Y + 90, PANEL_W - 140, 90, f"Delve {slot}", "$FontColorGoldYellow",
                   "$XExtraLargeFontSize", font="30pt")]
    for i in range(HERO_LINES):
        kids.append(styled(f"Hero{i + 1}", x + 80, PANEL_Y + 330 + i * 64, PANEL_W - 160, 60, f"{m}HERO{i + 1}@@",
                           "$LightButtonTextColor", "$XMediumLargeFontSize"))
    kids.append(styled("Recruits", x + 80, PANEL_Y + 330 + HERO_LINES * 64 + 20, PANEL_W - 160, 50, f"{m}RECRUITS@@",
                       "$CharacterListDetailsColor", "$XMediumFontSize"))
    kids.append(styled("Difficulty", x + 40, PANEL_Y + 1270, PANEL_W - 80, 110, f"{m}DIFF@@", f"{m}DIFFCOLOR@@",
                       "$HugeFontSize", align="center", font="30pt"))
    return {"type": "Widget", "name": f"Delve{slot}", "fields": {"rect": {"x": 0, "y": 0}}, "children": kids}


def save_slots():
    """The first screen, and it fills the whole screen: nothing of the game's own menu may show
    behind it. Three saves ("Delves") side by side in the frame of the game's hero list, each
    with its heroes, its difficulty and one button (Continue, or Start New when it is empty)."""
    kids = [{"type": "RectangleWidget", "name": "Backdrop",
             "fields": {"anchor": {"x": -0.5}, "fitToScreen": True, "color": [0.0, 0.0, 0.0, 1.0]},
             "children": [{"type": "ClickCatcherWidget", "name": "catcher", "fields": {"fitToParent": True}}]}]
    act = "PanelManager:OpenPanel:darkestdiablo/"
    for slot in (1, 2, 3):
        x = PANEL_X[slot - 1]
        kids.append(delve(slot))
        kids.append(styled(f"Slot{slot}Last", x + 70, PANEL_Y + 175, PANEL_W - 140, 50, "Last played",
                           "$CharacterListDetailsColor", "$XMediumFontSize"))
        # One button per save; its words are written in by the plugin (a hidden button still
        # takes clicks in this game, so Continue and Start New can't be two buttons in one spot).
        kids.append(fe_button(f"Slot{slot}", x + (PANEL_W - 470) // 2, PANEL_Y + 1450, f"@@S{slot}BUTTON@@",
                              act + f"slot-{slot}"))
        kids.append({"type": "ButtonWidget", "name": f"Slot{slot}Delete",
                     "fields": {"rect": {"x": x + PANEL_W - 160, "y": PANEL_Y + 92}, "filename": FE + "FrontEnd_Delete",
                                "onClickMessage": act + f"slot-delete-{slot}", "pressedFrame": 1, "hoveredFrame": 3,
                                "disabledFrame": 2}})
    # Deleting a save is asked twice. Each question's words are a ready-made group per slot that
    # the plugin shows. A hidden button still takes clicks in this game, so no two buttons may
    # share a spot: the four answer buttons are made once (not per slot) and stand in the row
    # under the frames, clear of the Exit button in the middle. The second question's buttons
    # are to the right of the first's and swap order, so a double click can't answer both.
    for slot in (1, 2, 3):
        kids.append({"type": "Widget", "name": f"Confirm{slot}A", "fields": {"rect": {"x": 0, "y": 0}}, "children": [
            line("Title", 640, f"Delete Delve {slot}?", "$FontColorGoldYellow", "$XExtraLargeFontSize"),
            line("Text", 780, "Its heroes, everything they carry and its stash will be gone."),
            line("Text2", 850, "The other Delves are not touched.")]})
        kids.append({"type": "Widget", "name": f"Confirm{slot}B", "fields": {"rect": {"x": 0, "y": 0}}, "children": [
            line("Title", 640, "Are you sure?", "$FontColorRed", "$XExtraLargeFontSize"),
            line("Text", 780, f"Delve {slot} can't be brought back. You won't be asked again.")]})
    row = PANEL_Y + PANEL_H + 70
    kids.append(fe_button("AskYes", -1460, row, "Delete it", act + "slot-delete-sure"))
    kids.append(fe_button("AskNo", -960, row, "Keep it", act + "slot-delete-cancel"))
    kids.append(fe_button("SureNo", 490, row, "Keep it", act + "slot-delete-cancel"))
    kids.append(fe_button("SureYes", 990, row, "Yes, delete it", act + "slot-delete-yes"))
    kids.append(line("RestartNotice", 1000, "This Delve is ready. Close the game and start it again to play it.", "$FontColorWhite"))
    kids.append(fe_button("Exit", -235, row, "@d2r:strexitentiregame", "FrontEndNavigation:ToExit"))
    return {"type": "Panel", "name": "darkestdiablo/SaveSlots",
            "fields": {"anchor": {"x": 0.5}, "fitToParent": True, "priority": 9000}, "children": kids}


def traveler():
    # The notice in town when a traveler arrives. The choice itself is made at the Campfire,
    # which shows only the offered classes (campfire.cpp, traveler mode).
    kids = [text("Heading", 78, 50, 1076, 70, "A traveler waits by your fire", "$StyleSettingsTitle"),
            text("Help", 135, 125, 956, 90, "Use the fire to meet them at the Campfire and choose who stays.")]
    kids.append(button("Ok", 357, 280, "OK", message="PanelManager:ClosePanel:darkestdiablo/Traveler"))
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
