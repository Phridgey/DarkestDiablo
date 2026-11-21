"""The route map layout exists twice: built into the plugin and as a mod file. The game uses the
plugin's copy, so a change made to only one of them silently does nothing (the Den button once
sent players to the real Den of Evil this way). Run: python -m unittest discover tests"""
import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PLUGIN = ROOT / "src" / "plugin" / "darkestdiablo_plugin.cpp"
LAYOUTS = ROOT / "src" / "data" / "data" / "global" / "ui" / "layouts" / "darkestdiablo"
LAYOUT = LAYOUTS / "RouteMaphd.json"
LAYOUT2 = LAYOUTS / "RouteMap2hd.json"
ARENA_PATCHES = ROOT / "src" / "patches"


def builtin_layout(name="RouteMapLayout"):
    src = PLUGIN.read_text(encoding="utf-8")
    return json.loads(re.search(name + r'\[\] = R"json\((.*?)\)json"', src, re.S).group(1))


def travel_levels(layout):
    """{button name: level id} for every button that travels."""
    out = {}
    for child in layout["children"]:
        m = re.search(r"darkestdiablo/travel-(\d+)$", child["fields"].get("onClickMessage", ""))
        if m:
            out[child["name"]] = int(m.group(1))
    return out


class RouteMapTest(unittest.TestCase):
    def test_both_copies_match(self):
        self.assertEqual(builtin_layout(), json.loads(LAYOUT.read_text(encoding="utf-8")),
                         "The plugin's built-in route map differs from RouteMaphd.json; change both.")

    def test_act2_copies_match(self):
        self.assertEqual(builtin_layout("RouteMap2Layout"), json.loads(LAYOUT2.read_text(encoding="utf-8")),
                         "The plugin's built-in Act II route map differs from RouteMap2hd.json; change both.")

    def test_every_built_stop_has_a_button(self):
        route = (ROOT / "src" / "plugin" / "route.h").read_text(encoding="utf-8")
        stops = re.findall(r'\{ "(\w+)",\s*"(\w+)",\s*(\d+),\s*(-?\d+),\s*(-?\d+) \}', route)
        buttons = {**travel_levels(builtin_layout()), **travel_levels(builtin_layout("RouteMap2Layout"))}
        for stop, node, tier, travel, arena in stops:
            if int(travel) >= 0:
                self.assertEqual(buttons.get(node), int(travel), f"{stop}: button {node} must travel to {travel}")

    def test_den_goes_to_the_arena(self):
        # The Den arena is a spare cave level (010_den_of_evil.py), never the real Den (level 8).
        arena = re.search(r'ARENA_ID\s*=\s*"(\d+)"', (ARENA_PATCHES / "010_den_of_evil.py").read_text(encoding="utf-8"))
        self.assertEqual(travel_levels(builtin_layout())["NodeDen"], int(arena.group(1)))


if __name__ == "__main__":
    unittest.main()
