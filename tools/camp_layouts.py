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
PANEL_SCALE = 1.3                       # FrontEnd_DifficultyModal (the Normal/Nightmare/Hell card) is 544 x 457
PANEL_W, PANEL_H = int(544 * PANEL_SCALE), int(457 * PANEL_SCALE)
PANEL_Y = (2160 - (int(457 * 1.3) + 25 + 118)) // 2  # the cards and their buttons, centred top to bottom
PANEL_GAP = 50
PANEL_X = [-(PANEL_W * 3 + PANEL_GAP * 2) // 2 + i * (PANEL_W + PANEL_GAP) for i in range(3)]
HERO_LINES = 8                          # one hero per class at most: two columns of four
BUTTON_Y = PANEL_Y + PANEL_H + 25
ROW_Y = 1930                            # the delete questions' answers


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
    """One save as the game's difficulty card: "Delve N", its living heroes as "level class"
    (two columns of four), and the difficulty it plays on, large at the bottom. The plugin
    writes the @@...@@ marks (campfire.cpp, RegisterSlotsLayout); an empty save shows none."""
    x = PANEL_X[slot - 1]
    m = f"@@S{slot}"
    kids = [{"type": "ImageWidget", "name": "Frame",
             "fields": {"rect": {"x": x, "y": PANEL_Y, "scale": PANEL_SCALE}, "filename": FE + "FrontEnd_DifficultyModal"}},
            styled("Title", x + 60, PANEL_Y + 40, PANEL_W - 200, 80, f"Delve {slot}", "$FontColorGoldYellow",
                   "$XExtraLargeFontSize", font="30pt")]
    col_w = (PANEL_W - 120) // 2
    for i in range(HERO_LINES):
        kids.append(styled(f"Hero{i + 1}", x + 60 + (i // 4) * col_w, PANEL_Y + 175 + (i % 4) * 52, col_w, 50,
                           f"{m}HERO{i + 1}@@", "$LightButtonTextColor", "$XMediumFontSize"))
    kids.append(styled("Recruits", x + 60, PANEL_Y + 385, PANEL_W - 120, 44, f"{m}RECRUITS@@",
                       "$CharacterListDetailsColor", "$SmallFontSize"))
    kids.append(styled("Difficulty", x + 30, PANEL_Y + PANEL_H - 150, PANEL_W - 60, 100, f"{m}DIFF@@", f"{m}DIFFCOLOR@@",
                       "$HugeFontSize", align="center", font="30pt"))
    return {"type": "Widget", "name": f"Delve{slot}", "fields": {"rect": {"x": 0, "y": 0}}, "children": kids}


def save_slots():
    """The first screen. It is laid over the empty Campfire (the hero screen with nobody at the
    fire) and darkens it; the plugin writes the darkness in (@@BACKDROP@@: see-through over the
    fire, black where the game's own menu would show). Three saves ("Delves") side by side in
    the game's difficulty card, each with its heroes, its difficulty and one button under it
    (Continue, or Start New when it is empty)."""
    kids = [{"type": "RectangleWidget", "name": "Backdrop",
             "fields": {"anchor": {"x": -0.5}, "fitToScreen": True, "color": "@@BACKDROP@@"},
             "children": [{"type": "ClickCatcherWidget", "name": "catcher", "fields": {"fitToParent": True}}]}]
    act = "PanelManager:OpenPanel:darkestdiablo/"
    for slot in (1, 2, 3):
        x = PANEL_X[slot - 1]
        kids.append(delve(slot))
        kids.append(styled(f"Slot{slot}Last", x + 60, PANEL_Y + 112, PANEL_W - 200, 44, "Last played",
                           "$CharacterListDetailsColor", "$SmallFontSize"))
        # One button per save; its words are written in by the plugin (a hidden button still
        # takes clicks in this game, so Continue and Start New can't be two buttons in one spot).
        kids.append(fe_button(f"Slot{slot}", x + (PANEL_W - 470) // 2, BUTTON_Y, f"@@S{slot}BUTTON@@",
                              act + f"slot-{slot}"))
        kids.append({"type": "ButtonWidget", "name": f"Slot{slot}Delete",
                     "fields": {"rect": {"x": x + PANEL_W - 150, "y": PANEL_Y + 40}, "filename": FE + "FrontEnd_Delete",
                                "onClickMessage": act + f"slot-delete-{slot}", "pressedFrame": 1, "hoveredFrame": 3,
                                "disabledFrame": 2}})
    # Deleting a save is asked twice. Each question's words are a ready-made group per slot that
    # the plugin shows. A hidden button still takes clicks in this game, so no two buttons may
    # share a spot: the four answer buttons are made once (not per slot) and stand in the bottom
    # row, clear of each other. The second question's buttons are to the
    # right of the first's and swap order, so a double click can't answer both.
    for slot in (1, 2, 3):
        kids.append({"type": "Widget", "name": f"Confirm{slot}A", "fields": {"rect": {"x": 0, "y": 0}}, "children": [
            line("Title", 640, f"Delete Delve {slot}?", "$FontColorGoldYellow", "$XExtraLargeFontSize"),
            line("Text", 780, "Its heroes, everything they carry and its stash will be gone."),
            line("Text2", 850, "The other Delves are not touched.")]})
        kids.append({"type": "Widget", "name": f"Confirm{slot}B", "fields": {"rect": {"x": 0, "y": 0}}, "children": [
            line("Title", 640, "Are you sure?", "$FontColorRed", "$XExtraLargeFontSize"),
            line("Text", 780, f"Delve {slot} can't be brought back. You won't be asked again.")]})
    kids.append(fe_button("AskYes", -1460, ROW_Y, "Delete it", act + "slot-delete-sure"))
    kids.append(fe_button("AskNo", -960, ROW_Y, "Keep it", act + "slot-delete-cancel"))
    kids.append(fe_button("SureNo", 490, ROW_Y, "Keep it", act + "slot-delete-cancel"))
    kids.append(fe_button("SureYes", 990, ROW_Y, "Yes, delete it", act + "slot-delete-yes"))
    kids.append(line("RestartNotice", 1000, "This Delve is ready. Close the game and start it again to play it.", "$FontColorWhite"))
    # Exit in the top left corner, the same button in the same place as at the Campfire
    # (ExitCorner in campfire.cpp, FireLayoutJson).
    exit_button = fe_button("ExitGame", 100, 60, "@d2r:strexitentiregame", "FrontEndNavigation:ToExit")
    exit_button["fields"].update({"pointSize": "$LargeFontSize", "textColor": "$LightButtonTextColor"})
    kids.append({"type": "Widget", "name": "Exit", "fields": {"anchor": {"x": -0.5, "y": 0.0}}, "children": [exit_button]})
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
