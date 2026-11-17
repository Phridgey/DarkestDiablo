"""Build the mod into build/darkestdiablo/ (data in darkestdiablo.mpq/, plugin in d2rloader/plugins/).

Usage: python tools/build.py

1. Copies src/modinfo.json and vanilla/data/global/dataversionbuild.txt.
2. Copies everything under src/data/ as it is.
3. Runs each src/patches/*.py in name order. A patch defines patch(t), where t.table(name)
   loads a vanilla table (e.g. "levels") and returns a Table you can change in place.
   Only tables a patch touched are written to the build.
   t.layout(name) does the same for a game menu layout (e.g. "mainmenupanelhd"): it returns the
   parsed JSON, to change in place.
4. Writes the Campfire's own panels (tools/camp_layouts.py) and copies src/d2rloader/ (the mod's
   D2RLoader settings) next to the data.
"""
import importlib.util, json, shutil, sys
sys.path.insert(0, str(__import__('pathlib').Path(__file__).resolve().parent))
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VANILLA = ROOT / "vanilla" / "data"
OUT = ROOT / "build" / "darkestdiablo" / "darkestdiablo.mpq"
EXCEL = Path("global") / "excel"


class Table:
    """A tab-separated D2 table: a header row, then one dict per row."""

    def __init__(self, path):
        self.path = path
        text = path.read_bytes().decode("utf-8-sig")
        lines = text.split("\r\n") if "\r\n" in text else text.split("\n")
        while lines and lines[-1] == "":
            lines.pop()
        self.columns = lines[0].split("\t")
        self.rows = []
        for line in lines[1:]:
            cells = line.split("\t")
            row = dict(zip(self.columns, cells + [""] * (len(self.columns) - len(cells))))
            row["__width__"] = len(cells)  # keep each row's original cell count
            self.rows.append(row)

    def find(self, column, value):
        return [r for r in self.rows if r.get(column) == value]

    def first(self, column, value):
        hits = self.find(column, value)
        if not hits:
            raise KeyError(f"{self.path.name}: no row where {column} = {value!r}")
        return hits[0]

    def add_column(self, name, default=""):
        if name not in self.columns:
            self.columns.append(name)
            for r in self.rows:
                r[name] = default

    def save(self, path):
        path.parent.mkdir(parents=True, exist_ok=True)
        out = ["\t".join(self.columns)]
        for r in self.rows:
            cells = [r.get(c, "") for c in self.columns]
            width = max(r.get("__width__", len(cells)),
                        max((i + 1 for i, v in enumerate(cells) if v != ""), default=0))
            out.append("\t".join(cells[:width]))
        path.write_bytes(("\r\n".join(out) + "\r\n").encode("utf-8"))


def loads_lenient(text):
    """Parse the game's layout files: JSON with // comments and trailing commas."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':  # copy a string as it is
            j = i + 1
            while text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith("//", i):
            while i < n and text[i] != "\n":
                i += 1
        elif text.startswith("/*", i):
            i = text.index("*/", i) + 2
        elif c == ",":
            j = i + 1
            while True:  # look past spaces and comments for what follows the comma
                while j < n and text[j].isspace():
                    j += 1
                if text.startswith("//", j):
                    while j < n and text[j] != "\n":
                        j += 1
                elif text.startswith("/*", j):
                    j = text.index("*/", j) + 2
                else:
                    break
            if j >= n or text[j] not in "}]":
                out.append(c)
            i += 1
        else:
            out.append(c)
            i += 1
    return json.loads("".join(out))


def find_widget(node, name):
    """The widget called `name` in a parsed layout (searching children), or None."""
    if node.get("name") == name:
        return node
    for child in node.get("children", []):
        hit = find_widget(child, name)
        if hit:
            return hit
    return None


class Tables:
    def __init__(self):
        self.loaded = {}
        self.layouts = {}

    def layout(self, name, change=True):
        """A vanilla menu layout as parsed JSON. With change=False it is only read, not shipped."""
        src = VANILLA / "global" / "ui" / "layouts" / f"{name}.json"
        if not src.exists():
            raise FileNotFoundError(f"{src} is missing. Extract data/global/ui/layouts from the game (see README.md).")
        if not change:
            return self.layouts.get(name) or loads_lenient(src.read_text(encoding="utf-8-sig"))
        if name not in self.layouts:
            self.layouts[name] = loads_lenient(src.read_text(encoding="utf-8-sig"))
        return self.layouts[name]

    widget = staticmethod(find_widget)

    def table(self, name):
        if name not in self.loaded:
            src = VANILLA / EXCEL / f"{name}.txt"
            if not src.exists():
                raise FileNotFoundError(f"{src} is missing. Extract the vanilla files first.")
            self.loaded[name] = Table(src)
        return self.loaded[name]


def write_camp_data(t, target):
    """What the plugin needs to draw saved heroes at the campfire, one fact per line:
    "spot <Class> <x> <y> <width> <height> <px> <py> <pz> <yaw>"  where a class stands on the hero screen
    "look <Class> <layout fields>"                                what a new hero of the class holds and wears there
    "item <code> w|s <model file>"                                a weapon or shield and its model
    "item <code> b <torso> <legs> <right arm> <left arm> <pad 1> <pad 2>"  body armor, 0 light to 2 heavy
    Read from the vanilla files at build time, so nothing of Blizzard's is in the repo."""
    lines = []
    create = loads_lenient((VANILLA / "global" / "ui" / "layouts" / "charactercreatepanelhd.json").read_text(encoding="utf-8-sig"))
    for child in create["children"]:
        if child.get("type") == "HDCharacterSelectorWidget":
            f = child["fields"]
            r, pos = f["rect"], f["player/position"]
            lines.append("spot %s %d %d %d %d %s %s %s %s" % (
                f["player/playerClass"], r["x"], r["y"], r["width"], r["height"], pos[0], pos[1], pos[2], f["player/yawDegrees"]))
            look = {k: v for k, v in f.items() if k.startswith("player/") and k not in ("player/playerClass", "player/position", "player/yawDegrees")}
            lines.append("look %s %s" % (f["player/playerClass"], json.dumps(look, separators=(",", ":"))[1:-1]))
    models = {}
    items_json = VANILLA / "hd" / "items" / "items.json"
    if items_json.exists():
        parsed = loads_lenient(items_json.read_text(encoding="utf-8-sig"))
        for entry in (parsed if isinstance(parsed, list) else [parsed]):
            for code, value in entry.items():
                if isinstance(value, dict) and value.get("asset"):
                    models[code] = value["asset"]
    else:
        print("note: vanilla/data/hd/items/items.json is missing; heroes at the campfire show armor weight only.")
    try:
        slots = {r["Code"]: r.get("BodyLoc1", "") for r in t.table("itemtypes").rows}
        for r in t.table("weapons").rows:
            if r.get("code") in models:
                lines.append(f"item {r['code']} w data/hd/items/weapon/{models[r['code']]}.json")
        for r in t.table("armor").rows:
            code, where = r.get("code", ""), slots.get(r.get("type", ""), "")
            if where == "tors":
                lines.append("item %s b %s" % (code, " ".join(r.get(c) or "0" for c in ("Torso", "Legs", "rArm", "lArm", "rSPad", "lSPad"))))
            elif where in ("rarm", "larm") and code in models:
                lines.append(f"item {code} s data/hd/items/armor/{models[code]}.json")
    except (FileNotFoundError, KeyError) as error:
        print(f"note: item tables not read ({error}); heroes at the campfire show no gear.")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote: d2rloader/camp_data.txt ({len(lines)} lines)")


def main():
    if not VANILLA.exists():
        sys.exit("vanilla/ is missing. Extract the game's tables first (see README.md).")
    want = (ROOT / "D2R_VERSION").read_text().strip()
    have = (VANILLA.parent / "D2R_VERSION").read_text().strip() if (VANILLA.parent / "D2R_VERSION").exists() else "?"
    if want != have:
        print(f"WARNING: repo targets {want} but vanilla/ was extracted from {have}.")

    if OUT.exists():
        shutil.rmtree(OUT)
    OUT.mkdir(parents=True)
    shutil.copy(ROOT / "src" / "modinfo.json", OUT / "modinfo.json")
    (OUT / "data" / "global").mkdir(parents=True)
    shutil.copy(VANILLA / "global" / "dataversionbuild.txt", OUT / "data" / "global" / "dataversionbuild.txt")

    data = ROOT / "src" / "data"
    if data.exists():
        shutil.copytree(data, OUT, dirs_exist_ok=True)

    # Hand-made maps: compose each src/maps/*.json layout from the game's rooms, copy into the mod.
    maps = sorted((ROOT / "src" / "maps").glob("*.json"))
    if maps:
        import compose_map
        for spec in maps:
            sys.argv = ["compose_map.py", str(spec)]
            compose_map.main()
        shutil.copytree(ROOT / "build" / "maps" / "data", OUT / "data", dirs_exist_ok=True)

    t = Tables()
    for p in sorted((ROOT / "src" / "patches").glob("*.py")):
        spec = importlib.util.spec_from_file_location(p.stem, p)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        mod.patch(t)
        print(f"patched: {p.name}")
    for name, table in t.loaded.items():
        table.save(OUT / "data" / EXCEL / f"{name}.txt")
        print(f"wrote: data/global/excel/{name}.txt")

    for name, layout in t.layouts.items():
        target = OUT / "data" / "global" / "ui" / "layouts" / f"{name}.json"
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(layout, indent=1), encoding="utf-8")
        print(f"wrote: data/global/ui/layouts/{name}.json")

    # The Campfire's own panels, and the mod's D2RLoader settings (more shared stash pages).
    import camp_layouts
    panels = OUT / "data" / "global" / "ui" / "layouts" / "darkestdiablo"
    panels.mkdir(parents=True, exist_ok=True)
    for name, make in camp_layouts.LAYOUTS.items():
        (panels / f"{name}hd.json").write_text(json.dumps(make(), indent=1), encoding="utf-8")
    # The save slot screen's text changes (who is by each fire), so the plugin hands it to the
    # game itself each time, from this template. A copy in the layouts folder would win over it.
    template = OUT.parent / "d2rloader" / "SaveSlots.layout.json"
    template.parent.mkdir(parents=True, exist_ok=True)
    template.write_text(json.dumps(camp_layouts.save_slots()), encoding="utf-8")
    loader = ROOT / "src" / "d2rloader"
    if loader.exists():
        shutil.copytree(loader, OUT.parent / "d2rloader", dirs_exist_ok=True)
    write_camp_data(t, OUT.parent / "d2rloader" / "camp_data.txt")

    # The D2RLoader plugin (built by tools/build-plugin.cmd) goes in the mod's own plugin folder.
    plugin = ROOT / "build" / "plugin" / "darkestdiablo.dll"
    plugin_dir = OUT.parent / "d2rloader" / "plugins"
    if plugin.exists():
        plugin_dir.mkdir(parents=True, exist_ok=True)
        target = plugin_dir / plugin.name
        if target.exists() and target.read_bytes() == plugin.read_bytes():
            print("plugin unchanged: d2rloader/plugins/darkestdiablo.dll")
        else:
            try:
                shutil.copy(plugin, target)
                print("placed: d2rloader/plugins/darkestdiablo.dll")
            except PermissionError:
                print("WARNING: could not replace the plugin; close the game and build again.")
    else:
        print("note: no plugin built yet (run tools\\build-plugin.cmd)")

    print(f"Built {OUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
