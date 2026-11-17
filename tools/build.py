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
    loader = ROOT / "src" / "d2rloader"
    if loader.exists():
        shutil.copytree(loader, OUT.parent / "d2rloader", dirs_exist_ok=True)

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
