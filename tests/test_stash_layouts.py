"""The caravan stash's panels (src/plugin/stash.h): valid JSON, unique names, everything inside its
panel, every button known to stash.cpp. Needs the built camp_tests.exe.

Usage: python tests/test_stash_layouts.py
"""
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "build" / "plugin" / "camp_tests.exe"
ACTIONS = ("tree", "upgrades", "shared", "closebank", "deposit", "pay-", "node-", "cut-", "slot-",
           "goldin", "goldout", "goldless", "goldmore", "journal", "jrune-", "jpage-", "jnone")
PREFIX = "PanelManager:OpenPanel:darkestdiablo/stash-"
POTION_PREFIX = "PanelManager:OpenPanel:darkestdiablo/potion-"
POTION_ACTIONS = ("use-", "pick-", "equip-")


def check(name, panel):
    size = panel["fields"]["rect"]
    # The loader rejects the whole panel if a game text has no namespace ("@strClose").
    for text in re.findall(r'"(@[^"]*)"', json.dumps(panel)):
        assert text.startswith(("@d2r:", "@darkestdiablo:", "@PANEL@")), (name, text)
    names = [c["name"] for c in panel["children"]]
    assert len(names) == len(set(names)), f"{name}: a name is used twice"
    for child in panel["children"]:
        fields = child["fields"]
        rect = fields.get("rect")
        if isinstance(rect, dict):
            scale = rect.get("scale", 1)
            w, h = rect.get("width", 0), rect.get("height", 0)
            if "FrontEnd_ButtonSmall" in fields.get("filename", ""):
                w, h = 366 * scale, 93 * scale
            assert rect["x"] >= 0 and rect["y"] >= 0 and w >= 0 and h >= 0, (name, child["name"], rect)
            assert rect["x"] + w <= size["width"] and rect["y"] + h <= size["height"] + 1, (name, child["name"], rect)
        message = fields.get("onClickMessage")
        if message is not None and name.startswith("hud_"):
            assert message.startswith(POTION_PREFIX) and message[len(POTION_PREFIX):].startswith(POTION_ACTIONS), (name, message)
        elif message is not None:
            assert message.startswith(PREFIX), (name, message)
            action = message[len(PREFIX):]
            assert any(action == a or (a.endswith("-") and action.startswith(a)) for a in ACTIONS), (name, action)


def main():
    if not EXE.exists():
        sys.exit("build/plugin/camp_tests.exe is missing: run tools\\build-plugin.cmd first.")
    with tempfile.TemporaryDirectory(dir=ROOT / "build") as folder:
        subprocess.run([str(EXE), "layouts", folder], check=True)
        files = sorted(Path(folder).glob("*.json"))
        assert len(files) >= 39
        for file in files:
            panel = json.loads(file.read_text(encoding="utf-8"))
            if file.stem.startswith("route"):  # a route map page with potion affixes drawn in
                names = {c["name"]: c for c in panel["children"]}
                assert "Affix_antidote" in names and "Affix_stamina" in names
                assert names["AffixLine"]["fields"]["text"] == "Potions: Antidote at Crypt   Stamina at pit"
            else:
                check(file.stem, panel)
    print("stash layouts passed")


if __name__ == "__main__":
    main()
