"""Every magic find run gives at least two set or unique items (decided 2026-11-23).

- Tree of Inifuss: Rakanishu and Treehead each drop one (035_inifuss_loot.py).
- Pit: its target now drops two (090_pit.py).
- Ancient Tunnels, Swampy Pit, Pit of Acheron (cleared by killing one unique, picked at random):
  as in the Pit, each unique-led wave type gets a target copy "<type>_t" whose unique drops two
  guaranteed sets or uniques; the plugin spawns the picked unique as that type ("targetclass").
- Pindleskin, Nihlathak (Halls of Vaught), Frozenstein (Frozen River, the Anya stop): two
  guaranteed sets or uniques first in their own loot.
The pick is a unique where the base has one, else a set, at or below the stop's level
(035_inifuss_loot.py, bonus_bases). Runs after the stops' patches and before the monster levels
(996), the superunique copies (998) and the class list (999), so those include these rows.
"""
import importlib.util
from pathlib import Path

_spec = importlib.util.spec_from_file_location("inifuss_loot", Path(__file__).with_name("035_inifuss_loot.py"))
inifuss = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(inifuss)

PICKS = 2
# level id: its unique-led wave types (the plugin's wave tables)
RANDOM_TARGET = {
    "65": ["dd_mf_a2_zombie5", "dd_mf_a2_sandraider3", "dd_mf_a2_mummy3", "dd_mf_a2_skmage_ltng4"],
    "90": [],   # filled from the types the stop's patch made (prefix dd_mf_a3_)
    "126": [],  # (prefix dd_mf_a4_)
}
PREFIX = {"90": "dd_mf_a3_", "126": "dd_mf_a4_"}
BOSSES = {"Pindleskin": "121", "Nihlathak Boss": "124", "Frozenstein": "114"}
COLS = (("", "MonLvlEx", ""), ("(N)", "MonLvlEx(N)", " (N)"), ("(H)", "MonLvlEx(H)", " (H)"))


def patch(t):
    stats = t.table("monstats")
    loot = inifuss.Loot(t.table("treasureclassex"))
    levels = t.table("levels")
    models = t.data("hd/character/monsters.json")
    blank = {k: "" for k in loot.tcs.rows[0] if k != "__width__"}
    bonus = {}

    def bonus_for(level_id, suffix, lvl_col, tag):
        key = (level_id, suffix)
        if key not in bonus:
            item_level = int(levels.first("Id", level_id)[lvl_col])
            bonus[key] = loot.code_tree(f"DD MF2 {level_id}{tag}", inifuss.bonus_bases(t, item_level),
                                        {q: "1024" for q in inifuss.QUALITIES})
        return bonus[key]

    def with_bonus(base, name, level_id, suffix, lvl_col, tag):
        """A loot table that drops PICKS guaranteed sets or uniques first, then `base`."""
        row = dict(blank)
        row.update({"Treasure Class": name, "Picks": str(-(PICKS + 1)), "NoDrop": "0",
                    "Item1": bonus_for(level_id, suffix, lvl_col, tag), "Prob1": str(PICKS), "Item2": base, "Prob2": "1"})
        loot.tcs.rows.append(row)
        loot.by_name[name] = row
        return name

    for level_id, types in RANDOM_TARGET.items():
        if level_id in PREFIX:
            types = sorted({r["Id"] for r in stats.rows if r.get("Id", "").startswith(PREFIX[level_id]) and not r["Id"].endswith("_t")})
        for source in types:
            numbered = len([r for r in stats.rows if r.get("*hcIdx", "").strip().isdigit()])
            row = stats.add_copy("Id", source, Id=source + "_t", NextInClass="", minion1="", **{"*hcIdx": str(numbered)})
            for suffix, lvl_col, tag in COLS:
                col = "TreasureClassUnique" + suffix
                if row.get(col):
                    row[col] = with_bonus(row[col], f"DD MF2 {source}{tag}", level_id, suffix, lvl_col, tag)
            t.loot_lines.append(f"targetclass {level_id} {numbered}")
            models[source + "_t"] = models[source]
    supers = t.table("superuniques")
    for name, level_id in BOSSES.items():
        boss = supers.first("Superunique", name)
        for col, (suffix, lvl_col, tag) in zip(("TC", "TC(N)", "TC(H)"), COLS):
            if boss.get(col):
                boss[col] = with_bonus(boss[col], f"DD MF2 {name}{tag}", level_id, suffix, lvl_col, tag)
