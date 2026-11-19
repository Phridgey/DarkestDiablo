"""Arena boss checks: the plugin waits for a boss by its monster type number, which the data
patches create. The two must agree, or an arena never clears (or clears too early).
Run: python -m unittest discover tests"""
import importlib.util
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PLUGIN = (ROOT / "src" / "plugin" / "darkestdiablo_plugin.cpp").read_text(encoding="utf-8")
DEN_PATCH = (ROOT / "src" / "patches" / "010_den_of_evil.py").read_text(encoding="utf-8")
INIFUSS_PATCH = (ROOT / "src" / "patches" / "035_inifuss_loot.py").read_text(encoding="utf-8")


class ArenaBossTest(unittest.TestCase):
    def test_corpsefire_type_matches_the_patch(self):
        plugin = int(re.search(r"CorpsefireClass\s*=\s*(\d+)", PLUGIN).group(1))
        patch = int(re.search(r"CORPSEFIRE_CLASS\s*=\s*(\d+)", DEN_PATCH).group(1))
        self.assertEqual(plugin, patch)

    def test_den_last_wave_waits_for_corpsefire(self):
        self.assert_last_wave("DenWaves", "super:40", "CorpsefireClass")

    def test_treehead_type_matches_the_patch(self):
        plugin = int(re.search(r"TreeheadClass\s*=\s*(\d+)", PLUGIN).group(1))
        patch = int(re.search(r"TREEHEAD_CLASS\s*=\s*(\d+)", INIFUSS_PATCH).group(1))
        self.assertEqual(plugin, patch)

    def test_inifuss_last_wave_waits_for_treehead(self):
        self.assert_last_wave("InifussWaves", "super:4", "TreeheadClass")

    def test_rakanishu_type_matches_and_his_wave_waits_for_him(self):
        plugin = int(re.search(r"RakanishuClass\s*=\s*(\d+)", PLUGIN).group(1))
        patch = int(re.search(r"RAKANISHU_CLASS\s*=\s*(\d+)", INIFUSS_PATCH).group(1))
        self.assertEqual(plugin, patch)
        waves = re.search(r"InifussWaves\[\] = \{(.*?)\n\};", PLUGIN, re.S).group(1)
        raka = next(line for line in waves.splitlines() if "super:3" in line)
        self.assertIn("RakanishuClass", raka)   # the arena is won only with Rakanishu dead too

    def test_mausoleum_waves_spawn_the_crafting_copies(self):
        waves = re.search(r"MausoleumWaves\[\] = \{(.*?)\n\};", PLUGIN, re.S).group(1)
        for monster in re.findall(r'"mspawn", "([^"]+)"', waves):
            self.assertTrue(monster.startswith("dd_craft_"), f"{monster} drops from the normal tables")

    def test_inifuss_waves_spawn_the_mf_copies(self):
        waves = re.search(r"InifussWaves\[\] = \{(.*?)\n\};", PLUGIN, re.S).group(1)
        for monster in re.findall(r'"mspawn", "([^"]+)"', waves):
            self.assertTrue(monster.startswith("dd_mf_"), f"{monster} drops from the normal tables")

    def assert_last_wave(self, table, superunique, boss):
        waves = re.search(table + r"\[\] = \{(.*?)\n\};", PLUGIN, re.S).group(1)
        last = [line for line in waves.splitlines() if line.strip().startswith("{")][-1]
        self.assertIn(superunique, last)
        self.assertIn(boss, last)               # the clear waits for the boss


class ArenaLevelsTest(unittest.TestCase):
    """Checks the built tables (skipped until tools/build.py has run)."""
    LEVELS = ROOT / "build" / "darkestdiablo" / "darkestdiablo.mpq" / "data" / "global" / "excel" / "levels.txt"

    def test_no_arena_is_a_quest_level(self):
        if not self.LEVELS.exists():
            self.skipTest("run python tools/build.py first")
        import csv
        with self.LEVELS.open(encoding="utf-8") as f:
            rows = {r["Id"]: r for r in csv.DictReader(f, delimiter="\t")}
        arenas = re.findall(r'\{ "[^"]+",\s*(\w+),\s*\d+,\s*\d+, \w+Waves', PLUGIN)
        ids = [{"DenOfEvilLevelId": "13"}.get(a, a) for a in arenas]
        self.assertTrue(ids)
        for level in ids:
            # Quest = 1 makes the game count the level's monsters for a quest (the Den's gave a
            # free skill point after a few kills, since arenas start empty).
            self.assertEqual(rows[level]["Quest"], "0", f"arena level {level} is a quest level")


class NewMonstersTest(unittest.TestCase):
    """Checks the built files (skipped until tools/build.py has run)."""
    BUILT = ROOT / "build" / "darkestdiablo" / "darkestdiablo.mpq" / "data"

    def test_every_new_monster_has_a_model(self):
        stats, models = self.BUILT / "global" / "excel" / "monstats.txt", self.BUILT / "hd" / "character" / "monsters.json"
        if not stats.exists() or not models.exists():
            self.skipTest("run python tools/build.py first")
        import csv
        import json
        with stats.open(encoding="utf-8") as f:
            new = [r["Id"] for r in csv.DictReader(f, delimiter="\t") if r["Id"].startswith("dd_")]
        mapping = json.loads(models.read_text(encoding="utf-8-sig"))
        self.assertTrue(new)
        for monster in new:
            # D2R finds a monster's 3D model by Id in monsters.json; without one it is invisible.
            self.assertIn(monster, mapping, f"{monster} has no model: it would be invisible")


class SpecialDropsTest(unittest.TestCase):
    """Checks the built tables (skipped until tools/build.py has run)."""
    TCS = ROOT / "build" / "darkestdiablo" / "darkestdiablo.mpq" / "data" / "global" / "excel" / "treasureclassex.txt"

    def test_guaranteed_drops_come_first(self):
        # A monster drops at most 6 items; with negative Picks entries drop in order, so a
        # guaranteed drop listed after 6 items' worth never drops (it happened to both of these).
        if not self.TCS.exists():
            self.skipTest("run python tools/build.py first")
        import csv
        with self.TCS.open(encoding="utf-8") as f:
            tcs = {r["Treasure Class"]: r for r in csv.DictReader(f, delimiter="\t")}
        for tag in ("", " (N)", " (H)"):
            self.assertEqual(tcs[f"DD MF Act 1{tag} Super B Treehead"]["Item1"], f"DD Treehead Bonus{tag}")

    def test_no_table_drops_keys(self):
        if not self.TCS.exists():
            self.skipTest("run python tools/build.py first")
        import csv
        with self.TCS.open(encoding="utf-8") as f:
            for row in csv.DictReader(f, delimiter="\t"):
                for k in range(1, 11):
                    self.assertNotEqual((row.get(f"Item{k}") or "").split(",")[0], "key", row["Treasure Class"])


def load_patch(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "src" / "patches" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class FakeTable:
    def __init__(self, rows):
        self.rows = rows


def tc(name, items, **ratios):
    row = {"Treasure Class": name, "Picks": "1", **{q: str(ratios.get(q, "")) for q in ("Unique", "Set", "Rare", "Magic")}}
    for k in range(1, 11):
        item, prob = items[k - 1] if k <= len(items) else ("", "")
        row[f"Item{k}"], row[f"Prob{k}"] = item, str(prob)
    return row


class MfLootTest(unittest.TestCase):
    def setUp(self):
        self.mod = load_patch("035_inifuss_loot")

    def test_chance_is_multiplied_by_1_4(self):
        for old in (0, 500, 800, 972):
            new = self.mod.ratio_times(old)
            self.assertAlmostEqual((1024 - old) / (1024 - new), 1.4, delta=0.03)  # chance = 1024 / (1024 - ratio)
        self.assertEqual(self.mod.ratio_times(1024), 1024)                          # 100% stays 100%

    def test_copies_drop_no_keys_and_start_from_the_highest_ratio(self):
        rows = [tc("Top", [("Junk", 2), ("Boss Items", 1)]),
                tc("Junk", [("key", 1), ("hp1", 3)]),
                tc("Boss Items", [("rin", 1)], Unique=800)]
        loot = self.mod.Loot(FakeTable(rows))
        top = loot.by_name[loot.with_mf("Top")]
        self.assertEqual(top["Unique"], str(self.mod.ratio_times(800)))   # the highest ratio below it, x1.4
        self.assertEqual(top["Magic"], str(self.mod.ratio_times(0)))
        junk = loot.by_name[top["Item1"]]
        self.assertEqual(junk["Treasure Class"], "DD NoKey Junk")
        self.assertEqual([(junk["Item1"], junk["Prob1"]), junk["Item2"]], [("hp1", "3"), ""])
        self.assertEqual(top["Item2"], "Boss Items")                        # no key below it: left alone
        self.assertNotIn("key", [r[f"Item{k}"] for r in rows for k in range(1, 11) if r["Treasure Class"].startswith("DD")])


if __name__ == "__main__":
    unittest.main()
