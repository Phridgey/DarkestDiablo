"""The caravan's stash window (docs/stash.md): no personal page, and an Upgrades tab.

The game ties the tabs to their numbers (0 personal, 1 shared), so the personal tab can't be taken
out: the tab bar is moved one tab to the left instead, which puts the personal tab (a see-through
picture, no text) outside the window, under a click catcher. (Frame -1 does not hide a tab: the game
drew it as a black box, 0.20.5.) The plugin moves anything left in a hero's
personal page to the shared pages (src/plugin/stash.cpp).

The Upgrades tab is an ordinary button in the second tab's place; the plugin catches its message.
Not changed: the old (legacy, "G" key) graphics and the controller layouts.
"""
import struct

TAB = 195
PICTURE = "hd/global/ui/panel/stash/mkb_tabs_195_tga{}.sprite"
OURS = "hd/global/ui/panel/darkestdiablo/stashtabs{}.sprite"
BLANK = 2


def with_blank_frame(data):
    """The game's two-frame tab picture (frames side by side, RGBA) with a see-through third frame."""
    frame = struct.unpack_from("<H", data, 6)[0]
    width, height = struct.unpack_from("<II", data, 8)
    frames, size = struct.unpack_from("<I", data, 20)[0], struct.unpack_from("<I", data, 32)[0]
    if data[:4] != b"SpA1" or frames != 2 or size != width * height * 4 or len(data) != 40 + size or frame * 2 > width:
        raise RuntimeError("The stash's tab picture is not the one this patch knows; has the game changed?")
    header = bytearray(data[:40])
    struct.pack_into("<I", header, 8, frame * 3)
    struct.pack_into("<I", header, 20, 3)
    struct.pack_into("<I", header, 32, frame * 3 * height * 4)
    out = bytearray(header)
    for y in range(height):
        row = 40 + y * width * 4
        out += data[row:row + frame * 2 * 4] + bytes(frame * 4)
    return bytes(out)


def patch(t):
    bank = t.layout("bankexpansionlayouthd")
    tabs = t.widget(bank, "BankTabs")
    fields = tabs["fields"]
    if fields["tabSize"]["x"] != TAB or fields["textStrings"][:2] != ["@personal", "@shared"]:
        raise RuntimeError("The stash's tabs are not the ones this patch knows; has the game changed?")
    x, y = fields["rect"]["x"], fields["rect"]["y"]
    fields["rect"] = dict(fields["rect"], x=x - TAB)
    fields["textStrings"][0] = ""
    for frames in ("inactiveFrames", "activeFrames"):
        fields[frames][0] = BLANK
    fields["filename"] = "PANEL\\DarkestDiablo\\StashTabs"
    for suffix in ("", ".lowend"):
        t.add_file(OURS.format(suffix), with_blank_frame(t.game_file(PICTURE.format(suffix))))
    at = bank["children"].index(tabs)
    bank["children"][at + 1:at + 1] = [
        {"type": "ClickCatcherWidget", "name": "PersonalTabCover",
         "fields": {"rect": {"x": x - TAB, "y": y, "width": TAB, "height": fields["tabSize"]["y"]}}},
        {"type": "ButtonWidget", "name": "UpgradesTab", "fields": {
            "rect": {"x": x + TAB, "y": y}, "filename": fields["filename"],
            "normalFrame": 0, "pressedFrame": 1, "hoveredFrame": 1, "disabledFrame": 0,
            "textString": "Upgrades", "pointSize": "$SmallPanelFontSize", "textColor": "$TabsInactiveTextColor",
            "onClickMessage": "PanelManager:OpenPanel:darkestdiablo/stash-upgrades"}},
    ]
