"""Writes the route map pages of all five acts, and Anya's scroll (the choice of resistance,
docs/act5.md).

Usage: python tools/route_pages.py

Each page is a menu layout (design pitch 2026-11-23, "Route map redesign pitch" on claude.ai):
- a button per stop (plus a grey "Locked" twin in the same spot, see RefreshRouteMap in
  darkestdiablo_plugin.cpp), joined by drawn lines;
- under it, the run's kind with its icon and colour (an item picture from the game: experience
  grind a Book of Skill, magic find run a Perfect Topaz, crafting hunt the Horadric Malus, story a
  bark scroll, act boss a Perfect Skull) and, in green, the stop's one-time reward;
- a legend of the kinds along the bottom.
A layout is fixed once the plugin loads (D2RLoader can't change our menus in a game), so whether a
stop is open, done or locked, and the picks left at a fork, go to the console when the map opens.
The game uses the copy built into the plugin, so this writes both: the mod files
src/data/data/global/ui/layouts/darkestdiablo/RouteMap[<act>]hd.json and src/plugin/routepages.h.
tests/test_route_map.py checks the two match and that every built stop in route.h has a button.
"""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LAYOUTS = ROOT / "src" / "data" / "data" / "global" / "ui" / "layouts" / "darkestdiablo"
HEADER = ROOT / "src" / "plugin" / "routepages.h"

# Run kinds: label, label colour, item picture (data/hd/global/ui/<path>.sprite), its scale.
KINDS = {
    "xp": ("Experience grind", "$FontColorLightBlue", "items\\misc\\quest\\book_of_skill", 0.26),
    "mf": ("Magic find run", "$FontColorGoldYellow", "items\\misc\\gem\\perfect_topaz", 0.52),
    "craft": ("Crafting hunt", "$FontColorLightPurple", "items\\weapon\\hammer\\horadric_malus", 0.17),
    "story": ("Story", "$FontColorBeige", "items\\misc\\quest\\bark_scroll", 0.26),
    "boss": ("Act boss", "$FontColorLightRed", "items\\misc\\gem\\perfect_skull", 0.52),
}
PERK = "$FontColorGreen"

# Each act: (title, town, tiers); each tier: (note above the row or None,
# [(node, label, travel level, kind, one-time reward in green or "")]).
PAGES = {
    1: ("Act I - Choose your path", 1, [
        (None, [("NodeDen", "Den of Evil", 13, "story", "+3 skill points")]),
        (None, [("NodeBurial", "Blood Raven", 10, "story", "")]),
        ("Fork - pick 2", [("NodeInifuss", "Rakanishu & Treehead", 12, "mf", "2 guaranteed sets or uniques"),
                           ("Fork1Mausoleum", "Mausoleum", 14, "craft", "Runes up to Ral"),
                           ("Fork1Crypt", "Crypt", 15, "xp", "+1 level")]),
        (None, [("NodeTristram", "Tristram", 38, "story", "Rare ring from Akara")]),
        ("Fork - pick 2", [("Fork2Pit", "Pit", 16, "mf", "Guaranteed set or unique"),
                           ("Fork2Countess", "Countess", 25, "craft", "5 runes, up to Ral"),
                           ("Fork2Malus", "Horadric Malus", 28, "xp", "Socket chisel, +1 level")]),
        (None, [("NodeAndariel", "Andariel", 37, "boss", "")]),
    ]),
    2: ("Act II - Choose your path", 40, [
        (None, [("NodeHalls", "Halls of the Dead", 60, "story", "Horadric Cube")]),
        ("Fork - pick 2", [("Fork3Sewers", "Sewers & Radament", 49, "xp", "+3 skill points, +1 level"),
                           ("Fork3Stony", "Stony Tomb", 55, "craft", "Runes up to Shael")]),
        ("Both, any order", [("NodeMaggot", "Maggot Lair", 64, "story", "Staff of Kings"),
                             ("NodeClawViper", "Claw Viper Temple", 61, "story", "Amulet of the Viper")]),
        ("Fork - pick 2", [("Fork4Arcane", "Arcane Sanctuary", 74, "craft", "Runes up to Shael"),
                           ("Fork4Tunnels", "Ancient Tunnels", 65, "mf", "Every pack led by a unique"),
                           ("Fork4Canyon", "Canyon of the Magi", 59, "xp", "+1 level")]),
        (None, [("NodeDuriel", "Duriel", 66, "boss", "")]),
    ]),
    3: ("Act III - Choose your path", 75, [
        ("All three, any order", [("NodeSpider", "Spider Cavern", 85, "story", "Khalim's Eye"),
                                  ("NodeFlayer", "Flayer Dungeon", 91, "story", "Khalim's Brain"),
                                  ("NodeKSewers", "Kurast Sewers", 93, "story", "Khalim's Heart")]),
        ("Fork - pick 2", [("Fork5LamEsen", "Lam Esen's Tome", 94, "xp", "+10 stat points, +1 level"),
                           ("Fork5Swampy", "Swampy Pit", 90, "mf", ""),
                           ("Fork5LowerKurast", "Lower Kurast", 95, "craft", "Runes up to Lem")]),
        (None, [("NodeTravincal", "Travincal", 96, "story", "")]),
        (None, [("NodeDurance", "Durance of Hate", 102, "boss", "")]),
    ]),
    4: ("Act IV - Choose your path", 103, [
        (None, [("NodePlains", "Plains of Despair", 105, "xp", "+4 skill points")]),
        ("Fork - pick 2", [("Fork6Abaddon", "Abaddon", 125, "xp", "+1 level"),
                           ("Fork6Acheron", "Pit of Acheron", 126, "mf", ""),
                           ("Fork6Infernal", "Infernal Pit", 127, "craft", "Runes up to Ohm")]),
        (None, [("NodeHellforge", "Hellforge", 107, "craft", "3 runes, Pul or better")]),
        (None, [("NodeChaos", "Chaos Sanctuary", 108, "boss", "")]),
    ]),
    5: ("Act V - Choose your path", 109, [
        (None, [("NodeShenk", "Shenk the Overseer", 116, "story", "Socket chisel")]),
        ("Fork - pick 2", [("Fork7Highlands", "Frigid Highlands", 113, "xp", "3 runes Pul to Ohm, +1 level"),
                           ("Fork7Pindle", "Pindleskin", 121, "mf", ""),
                           ("Fork7Cellar", "Icy Cellar", 119, "craft", "Runes, no cap")]),
        (None, [("NodeAnya", "Anya", 114, "mf", "+5 to a maximum resistance")]),
        ("Fork - pick 2", [("Fork8Vaught", "Halls of Vaught", 124, "mf", ""),
                           ("Fork8Cows", "Cow Level", 39, "craft", "Runes, no cap")]),
        (None, [("NodeAncients", "The Ancients", 120, "story", "+1 level")]),
        (None, [("NodeBaal", "Throne of Destruction", 131, "boss", "Elite set and unique")]),
    ]),
}

XS = {1: [406], 2: [218, 594], 3: [30, 406, 782]}
BUTTON_W, BUTTON_H = 350, 85
LINE_H = 26       # each of the two lines under a button: the kind, then the reward
ICON = 52         # the kind's picture, left of those two lines
CARD_H = BUTTON_H + 2 + 2 * LINE_H
GAP = 60          # between one row's card and the next row's button
LEGEND_Y = 1440


def panel_name(act):
    return "darkestdiablo/RouteMap" if act == 1 else f"darkestdiablo/RouteMap{act}"


def layout_name(act):
    return "RouteMapLayout" if act == 1 else f"RouteMap{act}Layout"


def file_name(act):
    return "RouteMaphd.json" if act == 1 else f"RouteMap{act}hd.json"


def button(name, x, y, label, level, live):
    fields = {"rect": {"x": x, "y": y}, "filename": "FrontEnd\\HD\\Final\\FrontEnd_ButtonSmall", "pressedFrame": 1,
              "disabledFrame": 2, "hoveredFrame": 3, "textString": label, "pointSize": "$SmallFontSize",
              "textColor": "$FontColorWhite" if live else "$FontColorDarkGrayBlue", "disabled": True}
    if live:
        fields["onClickMessage"] = f"PanelManager:OpenPanel:darkestdiablo/travel-{level}"
    return {"type": "ButtonWidget", "name": name if live else name + "Locked", "fields": fields}


def text(name, x, y, w, h, value, color, size="$SmallFontSize", align="center"):
    return {"type": "TextBoxWidget", "name": name, "fields": {"rect": {"x": x, "y": y, "width": w, "height": h}, "text": value,
            "style": {"fontColor": color, "pointSize": size, "alignment": {"h": align, "v": "center"}}}}


def icon(name, x, y, kind):
    picture, scale = KINDS[kind][2], KINDS[kind][3]
    return {"type": "ImageWidget", "name": name, "fields": {"rect": {"x": x, "y": y, "scale": scale}, "filename": picture}}


def page(act):
    title, town, tiers = PAGES[act]
    panel = panel_name(act)
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

    y = 190 if tiers[0][0] is None else 230
    previous = None  # (centres, bottom)
    for t, (note, stops) in enumerate(tiers):
        xs = XS[len(stops)]
        centres = [x + BUTTON_W // 2 for x in xs]
        if previous is not None:
            above, bottom = previous
            joined = sorted(set(above) | set(centres))
            for c in above:
                line(c - 20, bottom - 12, 40, "|")
            if len(joined) > 1:
                width = joined[-1] - joined[0]
                line(joined[0], bottom - 4, width, "_" * round(width / 15.67))
            if not note:
                for c in centres:
                    line(c - 20, bottom + 24, 40, "|")
            y = bottom + GAP + (16 if note else 0)  # room for the note
        if note:
            kids.append(text(f"Note{t}", 0, y - 34, 1162, 30, note, "$FontColorDarkGrayGold"))
        for x, (node, label, level, kind, perk) in zip(xs, stops):
            kids.append(button(node, x, y, label, level, False))
            kids.append(button(node, x, y, label, level, True))
            top = y + BUTTON_H + 2
            kids.append(icon(f"Icon{t}_{node}", x + 4, top, kind))
            kids.append(text(f"Kind{t}_{node}", x + ICON + 12, top, BUTTON_W - ICON - 12, LINE_H, KINDS[kind][0], KINDS[kind][1], align="left"))
            if perk:
                kids.append(text(f"Perk{t}_{node}", x + ICON + 12, top + LINE_H, BUTTON_W - ICON - 12, LINE_H, perk, PERK, align="left"))
        previous = (centres, y + CARD_H)
    # Legend: the five kinds, and what green means.
    xs = [30, 250, 470, 690, 910]
    for k, (kind, (label, color, _, _)) in enumerate(KINDS.items()):
        kids.append(icon(f"LegendIcon{k}", xs[k], LEGEND_Y - 8, kind))
        kids.append(text(f"Legend{k}", xs[k] + ICON + 6, LEGEND_Y, 170, LINE_H, label, color, align="left"))
    kids.append(text("LegendPerk", 0, LEGEND_Y - 40, 1162, LINE_H, "Green: a one-time reward", PERK))
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
    header = ["// Generated by tools/route_pages.py: the route map pages of all five acts and Anya's scroll.",
              "// Do not edit by hand; change the script and run it (it also writes the matching layout files).",
              "#pragma once", ""]
    for act in PAGES:
        layout = page(act)
        packed = json.dumps(layout, separators=(",", ":"), ensure_ascii=False)
        (LAYOUTS / file_name(act)).write_text(json.dumps(layout, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        header.append(f'static constexpr char {layout_name(act)}[] = R"json({packed})json";')
    anya = anya_scroll()
    (LAYOUTS / "AnyaScrollhd.json").write_text(json.dumps(anya, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    header.append(f'static constexpr char AnyaScrollLayout[] = R"json({json.dumps(anya, separators=(",", ":"), ensure_ascii=False)})json";')
    # Stop names for the console note when the map opens (the map itself can't show progress).
    header.append("struct RouteNodeLabel { const char* node; const char* label; };")
    header.append("static constexpr RouteNodeLabel RouteNodeLabels[] = {")
    for act, (_, _, tiers) in PAGES.items():
        for _, stops in tiers:
            for node, label, *_ in stops:
                header.append(f'\t{{ "{node}", "{label}" }},')
    header.append("};")
    HEADER.write_text("\n".join(header) + "\n", encoding="utf-8")
    print("wrote", HEADER.relative_to(ROOT), "and", len(PAGES), "layouts")


if __name__ == "__main__":
    main()
