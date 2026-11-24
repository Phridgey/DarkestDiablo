"""Act IV (docs/act4.md), on copies of the game's own rooms with no stairs (src/maps/<name>.json).
The plugin spawns the waves (darkestdiablo_plugin.cpp: "Act IV").

- Plains of Despair (level 105, tier 15, XP grind): two of the plains' blocks in a line; Izual at
  the end. The plains are outdoor (placed anew every game), so the level becomes one fixed map at
  its own spot (OffsetX/Y, clear of the town on the same layer).
- The fork (tier 16): Act V's three red portal levels, each four lava rooms in a line (twice the
  usual length): Abaddon (XP grind), Pit of Acheron (MF run), Infernal Pit (crafting hunt).
- Hellforge (on level 107, tier 17, crafting hunt): a lava room leading to the forge, with
  Hephasto. The River of Flame (107) is not a stop, so the forge borrows its level.
- Chaos Sanctuary (level 108, tier 18): the game's own, full length, seals and all; its monsters
  are copies worth 40% more experience and triple gold that drop as an MF run's.
"""
import importlib.util
from pathlib import Path


def _load(name, file):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(file))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


act3 = _load("act3", "113_act3.py")

CRAFT_A4 = {"": ([("r27", 3), ("Runes 13", 45)], ("Flawless Gem", "Perfect Gem"), "Hpotion 5"),
            "(N)": ([("r27", 3), ("Runes 13", 45)], ("Flawless Gem", "Perfect Gem"), "Hpotion 5"),
            "(H)": ([("r27", 3), ("Runes 13", 45)], ("Flawless Gem", "Perfect Gem"), "Hpotion 5")}

PLAINS = {"id": "105", "offset": (2000, 1000), "map": "Act4/Mesa/darkest_plains.ds1", "size": ("24", "48"),
          "template": "Act 4 - Mesa 2 24X24", "name": "Act 4 - Darkest Diablo Plains",
          "waves": ["willowisp3", "vilemother1", "fingermage1", "regurgitator1", "doomknight1"], "prefix": "dd_xp_a4_",
          "pieces": ["mid24x24_0", "mid24x24_1"], "kind": "xp"}
ABADDON = {"id": "125", "map": "Act4/Expansion/darkest_abaddon.ds1", "size": ("24", "96"), "template": "Act 5 - Lava NS",
           "name": "Act 5 - Darkest Diablo Abaddon", "waves": ["bloodlord3", "imp2"], "prefix": "dd_xp_a4b_",
           "pieces": ["lavas", "lavans", "lavans2", "lavan"], "kind": "xp"}
ACHERON = {"id": "126", "map": "Act4/Expansion/darkest_acheron.ds1", "size": ("24", "96"), "template": "Act 5 - Lava NS",
           "name": "Act 5 - Darkest Diablo Pit of Acheron", "waves": ["bloodlord2", "overseer3"], "prefix": "dd_mf_a4_",
           "pieces": ["lavas2", "lavans2", "lavans", "lavan2"], "kind": "mf"}
INFERNAL = {"id": "127", "map": "Act4/Expansion/darkest_infernal.ds1", "size": ("24", "96"), "template": "Act 5 - Lava NS",
            "name": "Act 5 - Darkest Diablo Infernal Pit", "waves": ["imp4", "overseer4"], "prefix": "dd_craft_a4_",
            "pieces": ["lavas", "lavans2", "lavans", "lavan2"], "kind": "craft", "craft": CRAFT_A4, "loot_name": "DD Craft A4"}
HELLFORGE = {"id": "107", "offset": (2000, 1000), "map": "Act4/Lava/darkest_hellforge.ds1", "size": ("48", "24"),
             "template": "Act 4 - Lava Forge W", "name": "Act 4 - Darkest Diablo Hellforge",
             "waves": ["blunderbore4", "vilemother3", "fingermage2", "doomknight2"], "prefix": "dd_craft_a4b_",
             "pieces": ["lavae", "forgew"], "kind": "craft", "craft": CRAFT_A4, "loot_name": "DD Craft A4B"}

CHAOS_ID = "108"
CHAOS_PREFIX = "dd_xp_chaos_"  # "dd_xp_": 115_xp_gold.py triples their gold


def chaos(t):
    """The sanctuary's own monsters become XP + MF copies (the level's monster lists point to them)."""
    levels = t.table("levels")
    row = levels.first("Id", CHAOS_ID)
    cols = [c for c in levels.columns if c.startswith(("mon", "umon")) and c[-1].isdigit() and not c.startswith("mon ")]
    sources = sorted({row[c] for c in cols if row.get(c)})
    a = {"waves": sources, "prefix": CHAOS_PREFIX}
    act3.arenas.copies(t, a, act3.arenas.XP_BONUS)
    act3.with_mf(t, a)
    for c in cols:
        if row.get(c):
            row[c] = CHAOS_PREFIX + row[c]


def patch(t):
    for a in (PLAINS, ABADDON, ACHERON, INFERNAL, HELLFORGE):
        act3.stop(t, a)
    chaos(t)
