"""Compose a D2R map from the game's own maze rooms, as a .ds1 (gameplay) plus its HD .json (look).

Usage: python tools/compose_map.py src/maps/<name>.json

The layout file names the rooms to stitch on a grid, for example:

    {
      "name": "darkest_den",
      "folder": "act1/caves",
      "room": 24,
      "keep_monsters": ["act1/caves/cavenspec"],
      "pieces": [
        {"x": 0, "y": 0, "piece": "act1/caves/cavespre1"},
        {"x": 0, "y": 1, "piece": "act1/caves/cavens"}
      ]
    }

The rooms are read from vanilla/ (your own extracted game files), so the repo only holds the
layout. Output goes to build/maps/: data/global/tiles/<folder>/<name>.ds1 and
data/hd/env/preset/<folder>/<name>.json, which tools/build.py copies into the mod.

The DS1 reading/writing and the HD stitching follow the D2R Map Editor
(github.com/reja123456789-pixel/D2RMapEditor, "free to use, change and share"): rooms share
their border row/column, later rooms overwrite the shared border where they have something,
and in the HD file every room's entities move by (grid cell * room size * 10) with the first
room's terrain kept as the map's terrain and the others kept as plain entities.
"""
import copy
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VANILLA = ROOT / "vanilla" / "data"
OUT = ROOT / "build" / "maps"


# ---------------------------------------------------------------- DS1 (version 18 and up)
class Reader:
    def __init__(self, data):
        self.data, self.p = data, 0

    def i32(self):
        v = struct.unpack_from("<i", self.data, self.p)[0]
        self.p += 4
        return v

    def u32s(self, n):
        v = list(struct.unpack_from(f"<{n}I", self.data, self.p))
        self.p += 4 * n
        return v

    def left(self):
        return len(self.data) - self.p


def has_tag_layer(tag_type):
    return tag_type in (1, 2)


def ds1_parse(data):
    r = Reader(data)
    m = {"version": r.i32(), "width": r.i32() + 1, "height": r.i32() + 1}
    v = m["version"]
    if v < 16:
        raise ValueError(f"DS1 version {v} not supported (D2R ships 18)")
    m["act"] = r.i32()
    m["tagType"] = r.i32()
    m["files"] = []
    for _ in range(r.i32()):
        end = data.index(b"\0", r.p)
        m["files"].append(data[r.p:end].decode("latin-1"))
        r.p = end + 1
    walls, floors = r.i32(), r.i32()
    cells = m["width"] * m["height"]
    m["walls"], m["orients"], m["floors"] = [], [], []
    for _ in range(walls):
        m["walls"].append(r.u32s(cells))
        m["orients"].append(r.u32s(cells))
    for _ in range(floors):
        m["floors"].append(r.u32s(cells))
    m["shadow"] = r.u32s(cells)
    m["tag"] = r.u32s(cells) if has_tag_layer(m["tagType"]) else None
    m["objects"] = []
    if r.left() >= 4:
        for _ in range(r.i32()):
            t, i, x, y, f = (r.i32() for _ in range(5))
            m["objects"].append({"type": t, "id": i, "x": x, "y": y, "flags": f})
    m["groups"] = None
    if has_tag_layer(m["tagType"]) and r.left() >= 4:
        m["groupUnknown"] = r.i32()
        m["groups"] = [[r.i32() for _ in range(5)] for _ in range(r.i32())]
    m["paths"] = []  # NPC patrol paths are not carried over (none in maze rooms)
    return m


def ds1_write(m):
    out = bytearray()
    i32 = lambda v: out.extend(struct.pack("<i", v))
    i32(18); i32(m["width"] - 1); i32(m["height"] - 1); i32(m["act"]); i32(0)  # tagType 0: no tag layer
    i32(len(m["files"]))
    for f in m["files"]:
        out.extend(f.encode("latin-1") + b"\0")
    i32(len(m["walls"])); i32(len(m["floors"]))
    layer = lambda a: out.extend(struct.pack(f"<{len(a)}I", *a))
    for w, o in zip(m["walls"], m["orients"]):
        layer(w); layer(o)
    for f in m["floors"]:
        layer(f)
    layer(m["shadow"])
    i32(len(m["objects"]))
    for o in m["objects"]:
        for k in ("type", "id", "x", "y", "flags"):
            i32(o[k])
    i32(0)  # no NPC paths
    return bytes(out)


# ---------------------------------------------------------------- compose
def norm_tile(f):
    return f.lower().replace("\\", "/").rsplit(".", 1)[0]


def compose(spec):
    room = spec["room"]
    keep = {k.lower() for k in spec.get("keep_monsters", [])}
    placed = []
    for p in spec["pieces"]:
        f = VANILLA / "global" / "tiles" / (p["piece"] + ".ds1")
        if not f.exists():
            sys.exit(f"{f} is missing. Extract data/global/tiles and data/hd/env/preset for '{spec['folder']}' into vanilla/.")
        d = ds1_parse(f.read_bytes())
        if d["width"] != room + 1 or d["height"] != room + 1:
            sys.exit(f"{p['piece']} is {d['width']}x{d['height']}, expected {room + 1}x{room + 1}")
        placed.append((p, d))
    gw = max(p["x"] for p, _ in placed) + 1
    gh = max(p["y"] for p, _ in placed) + 1
    W, H = gw * room + 1, gh * room + 1
    cells = W * H
    files = []
    for _, d in placed:
        for f in d["files"]:
            if f.lower().endswith((".dt1", ".tg1")) and norm_tile(f) not in {norm_tile(g) for g in files}:
                files.append(f)
    nw = max(len(d["walls"]) for _, d in placed)
    nf = max(len(d["floors"]) for _, d in placed)
    m = {"width": W, "height": H, "act": placed[0][1]["act"], "files": files,
         "walls": [[0] * cells for _ in range(nw)], "orients": [[0] * cells for _ in range(nw)],
         "floors": [[0] * cells for _ in range(nf)], "shadow": [0] * cells, "objects": []}
    for p, d in placed:
        ox, oy = p["x"] * room, p["y"] * room
        for y in range(d["height"]):
            for x in range(d["width"]):
                s, t = y * d["width"] + x, (oy + y) * W + (ox + x)
                for l, a in enumerate(d["walls"]):
                    if a[s]:
                        m["walls"][l][t] = a[s]
                        m["orients"][l][t] = d["orients"][l][s]
                for l, a in enumerate(d["floors"]):
                    if a[s]:
                        m["floors"][l][t] = a[s]
                if d["shadow"][s]:
                    m["shadow"][t] = d["shadow"][s]
        monsters = p["piece"].lower() in keep
        for o in d["objects"]:
            if o["type"] == 2 or (monsters and o["type"] == 1):
                m["objects"].append({**o, "x": o["x"] + ox * 5, "y": o["y"] + oy * 5})
    # Extra objects from the layout, e.g. a waypoint: {"type": 2, "id": <objpreset Index>, "tile": [x, y]}
    # Placed at sub-tile tile*5+4, as the game's own maps place their waypoints.
    for o in spec.get("extra_objects", []):
        tx, ty = o["tile"]
        m["objects"].append({"type": o["type"], "id": o["id"], "x": tx * 5 + 4, "y": ty * 5 + 4, "flags": 0})
    # Close the map's outer edges: maze rooms are open where the next room would join. Mark open
    # floor on the border as unwalkable (floor bit 0x20000), so players can't walk off the map.
    if spec.get("close_edges", True):
        for y in range(H):
            for x in range(W):
                if x in (0, W - 1) or y in (0, H - 1):
                    i = y * W + x
                    for a in m["floors"]:
                        if a[i] and ((a[i] >> 20) & 0x3F) != 30:
                            a[i] |= 0x20000
    return m, (gw, gh)


KINDS = ["particles", "models", "skeletons", "animations", "textures", "physics", "json", "variantdata", "objecteffects", "other"]


def build_hd(spec):
    room = spec["room"]
    deps = {k: [] for k in KINDS}
    seen = {k: set() for k in KINDS}
    entities, terrain, biome, next_id = [], None, "", 1000000000

    def move(e, dx, dz, suffix):
        nonlocal next_id
        c = copy.deepcopy(e)
        c["id"] = next_id
        next_id += 1
        c["name"] = f"{c.get('name', '')}_{suffix}"
        for k in c.get("components") or []:
            if k.get("type") == "TransformDefinitionComponent" and k.get("position"):
                k["position"]["x"] += dx
                k["position"]["z"] += dz
        return c

    for p in spec["pieces"]:
        f = VANILLA / "hd" / "env" / "preset" / (p["piece"] + ".json")
        if not f.exists():
            sys.exit(f"{f} is missing (the room's HD file).")
        hd = json.loads(f.read_text(encoding="utf-8-sig"))
        dx, dz, suffix = p["x"] * room * 10, p["y"] * room * 10, f"p{p['x']}_{p['y']}"
        for k in KINDS:
            for d in (hd.get("dependencies") or {}).get(k) or []:
                if d.get("path") not in seen[k]:
                    seen[k].add(d.get("path"))
                    deps[k].append(d)
        for e in hd.get("entities") or []:
            entities.append(move(e, dx, dz, suffix))
        ground = move(hd["terrain"], dx, dz, suffix)
        if terrain is None:
            terrain = ground
        else:
            ground["components"] = [c for c in ground.get("components") or [] if c.get("type") != "TerrainDefinitionComponent"]
            entities.append(ground)
        biome = biome or hd.get("biomeFilename", "")
    # HD look for extra objects: the game draws some objects (waypoints) from a prefab in the map's
    # HD file, not from the object itself. Sub-tile -> HD units: x2, plus the prefab's offset.
    for o in spec.get("extra_objects", []):
        pf = o.get("hd_prefab")
        if not pf:
            continue
        tx, ty = o["tile"]
        ox, oz = pf.get("offset", [0, 0])
        entities.append({"type": "Entity", "name": "prefab", "id": next_id, "components": [
            {"type": "TransformDefinitionComponent", "name": "prefab_TransformDefinitionComponent",
             "position": {"x": float((tx * 5 + 4) * 2 + ox), "y": 0.0, "z": float((ty * 5 + 4) * 2 + oz)},
             "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0}, "scale": {"x": 1.0, "y": 1.0, "z": 1.0},
             "inheritOnlyPosition": False},
            {"type": "PrefabPlacementDefinitionComponent", "name": "prefab_PrefabPlacementDefinitionComponent",
             "prefab": pf["prefab"]}]})
        next_id += 1
        for path in pf.get("models", []):
            if path not in seen["models"]:
                seen["models"].add(path)
                deps["models"].append({"path": path})
    return {"dependencies": deps, "type": "Preset", "name": "preset", "entities": entities, "terrain": terrain,
            "biomeFilename": biome, "perTileBiomeOverrides": [], "specialTiles": {}}


def main():
    spec_file = Path(sys.argv[1])
    spec = json.loads(spec_file.read_text(encoding="utf-8"))
    m, (gw, gh) = compose(spec)
    name, folder = spec["name"], spec["folder"]
    ds1_out = OUT / "data" / "global" / "tiles" / folder / f"{name}.ds1"
    hd_out = OUT / "data" / "hd" / "env" / "preset" / folder / f"{name}.json"
    ds1_out.parent.mkdir(parents=True, exist_ok=True)
    hd_out.parent.mkdir(parents=True, exist_ok=True)
    ds1_out.write_bytes(ds1_write(m))
    hd_out.write_text(json.dumps(build_hd(spec), indent=1), encoding="utf-8")
    print(f"composed {name}: {gw}x{gh} rooms, {m['width']}x{m['height']} tiles, {len(m['objects'])} objects")
    print(f"  levels.txt: DrlgType 2, SizeX {m['width'] - 1}, SizeY {m['height'] - 1}")
    return m


if __name__ == "__main__":
    main()
