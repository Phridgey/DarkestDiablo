"""The caravan stash's panels (src/plugin/stash.h): valid JSON, unique names, everything inside its
panel, every button known to stash.cpp. Needs the built camp_tests.exe.

Usage: python tests/test_stash_layouts.py
"""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "build" / "plugin" / "camp_tests.exe"
ACTIONS = ("tree", "upgrades", "shared", "closebank", "deposit", "pay-", "node-", "cut-", "slot-")
PREFIX = "PanelManager:OpenPanel:darkestdiablo/stash-"


def check(name, panel):
    size = panel["fields"]["rect"]
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
        if message is not None:
            assert message.startswith(PREFIX), (name, message)
            action = message[len(PREFIX):]
            assert any(action == a or (a.endswith("-") and action.startswith(a)) for a in ACTIONS), (name, action)


def main():
    if not EXE.exists():
        sys.exit("build/plugin/camp_tests.exe is missing: run tools\\build-plugin.cmd first.")
    with tempfile.TemporaryDirectory(dir=ROOT / "build") as folder:
        subprocess.run([str(EXE), "layouts", folder], check=True)
        files = sorted(Path(folder).glob("*.json"))
        assert len(files) == 6
        for file in files:
            check(file.stem, json.loads(file.read_text(encoding="utf-8")))
    print("stash layouts passed")


if __name__ == "__main__":
    main()
