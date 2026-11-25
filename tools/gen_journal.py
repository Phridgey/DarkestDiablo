"""Writes build/plugin-gen/journaldata.h: the stash journal's runes and runewords (src/plugin/journal.h).

Reads the LoD tables and the English texts from vanilla/ (your own game copy), with this mod's rune
level requirements (src/patches/008_act1_rune_levels.py). tools/build-plugin.cmd runs it before every
build; the output is in build/, never committed (it comes from Blizzard's tables). It stops on a stat
it has no words for, so a wrong tooltip can't slip through.
"""
import csv
import importlib.util
import json
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASE = ROOT / "vanilla" / "data" / "global" / "excel" / "base"
TEXTS = ROOT / "vanilla" / "data" / "local" / "lng" / "strings"
OUT = ROOT / "build" / "plugin-gen" / "journaldata.h"

RUNES = ["El", "Eld", "Tir", "Nef", "Eth", "Ith", "Tal", "Ral", "Ort", "Thul", "Amn", "Sol", "Shael", "Dol", "Hel", "Io", "Lum",
         "Ko", "Fal", "Lem", "Pul", "Um", "Mal", "Ist", "Gul", "Vex", "Ohm", "Lo", "Sur", "Ber", "Jah", "Cham", "Zod"]
# Item type -> (name, which of a rune's three effects counts).
TYPES = {"tors": ("Body Armor", "helm"), "helm": ("Helm", "helm"), "shld": ("Shield", "shield"), "pala": ("Paladin Shield", "shield"),
         "weap": ("Weapon", "weapon"), "mele": ("Melee Weapon", "weapon"), "miss": ("Missile", "weapon"), "swor": ("Sword", "weapon"),
         "axe": ("Axe", "weapon"), "mace": ("Mace", "weapon"), "hamm": ("Hammer", "weapon"), "club": ("Club", "weapon"),
         "scep": ("Scepter", "weapon"), "wand": ("Wand", "weapon"), "staf": ("Staff", "weapon"), "pole": ("Polearm", "weapon"),
         "spea": ("Spear", "weapon"), "knif": ("Dagger", "weapon"), "h2h": ("Claw", "weapon")}
SLOT = {"weapon": "weapon", "helm": "armor or helm", "shield": "shield"}
TABS = ["Bow and Crossbow", "Passive and Magic", "Javelin and Spear", "Fire", "Lightning", "Cold", "Curses", "Poison and Bone",
        "Necromancer Summoning", "Paladin Combat", "Offensive Auras", "Defensive Auras", "Barbarian Combat", "Combat Masteries", "Warcries",
        "Druid Summoning", "Shape Shifting", "Elemental", "Traps", "Shadow Disciplines", "Martial Arts"]
CLASSES = {"ama": "Amazon", "sor": "Sorceress", "nec": "Necromancer", "pal": "Paladin", "bar": "Barbarian", "dru": "Druid", "ass": "Assassin"}

# {n}: the amount ("5" or "5-10"). {d}: the same with a leading + or -. {l}: the amount per character level.
PLAIN = {
    "abs-cold%": "{n}% Cold Absorb", "abs-fire%": "{n}% Fire Absorb", "abs-ltng%": "{n}% Lightning Absorb", "abs-fire": "+{n} Fire Absorb",
    "abs-mag": "+{n} Magic Absorb", "ac": "{d} Defense", "ac%": "{d}% Enhanced Defense", "ac-miss": "{d} Defense vs. Missile",
    "ac/lvl": "+{l} Defense per Character Level", "all-stats": "{d} to all Attributes", "allskills": "{d} to All Skills",
    "att": "{d} to Attack Rating", "att%": "{n}% Bonus to Attack Rating", "att-demon": "{d} to Attack Rating against Demons",
    "att-undead": "{d} to Attack Rating against Undead", "balance2": "{d}% Faster Hit Recovery", "balance3": "{d}% Faster Hit Recovery",
    "block": "{n}% Increased Chance of Blocking", "block2": "{d}% Faster Block Rate", "cast2": "{d}% Faster Cast Rate", "cast3": "{d}% Faster Cast Rate",
    "charge-noconsume": "{d}% Chance for Finishing Moves to not Consume Charges", "cheap": "Reduces all Vendor Prices {n}%",
    "crush": "{n}% Chance of Crushing Blow", "deadly": "{n}% Deadly Strike", "deadly/lvl": "+{l}% Deadly Strike per Character Level",
    "demon-heal": "{d} Life after each Demon Kill", "dex": "{d} to Dexterity", "dmg": "{d} Damage", "dmg%": "{d}% Enhanced Damage",
    "dmg-ac": "{d} to Monster Defense per Hit", "dmg-dem/lvl": "+{l}% Damage to Demons per Character Level", "dmg-demon": "{d}% Damage to Demons",
    "dmg-max": "{d} to Maximum Damage", "dmg-min": "{d} to Minimum Damage", "dmg-to-mana": "{n}% Damage Taken Goes To Mana",
    "dmg-undead": "{d}% Damage to Undead", "ease": "Requirements {d}%", "enr": "{d} to Energy", "ethereal": "Ethereal",
    "explosivearrow": "Fires Explosive Arrows or Bolts (Level {n})", "extra-cold": "{d}% to Cold Skill Damage", "extra-fire": "{d}% to Fire Skill Damage",
    "extra-ltng": "{d}% to Lightning Skill Damage", "extra-pois": "{d}% to Poison Skill Damage", "fireskill": "{d} to Fire Skills",
    "freeze": "Freezes Target +{n}", "gold%": "{d}% Gold from Monsters", "gold%/lvl": "+{l}% Gold from Monsters per Character Level",
    "half-freeze": "Half Freeze Duration", "heal-kill": "{d} Life after each Kill", "hp": "{d} to Life", "hp%": "Increase Maximum Life {n}%",
    "hp/lvl": "+{l} to Life per Character Level", "ignore-ac": "Ignore Target's Defense", "indestruct": "Indestructible", "knock": "Knockback",
    "lifesteal": "{n}% Life Stolen per Hit", "light": "{d} to Light Radius", "mag%": "{d}% Better Chance of Getting Magic Items",
    "mag%/lvl": "+{l}% Better Chance of Getting Magic Items per Character Level", "mana": "{d} to Mana", "mana%": "Increase Maximum Mana {n}%",
    "mana-kill": "{d} to Mana after each Kill", "mana/lvl": "+{l} to Mana per Character Level", "manasteal": "{n}% Mana Stolen per Hit",
    "move2": "{d}% Faster Run/Walk", "move3": "{d}% Faster Run/Walk", "nofreeze": "Cannot Be Frozen", "noheal": "Prevent Monster Heal",
    "openwounds": "{n}% Chance of Open Wounds", "pierce": "{n}% Piercing Attack", "pierce-cold": "-{n}% to Enemy Cold Resistance",
    "pierce-fire": "-{n}% to Enemy Fire Resistance", "pierce-ltng": "-{n}% to Enemy Lightning Resistance", "pierce-pois": "-{n}% to Enemy Poison Resistance",
    "reanimate": "{n}% Reanimate as: Returned", "red-dmg": "Damage Reduced by {n}", "red-dmg%": "Damage Reduced by {n}%",
    "red-mag": "Magic Damage Reduced by {n}", "reduce-ac": "-{n}% Target Defense", "regen": "Replenish Life {d}", "regen-mana": "Regenerate Mana {n}%",
    "res-all": "All Resistances {d}", "res-cold": "Cold Resist {d}%", "res-fire": "Fire Resist {d}%", "res-ltng": "Lightning Resist {d}%",
    "res-pois": "Poison Resist {d}%", "res-cold-max": "{d}% to Maximum Cold Resist", "res-fire-max": "{d}% to Maximum Fire Resist",
    "res-ltng-max": "{d}% to Maximum Lightning Resist", "res-pois-max": "{d}% to Maximum Poison Resist",
    "res-pois-len": "Poison Length Reduced by {n}%", "rip": "Slain Monsters Rest in Peace", "slow": "Slows Target by {n}%",
    "stam": "{d} Maximum Stamina", "stamdrain": "{n}% Slower Stamina Drain", "str": "{d} to Strength", "str/lvl": "+{l} to Strength per Character Level",
    "stupidity": "Hit Blinds Target +{n}", "swing1": "{d}% Increased Attack Speed", "swing2": "{d}% Increased Attack Speed",
    "swing3": "{d}% Increased Attack Speed", "thorns": "Attacker Takes Damage of {n}", "vit": "{d} to Vitality", "vit/lvl": "+{l} to Vitality per Character Level",
    "dmg-cold": "Adds {n} Cold Damage", "dmg-fire": "Adds {n} Fire Damage", "dmg-ltng": "Adds {n} Lightning Damage", "dmg-mag": "Adds {n} Magic Damage",
    "dmg-elem": "Adds {n} Fire, Lightning and Cold Damage",
}
CASTS = {"att-skill": "on attack", "hit-skill": "on striking", "gethit-skill": "when struck", "kill-skill": "when you Kill an Enemy",
         "death-skill": "when you Die", "levelup-skill": "when you Level-Up"}


def table(name):
    with open(BASE / name, encoding="utf-8-sig", errors="replace", newline="") as f:
        return list(csv.DictReader(f, delimiter="\t"))


class Words:
    def __init__(self):
        self.text = {}
        for name in ("item-runes.json", "skills.json"):
            for entry in json.loads((TEXTS / name).read_text(encoding="utf-8-sig")):
                self.text[entry["Key"]] = entry["enUS"]
        names = {row["skilldesc"]: row["str name"] for row in table("skilldesc.txt")}
        self.skills = {}
        for row in table("skills.txt"):
            shown = self.text.get(names.get(row["skilldesc"], ""), row["skill"])
            for key in (row["skill"].lower(), row["*Id"]):
                self.skills.setdefault(key, (shown, row["charclass"]))

    def stat(self, code, param, low, high):
        lo, hi = int(low or 0), int(high or 0)
        n = str(lo) if lo == hi else f"{lo}-{hi}"
        d = n if lo < 0 else "+" + n
        if code in PLAIN:
            per = [int(param) / 8] if param else [lo / 8, hi / 8]
            return PLAIN[code].format(n=n, d=d, l="-".join(f"{v:g}" for v in dict.fromkeys(per)))
        if code in CASTS:
            return f"{lo}% Chance to cast Level {hi} {self.skill(param)} {CASTS[code]}"
        if code in CLASSES:
            return f"{d} to {CLASSES[code]} Skill Levels"
        if code == "aura":
            return f"Level {n} {self.skill(param)} Aura When Equipped"
        if code == "charged":
            return f"Level {hi} {self.skill(param)} ({lo} Charges)"
        if code == "oskill":
            return f"{d} to {self.skill(param)}"
        if code == "skill":
            return f"{d} to {self.skill(param)} ({CLASSES[self.skills[param.lower()][1]]} Only)"
        if code == "skilltab":
            return f"{d} to {TABS[int(param)]} Skills"
        if code == "dmg-pois":
            return f"+{lo * int(param) // 256} Poison Damage over {int(param) // 25} Seconds"
        if code == "howl":
            return f"Hit Causes Monster to Flee {lo * 100 // 128}%"
        if code == "rep-dur":
            return f"Repairs 1 Durability in {100 // int(param)} Seconds"
        raise SystemExit(f"gen_journal: no words for stat '{code}'")

    def skill(self, param):
        return self.skills[param.lower()][0]


def cpp(text):
    """Text that ends up inside a JSON string inside a C++ string."""
    assert '"' not in text and "\\" not in text and "|" not in text, text
    return '"' + text.replace("\n", "\\\\n").replace("ÿ", "\\\\u00ff") + '"'


BLUE, GOLD, GREY, WHITE = "ÿc3", "ÿc4", "ÿc5", "ÿc0"


def main():
    words = Words()
    spec = importlib.util.spec_from_file_location("levels", ROOT / "src" / "patches" / "008_act1_rune_levels.py")
    patch = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(patch)
    levels = {row["code"]: int(row["levelreq"] or 0) for row in table("misc.txt")}
    levels.update(patch.LEVEL_REQ)
    codes = [f"r{i:02d}" for i in range(1, 34)]
    effects = {}  # rune -> weapon/helm/shield -> lines
    for row in table("gems.txt"):
        if row["code"] in codes:
            effects[row["code"]] = {part: [words.stat(row[f"{part}Mod{i}Code"], row[f"{part}Mod{i}Param"], row[f"{part}Mod{i}Min"], row[f"{part}Mod{i}Max"])
                                           for i in (1, 2, 3) if row[f"{part}Mod{i}Code"]] for part in ("weapon", "helm", "shield")}
    out = []
    for row in table("runes.txt"):
        if row["complete"] != "1":
            continue
        runes = [row[f"Rune{i}"] for i in range(1, 7) if row[f"Rune{i}"]]
        types = [row[f"itype{i}"] for i in range(1, 7) if row[f"itype{i}"]]
        name = words.text[row["Name"]]
        level = max(levels[r] for r in runes)
        kinds = ", ".join(TYPES[t][0] for t in types)
        lines = [GOLD + name, GREY + "'" + "".join(RUNES[codes.index(r)] for r in runes) + "'",
                 WHITE + f"{len(runes)} sockets: {kinds}", WHITE + f"Required Level: {level}"]
        lines += [BLUE + words.stat(row[f"T1Code{i}"], row[f"T1Param{i}"], row[f"T1Min{i}"], row[f"T1Max{i}"]) for i in range(1, 8) if row[f"T1Code{i}"]]
        for part in dict.fromkeys(TYPES[t][1] for t in types):
            lines.append(GREY + f"From the runes ({SLOT[part]}):")
            lines += [BLUE + text for r in runes for text in effects[r][part]]
        out.append((level, name, kinds, [codes.index(r) for r in runes], "\n".join(lines)))
    out.sort(key=lambda w: (w[0], w[1]))
    text = ["// Made by tools/gen_journal.py from your own game's tables. Not committed.", "#pragma once", "namespace Journal {",
            "struct RuneData { const char* name; int level; const char* weapon; const char* armor; const char* shield; };",
            "struct WordData { int level; const char* name; const char* types; int count; int runes[6]; const char* tip; };",
            "inline constexpr RuneData Runes[] = {"]
    for code, name in zip(codes, RUNES):
        parts = [cpp(", ".join(effects[code][p]).replace("Attack Rating against", "AR vs.").replace("Damage to", "Damage vs.")) for p in ("weapon", "helm", "shield")]
        text.append(f'\t{{ "{name}", {levels[code]}, {", ".join(parts)} }},')
    text += ["};", "// In order of level.", "inline constexpr WordData Words[] = {"]
    for level, name, kinds, runes, tip in out:
        text.append(f'\t{{ {level}, {cpp(name)}, {cpp(kinds)}, {len(runes)}, {{ {", ".join(map(str, runes))} }}, {cpp(tip)} }},')
    text += ["};", "}", ""]
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(text), encoding="utf-8")
    print(f"journaldata.h: {len(out)} runewords")


if __name__ == "__main__":
    main()
