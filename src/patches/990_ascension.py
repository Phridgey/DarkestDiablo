"""Ascend (docs/ascend.md): the arenas, the Ascension bosses and their loot. The plugin
(src/plugin/ascension.cpp) does the rest.

- Arenas: the five act bosses' rooms without monsters, on spare levels (src/maps/ascend*.json).
- Bosses: copies of the act bosses ("dd_ascend<act>_<k>"), each worn by superunique rows so the game
  tints them and gives them elite affixes. A superunique takes 3 affixes; the 4th is built into the copy's
  own numbers and shown in its name. The plugin picks one row at random.
- The exact life and speed bonuses are written by the plugin once the boss stands
  ("ascendhp" lines); movement and damage are in the copy.
- Loot: a monster drops at most 6 items, so the boss drops 3 sets or uniques and 3 more of its
  own; the rest comes from the "spoils" (dd_ascendloot<act>_g/_j).

Runs after the monster levels of the route (004) and before the superunique copies and class list.
"""
import importlib.util
import random
from pathlib import Path


def _load(name, file):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(file))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


act3 = _load("act3", "113_act3.py")
inifuss = _load("inifuss_loot", "035_inifuss_loot.py")

HP, DAMAGE, MOVE = 1.40, 1.35, 1.10
UNIQUE_HP = 4        # monumod.txt: a unique's life in Normal is +300%; the plugin corrects it if not

# level id, size, template preset, piece, look (level), own offset, boss row
ARENAS = {
    1: {"id": "36",  "size": ("22", "31"), "template": "Act 1 - Andariel", "pieces": ["andy3"], "look": "37",
        "map": "Act1/Catacomb/darkest_ascend1.ds1", "boss": "andariel", "title": "Andariel"},
    2: {"id": "56",  "size": ("32", "47"), "template": "Act 2 - Duriel's Lair", "pieces": ["duriel"], "look": "73",
        "map": "Act2/Tomb/darkest_ascend2.ds1", "boss": "duriel", "title": "Duriel"},
    3: {"id": "101", "size": ("41", "29"), "template": "Act 3 - Mephisto Complex", "pieces": ["mephcomp"], "look": "102",
        "map": "Act3/Travincal/darkest_ascend3.ds1", "boss": "mephisto", "title": "Mephisto"},
    4: {"id": "106", "size": ("24", "24"), "template": "Act 4 - Diablo Heart", "pieces": ["heart"], "look": "108",
        "offset": (2500, 1000), "map": "Act4/Diab/darkest_ascend4.ds1", "boss": "diablo", "title": "Diablo"},
    5: {"id": "130", "size": ("55", "55"), "template": "Act 5 - WorldStone", "pieces": ["wstone01"], "look": "132",
        "map": "Expansion/baalLair/darkest_ascend5.ds1", "boss": "baalcrab", "title": "Baal"},
}
TINT = {1: "14", 2: "9", 3: "19", 4: "21", 5: "12"}  # superuniques.txt Utrans
# Affixes the game gives (monumod.txt ids): extra strong, cursed, mana burn, spectral hit, aura.
# The boss must never be immune to anything (decided 2026-11-24): fire, lightning and cold
# enchanted (9, 17, 18) add 75 to their resistance, so they stay out; check_immunity() makes sure.
MODS = [5, 7, 25, 27, 30]
MOD_RESIST = 20      # the most any of MODS adds to a resistance (spectral hit: elements; mana burn: magic)
# The 4th affix, built into the copy's numbers, so it must not repeat one of the three.
BUILT_IN = ["Stone Skin", "Magic Resistant", "Extra Fast", "Berserker"]
COMBOS = 6           # superunique rows per built-in affix
# Item levels and base items: act-appropriate bases, affixes from an act further.
TARGET = {1: 18, 2: 36, 3: 54, 4: 72, 5: 90}
STRING_ID = 91400


def num(row, col):
    v = row.get(col, "").strip()
    return int(v) if v.lstrip("-").isdigit() else None


def scale(row, cols, factor):
    for c in cols:
        v = num(row, c)
        if v:
            row[c] = str(max(1, round(v * factor)))


def check_immunity(row):
    for res in ("ResDm", "ResMa", "ResFi", "ResLi", "ResCo", "ResPo"):
        worst = (num(row, res) or 0) + (0 if res in ("ResDm", "ResPo") else MOD_RESIST)
        if worst >= 100:
            raise RuntimeError(f"{row['Id']}: {res} could reach {worst}, an immunity")


def patch(t):
    rng = random.Random(20261124)  # the same table every build
    stats, supers, models = t.table("monstats"), t.table("superuniques"), t.data("hd/character/monsters.json")
    loot = inifuss.Loot(t.table("treasureclassex"))
    names = []
    for act, a in ARENAS.items():
        act3.stop(t, {**a, "name": f"Act {act} - Darkest Diablo Ascension", "waves": [], "prefix": "dd_none_"})
        source = stats.first("Id", a["boss"])
        tc = boss_loot(t, loot, act, source)
        life = {}
        for k, affix in enumerate(BUILT_IN):
            cls = len([r for r in stats.rows if r.get("*hcIdx", "").strip().isdigit()])
            boss_id = f"dd_ascend{act}_{k}"
            row = stats.add_copy("Id", a["boss"], Id=boss_id, NextInClass="", minion1="", minion2="", MinGrp="1", MaxGrp="1",
                                 TCQuestId="", TCQuestCP="", TreasureClassQuest="", **{"*hcIdx": str(cls)})
            models[boss_id] = models[a["boss"]]
            row["Level"] = str(max(1, num(source, "Level") - 3))  # a superunique adds 3
            for tag in ("", "(N)", "(H)"):
                scale(row, [f"{p}{e}{tag}" for p in ("A1", "A2", "S1") for e in ("MinD", "MaxD")], DAMAGE)
                scale(row, [f"El{n}{e}{tag}" for n in (1, 2, 3) for e in ("MinD", "MaxD")], DAMAGE)
                scale(row, [f"minHP{tag}", f"maxHP{tag}"], HP / UNIQUE_HP)
                if affix == "Stone Skin":
                    row["ResDm" + tag] = str(min(75, (num(row, "ResDm" + tag) or 0) + 50))
                    scale(row, ["AC" + tag], 1.5)
                elif affix == "Magic Resistant":
                    for res in ("ResFi", "ResLi", "ResCo"):
                        row[res + tag] = str(min(75, (num(row, res + tag) or 0) + 40))
                elif affix == "Berserker":
                    scale(row, [f"{p}TH{tag}" for p in ("A1", "A2", "S1")], 1.5)
                    row["Crit" + tag] = str((num(row, "Crit" + tag) or 0) + 25)
            check_immunity(row)
            scale(row, [f"Sk{n}lvl" for n in range(1, 9)], DAMAGE)  # spells grow by skill level
            scale(row, ["Velocity", "Run"], MOVE * (1.25 if affix == "Extra Fast" else 1.0))
            t.loot_lines.append(f"ascendclass {act} {cls}")
            combos = set()
            while len(combos) < COMBOS:
                combos.add(tuple(sorted(rng.sample(MODS, 3))))
            for c, mods in enumerate(sorted(combos)):
                hc = max(int(r["hcIdx"]) for r in supers.rows if r.get("hcIdx", "").strip().isdigit()) + 1
                key = f"ddascend{act}_{k}"
                supers.add_copy("Superunique", "Griswold", Superunique=f"Ascended {a['title']} {k}{c}", Name=key, Class=boss_id, hcIdx=str(hc),
                                Mod1=str(mods[0]), Mod2=str(mods[1]), Mod3=str(mods[2]), MinGrp="0", MaxGrp="0",
                                **{"Utrans": TINT[act], "Utrans(N)": TINT[act], "Utrans(H)": TINT[act], "MonSound": "",
                                   "TC": tc, "TC(N)": tc, "TC(H)": tc,
                                   "TC Desecrated": tc, "TC(N) Desecrated": tc, "TC(H) Desecrated": tc})
                t.loot_lines.append(f"ascendsuper {act} {hc}")
            names.append((f"ddascend{act}_{k}", f"Ascended {a['title']} ({affix})"))
        # A monster's life is a share (minHP, percent) of its level's life in monlvl.txt.
        level_life = int(t.table("monlvl").first("Level", source["Level"])["HP"])
        t.loot_lines.append(f"ascendhp {act} {round(level_life * num(source, 'minHP') / 100 * HP)}")
        spoils(t, loot, act, stats, models)
    if t.excel.name != "excel":
        return
    strings = t.data("local/lng/strings/monsters.json")
    langs = [k for k in strings[0] if k not in ("id", "Key")]
    for n, (key, text) in enumerate(names):
        if not any(e.get("Key") == key for e in strings):
            strings.append({"id": STRING_ID + n, "Key": key, **{lang: text for lang in langs}})


def boss_loot(t, loot, act, source):
    """3 guaranteed sets or uniques of the act, then 3 picks of the boss's own loot."""
    bonus = loot.code_tree(f"DD Ascend U{act}", inifuss.bonus_bases(t, TARGET[act]), {q: "1024" for q in inifuss.QUALITIES})
    row = {k: "" for k in loot.tcs.rows[0] if k != "__width__"}
    name = f"DD Ascend {act}"
    row.update({"Treasure Class": name, "Picks": "-6", "NoDrop": "0", "Item1": bonus, "Prob1": "3",
                "Item2": source["TreasureClass"], "Prob2": "3"})
    loot.tcs.rows.append(row)
    loot.by_name[name] = row
    return name


def spoils(t, loot, act, stats, models):
    """Two helpless creatures the plugin spawns and kills after the boss: one drops rare gear made for the
    act's end, the other rare rings, amulets and jewels and magic charms. Items take the level of
    what drops them, so their level gives affixes of the next act."""
    top = TARGET[act]
    gear = sorted({r["code"] for table in ("weapons", "armor") for r in t.table(table).rows
                   if r.get("spawnable") == "1" and r.get("quest", "") in ("", "0") and r.get("stackable", "") != "1"
                   and len(r.get("code", "")) == 3 and num(r, "level") is not None and top - 14 <= num(r, "level") <= top})
    blank = {k: "" for k in loot.tcs.rows[0] if k != "__width__"}
    rare = {"Rare": "1024", "Magic": "1024"}
    for kind, picks, tree in (("g", 5, loot.code_tree(f"DD Ascend G{act}", gear, rare)),
                              ("j", 6, loot.code_tree(f"DD Ascend J{act}", ["rin", "amu", "jew", "cm1", "cm2", "cm3"], rare))):
        name = f"DD Ascend S{act}{kind}"
        row = dict(blank)
        row.update({"Treasure Class": name, "Picks": str(picks), "NoDrop": "0", "Item1": tree, "Prob1": "1", **rare})
        loot.tcs.rows.append(row)
        loot.by_name[name] = row
        cls = len([r for r in stats.rows if r.get("*hcIdx", "").strip().isdigit()])
        mid = f"dd_ascendloot{act}_{kind}"
        m = stats.add_copy("Id", "fallen1", Id=mid, NextInClass="", minion1="", minion2="", AI="Idle", MinGrp="1", MaxGrp="1",
                           Velocity="0", Run="0", Level=str(min(99, top + 18)), **{"*hcIdx": str(cls)})
        for col in list(m):
            if col.startswith("TreasureClass"):
                m[col] = name
            elif col.startswith(("minHP", "maxHP")):
                m[col] = "1"
            elif col.startswith("Exp"):
                m[col] = "0"
        models[mid] = models["fallen1"]
