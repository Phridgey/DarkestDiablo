"""Writes the route map pages of Acts III to V (docs/act3.md, act4.md, act5.md), and Anya's
scroll (the choice of resistance, docs/act5.md).

Usage: python tools/route_pages.py

Each page is a menu layout made of buttons (one per stop, plus a grey "Locked" twin in the same
spot, see RefreshRouteMap in darkestdiablo_plugin.cpp) joined by drawn lines. The game uses the
copy built into the plugin, so this writes both: the mod files
src/data/data/global/ui/layouts/darkestdiablo/RouteMap<act>hd.json and src/plugin/routepages.h.
tests/test_route_map.py checks the two match and that every built stop in route.h has a button.
"""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LAYOUTS = ROOT / "src" / "data" / "data" / "global" / "ui" / "layouts" / "darkestdiablo"
HEADER = ROOT / "src" / "plugin" / "routepages.h"

# Each tier: (note above the row or None, [(node, label, travel level, kind under it or None)]).
PAGES = {
    3: ("Act III - Choose your path", 75, [
        ("All three, any order", [("NodeSpider", "Spider Cavern", 85, "Khalim's Eye"),
                                  ("NodeFlayer", "Flayer Dungeon", 91, "Khalim's Brain"),
                                  ("NodeKSewers", "Kurast Sewers", 93, "Khalim's Heart")]),
        ("Fork - pick 2", [("Fork5LamEsen", "Lam Esen's Tome", 94, "XP grind"),
                           ("Fork5Swampy", "Swampy Pit", 90, "MF run"),
                           ("Fork5LowerKurast", "Lower Kurast", 95, "Crafting hunt")]),
        (None, [("NodeTravincal", "Travincal", 96, None)]),
        (None, [("NodeDurance", "Durance of Hate", 102, None)]),
    ]),
    4: ("Act IV - Choose your path", 103, [
        (None, [("NodePlains", "Plains of Despair", 105, None)]),
        ("Fork - pick 2", [("Fork6Abaddon", "Abaddon", 125, "XP grind"),
                           ("Fork6Acheron", "Pit of Acheron", 126, "MF run"),
                           ("Fork6Infernal", "Infernal Pit", 127, "Crafting hunt")]),
        (None, [("NodeHellforge", "Hellforge", 107, None)]),
        (None, [("NodeChaos", "Chaos Sanctuary", 108, None)]),
    ]),
    5: ("Act V - Choose your path", 109, [
        (None, [("NodeShenk", "Shenk the Overseer", 116, None)]),
        ("Fork - pick 2", [("Fork7Highlands", "Frigid Highlands", 113, "XP grind"),
                           ("Fork7Pindle", "Pindleskin", 121, "MF run"),
                           ("Fork7Cellar", "Icy Cellar", 119, "Crafting hunt")]),
        (None, [("NodeAnya", "Anya", 114, None)]),
        ("Fork - pick 2", [("Fork8Vaught", "Halls of Vaught", 124, "MF run"),
                           ("Fork8Cows", "Cow Level", 39, "Crafting hunt")]),
        (None, [("NodeAncients", "The Ancients", 120, None)]),
        (None, [("NodeBaal", "Throne of Destruction", 131, None)]),
    ]),
}

XS = {1: [406], 2: [218, 594], 3: [30, 406, 782]}
BUTTON_H = 85
KIND_H = 28


def button(name, x, y, label, level, live):
    fields = {"rect": {"x": x, "y": y}, "filename": "FrontEnd\\HD\\Final\\FrontEnd_ButtonSmall", "pressedFrame": 1,
              "disabledFrame": 2, "hoveredFrame": 3, "textString": label, "pointSize": "$SmallFontSize",
              "textColor": "$FontColorWhite" if live else "$FontColorDarkGrayBlue", "disabled": True}
    if live:
        fields["onClickMessage"] = f"PanelManager:OpenPanel:darkestdiablo/travel-{level}"
    return {"type": "ButtonWidget", "name": name if live else name + "Locked", "fields": fields}


def text(name, x, y, w, h, value, color, size="$SmallFontSize"):
    return {"type": "TextBoxWidget", "name": name, "fields": {"rect": {"x": x, "y": y, "width": w, "height": h}, "text": value,
            "style": {"fontColor": color, "pointSize": size, "alignment": {"h": "center", "v": "center"}}}}


def page(act):
    title, town, tiers = PAGES[act]
    panel = f"darkestdiablo/RouteMap{act}"
    kids = [
        {"type": "ImageWidget", "name": "Background", "fields": {"rect": {"x": 0, "y": 0}, "filename": "PANEL\\Waypoints\\Waypoints_Base", "frame": 0}},
        {"type": "ImageWidget", "name": "LeftHinge", "fields": {"rect": "$LeftHingeRect", "filename": "$LeftHingeSprite"}},
        {"type": "TextBoxWidget", "name": "Title", "fields": {"rect": "$InGamePanelTitleRect", "text": title, "style": "$StyleTitleBlock"}},
        button("NodeTown", 30, 110, "Return to town", town, False),
        button("NodeTown", 30, 110, "Return to town", town, True),
    ]
    lines = 0

    def line(x, y, w, chars):
        nonlocal lines
        lines += 1
        kids.append(text(f"Line{lines}", x, y, w, 40, chars, "$FontColorGoldYellow", "$MediumFontSize"))

    y = 190 if tiers[0][0] is None else 235
    previous = None  # (centres, bottom)
    for t, (note, stops) in enumerate(tiers):
        xs = XS[len(stops)]
        centres = [x + 175 for x in xs]
        if previous is not None:
            above, bottom = previous
            for c in above:
                line(c - 20, bottom - 5, 40, "|")
            joined = sorted(set(above) | set(centres))
            if len(joined) > 1:
                width = joined[-1] - joined[0]
                line(joined[0], bottom + 5, width, "_" * round(width / 15.67))
            for c in centres:
                line(c - 20, bottom + 40, 40, "|")
            y = bottom + 90
        if note:
            kids.append(text(f"Note{t}", 0, y - 45, 1162, 30, note, "$FontColorDarkGrayGold"))
        for x, (node, label, level, kind) in zip(xs, stops):
            kids.append(button(node, x, y, label, level, False))
            kids.append(button(node, x, y, label, level, True))
            if kind:
                kids.append(text(f"Kind{t}_{node}", x, y + 82, 350, KIND_H, kind, "$FontColorDarkGrayGold"))
        bottom = y + (110 if any(k for *_, k in stops) else BUTTON_H)
        previous = (centres, bottom)
    kids.append({"type": "ButtonWidget", "name": "CloseButton", "fields": {"rect": {"x": 1075, "y": 9}, "filename": "PANEL\\closebtn_4x",
                 "hoveredFrame": 3, "tooltipString": "@d2r:strClose", "sound": "cursor_close_window_hd",
                 "onClickMessage": f"PanelManager:ClosePanel:{panel}", "pressedFrame": 1}})
    return {"type": "Panel", "name": panel, "fields": {"rect": "$LeftPanelRect", "anchor": "$LeftPanelAnchor", "priority": 8500},
            "children": kids}


ANYA_CHOICES = [("Fire", "fire"), ("Cold", "cold"), ("Lightning", "lightning"), ("Poison", "poison")]


def anya_scroll():
    """Anya's reward (docs/act5.md): a choice of one resistance whose maximum rises by 5."""
    panel = "darkestdiablo/AnyaScroll"
    kids = [
        {"type": "ImageWidget", "name": "Background", "fields": {"rect": {"x": 0, "y": 0}, "filename": "PANEL\\Waypoints\\Waypoints_Base", "frame": 0}},
        {"type": "ImageWidget", "name": "LeftHinge", "fields": {"rect": "$LeftHingeRect", "filename": "$LeftHingeSprite"}},
        {"type": "TextBoxWidget", "name": "Title", "fields": {"rect": "$InGamePanelTitleRect", "text": "Anya's Scroll", "style": "$StyleTitleBlock"}},
        text("Hint", 0, 160, 1162, 60, "Choose a resistance: its maximum rises by 5.", "$FontColorDarkGrayGold"),
    ]
    for k, (label, key) in enumerate(ANYA_CHOICES):
        kids.append({"type": "ButtonWidget", "name": f"Anya{label}", "fields": {
            "rect": {"x": 406, "y": 260 + 130 * k}, "filename": "FrontEnd\\HD\\Final\\FrontEnd_ButtonSmall", "pressedFrame": 1,
            "disabledFrame": 2, "hoveredFrame": 3, "textString": f"{label} resistance", "pointSize": "$SmallFontSize",
            "textColor": "$FontColorWhite", "onClickMessage": f"PanelManager:OpenPanel:darkestdiablo/anya-{key}"}})
    kids.append({"type": "ButtonWidget", "name": "CloseButton", "fields": {"rect": {"x": 1075, "y": 9}, "filename": "PANEL\\closebtn_4x",
                 "hoveredFrame": 3, "tooltipString": "@d2r:strClose", "sound": "cursor_close_window_hd",
                 "onClickMessage": f"PanelManager:ClosePanel:{panel}", "pressedFrame": 1}})
    return {"type": "Panel", "name": panel, "fields": {"rect": "$LeftPanelRect", "anchor": "$LeftPanelAnchor", "priority": 8500},
            "children": kids}


def main():
    header = ["// Generated by tools/route_pages.py: the route map pages of Acts III to V. Do not edit by hand;",
              "// change the script and run it (it also writes the matching RouteMap<act>hd.json files).",
              "#pragma once", ""]
    for act in PAGES:
        layout = page(act)
        packed = json.dumps(layout, separators=(",", ":"), ensure_ascii=False)
        (LAYOUTS / f"RouteMap{act}hd.json").write_text(json.dumps(layout, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        header.append(f'static constexpr char RouteMap{act}Layout[] = R"json({packed})json";')
    anya = anya_scroll()
    (LAYOUTS / "AnyaScrollhd.json").write_text(json.dumps(anya, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    header.append(f'static constexpr char AnyaScrollLayout[] = R"json({json.dumps(anya, separators=(",", ":"), ensure_ascii=False)})json";')
    HEADER.write_text("\n".join(header) + "\n", encoding="utf-8")
    print("wrote", HEADER.relative_to(ROOT), "and", len(PAGES), "layouts")


if __name__ == "__main__":
    main()
