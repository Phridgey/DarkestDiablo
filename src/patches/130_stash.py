"""The caravan's stash window (docs/stash.md): no personal page, and an Upgrades tab.

The game ties the tabs to their numbers (0 personal, 1 shared), so the personal tab can't be taken
out: the tab bar is moved one tab to the left instead, which puts the personal tab (without picture
or text) outside the window, under a click catcher. The plugin moves anything left in a hero's
personal page to the shared pages (src/plugin/stash.cpp).

The Upgrades tab is an ordinary button in the second tab's place; the plugin catches its message.
Not changed: the old (legacy, "G" key) graphics and the controller layouts.
"""
TAB = 195


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
        fields[frames][0] = -1
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
