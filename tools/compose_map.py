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
def room_size(spec):
    """A room's size in tiles, (width, height): "room" is one number (square rooms) or [w, h]
    (e.g. the Barracks' 10x14 rooms)."""
    r = spec["room"]
    return (r[0], r[1]) if isinstance(r, list) else (r, r)


def norm_tile(f):
    return f.lower().replace("\\", "/").rsplit(".", 1)[0]


def compose(spec):
    rw, rh = room_size(spec)
    keep = {k.lower() for k in spec.get("keep_monsters", [])}
    placed = []
    for p in spec["pieces"]:
        f = VANILLA / "global" / "tiles" / (p["piece"] + ".ds1")
        if not f.exists():
            sys.exit(f"{f} is missing. Extract data/global/tiles and data/hd/env/preset for '{spec['folder']}' into vanilla/.")
        d = ds1_parse(f.read_bytes())
        if len(spec["pieces"]) > 1 and (d["width"] != rw + 1 or d["height"] != rh + 1):
            sys.exit(f"{p['piece']} is {d['width']}x{d['height']}, expected {rw + 1}x{rh + 1}")
        holes = sum(1 for v in d["floors"][0] if v == 0) if d["floors"] else 0
        if (len(spec["pieces"]) > 1 and holes > (d["width"] * d["height"]) // 10 and not spec.get("circle")  # a circle fills every cell
                and p["piece"] not in spec.get("allow_holes", [])):  # rooms that are walled-in with void around
            # In a stitched map every room must be complete. "Theme" rooms are partial overlays;
            # stitched in as a room they leave cells with no tile inside the map, and the game
            # crashed loading the level (BC_VERIFY: hTile). A single room with empty edges is fine.
            sys.exit(f"{p['piece']} has no floor in {holes} of {d['width'] * d['height']} cells (a partial "
                     "overlay room, e.g. a 'theme' room); use a complete room.")
        placed.append((p, d))
    gw = max(p["x"] for p, _ in placed) + 1
    gh = max(p["y"] for p, _ in placed) + 1
    W, H = gw * rw + 1, gh * rh + 1
    if len(placed) == 1:  # one whole map (e.g. an outdoor preset), any size
        W, H = placed[0][1]["width"], placed[0][1]["height"]
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
        ox, oy = p["x"] * rw, p["y"] * rh
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
        drop_monsters = set(spec.get("drop_monsters", []))  # e.g. one monster spot of a kept room
        drop = set(spec.get("drop_objects", []))  # e.g. a room's treasure chests
        for o in d["objects"]:
            if o["type"] == 2 and o["id"] in drop:
                continue
            if o["type"] == 2 or (monsters and o["type"] == 1 and o["id"] not in drop_monsters):
                m["objects"].append({**o, "x": o["x"] + ox * 5, "y": o["y"] + oy * 5})
    # Extra objects from the layout, e.g. a waypoint: {"type": 2, "id": <objpreset Index>, "tile": [x, y]}
    # Placed at sub-tile tile*5+4, as the game's own maps place their waypoints.
    for o in spec.get("extra_objects", []):
        tx, ty = o["tile"]
        m["objects"].append({"type": o["type"], "id": o["id"], "x": tx * 5 + 4, "y": ty * 5 + 4, "flags": 0})
    # "open": [x0, y0, x1, y1] (tiles, inclusive) makes one big room of the rooms in that box: no
    # walls inside it, floor everywhere inside, the rooms' own walls kept on its edge. build_hd
    # leaves out the models inside it too. Objects stay.
    box = spec.get("open")
    if box:
        x0, y0, x1, y1 = box
        base = m["floors"][0]
        inner = [base[y * W + x] for y in range(y0 + 1, y1) for x in range(x0 + 1, x1)]
        counts = {}
        for v in inner:
            if v and not v & 0x20000:
                counts[v] = counts.get(v, 0) + 1
        floor = max(counts, key=counts.get)
        for y in range(y0 + 1, y1):
            for x in range(x0 + 1, x1):
                i = y * W + x
                for l in range(len(m["walls"])):
                    m["walls"][l][i] = m["orients"][l][i] = 0
                m["shadow"][i] = 0
                for a in m["floors"][1:]:
                    a[i] = 0
                if not base[i] or base[i] & 0x20000:
                    base[i] = floor
        # The box's edge is its wall. Where the rooms left a gap in it (between two rooms' walls),
        # the gap is made unwalkable, so nobody walks out of the hall.
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                if x in (x0, x1) or y in (y0, y1):
                    i = y * W + x
                    if not any(m["walls"][l][i] for l in range(len(m["walls"]))) and base[i]:
                        base[i] |= 0x20000
    # Cells with no floor at all (rooms walled in with void around them, "allow_holes") get the
    # first floor tile marked unwalkable, so the map has no empty cells.
    if spec.get("allow_holes") and m["floors"]:
        base = m["floors"][0]
        fill = next((v for v in base if v and not v & 0x20000), 0)
        for i, v in enumerate(base):
            if not v and fill:
                base[i] = fill | 0x20000
    # "circle": {"radius": R} makes one round open cavern from the stitched rooms: floor inside a
    # circle of R tiles around the map's centre, solid rock (unwalkable) outside, and walls only
    # along the edge (for the automap; "walls": false leaves them out).
    # build_hd drops every wall and rock model and rings the circle with cave rocks instead.
    cr = spec.get("circle")
    if cr:
        base = m["floors"][0]
        counts, rock = {}, None
        for v in base:
            if not v:
                continue
            if ((v >> 20) & 0x3F) == 30:
                rock = rock or v
            elif not v & 0x20000:
                counts[v] = counts.get(v, 0) + 1
        floor = max(counts, key=counts.get)
        # The rooms' own wall tiles, for the boundary walls below: the most used left (1) and
        # top (2) wall tile.
        wall_tiles = {}
        for o in (1, 2):
            seen_walls = [m["walls"][0][i] for i in range(cells) if m["walls"] and m["orients"][0][i] == o and m["walls"][0][i]]
            if seen_walls:
                wall_tiles[o] = max(set(seen_walls), key=seen_walls.count)
        # Rooms without straight walls (the open cave rooms) borrow them: "wall_from" names rooms
        # of the same tile set that have them (e.g. a cave theme room).
        for piece in cr.get("wall_from", []):
            d = ds1_parse((VANILLA / "global" / "tiles" / (piece + ".ds1")).read_bytes())
            for o in (1, 2):
                seen_walls = [w[i] for w, ow in zip(d["walls"], d["orients"]) for i in range(len(w)) if ow[i] == o and w[i]]
                if seen_walls and o not in wall_tiles:
                    wall_tiles[o] = max(set(seen_walls), key=seen_walls.count)
        cx, cy, r = (W - 1) / 2, (H - 1) / 2, cr["radius"]
        for y in range(H):
            for x in range(W):
                i = y * W + x
                for l in range(len(m["walls"])):
                    m["walls"][l][i] = m["orients"][l][i] = 0
                m["shadow"][i] = 0
                for a in m["floors"][1:]:
                    a[i] = 0
                if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                    base[i] = floor
                else:
                    base[i] = (rock or floor) | 0x20000
        # Walls along the circle's edge, in the gameplay map only (the HD look keeps its ring of
        # models). They are what the automap draws, so the map shows the hall's boundary; the
        # floor tiles alone draw nothing there. A wall sits on a cell's left (1) or top (2) edge;
        # a cell with both gets the corner (3), as the crypt rooms do.
        if cr.get("walls", True) and m["walls"] and 1 in wall_tiles and 2 in wall_tiles:
            inside = lambda x, y: 0 <= x < W and 0 <= y < H and (x - cx) ** 2 + (y - cy) ** 2 <= r * r
            for y in range(H):
                for x in range(W):
                    left = inside(x, y) != inside(x - 1, y)
                    top = inside(x, y) != inside(x, y - 1)
                    if left or top:
                        i = y * W + x
                        m["walls"][0][i] = wall_tiles[1 if left else 2]
                        m["orients"][0][i] = 3 if left and top else 1 if left else 2
        # The rooms' own objects (torches and such) only where the floor still is.
        m["objects"] = [o for o in m["objects"]
                        if (o["x"] / 5 - cx) ** 2 + (o["y"] / 5 - cy) ** 2 <= (r - 1) ** 2]
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


def is_solid_model(e):
    """A wall, fence, rock or other model with a body: everything that shapes a maze room."""
    kinds = {k.get("type") for k in e.get("components") or []}
    return bool(kinds & {"ModelDefinitionComponent", "PhysicsBodyDefinitionComponent"})


RING_MODELS = ["data/hd/env/model/act1/caves/act1_caves_rocks/rocks01.model",
               "data/hd/env/model/act1/caves/act1_caves_rocks/mound01.model",
               "data/hd/env/model/act1/caves/act1_caves_rocks/mound02.model",
               "data/hd/env/model/act1/caves/act1_caves_rocks/stalagmite02.model",
               "data/hd/env/model/act1/caves/act1_caves_rocks/stalagmite03.model",
               "data/hd/env/model/act1/caves/act1_caves_rocks/stalagmite05.model"]


# The default ring: three rows of cave rocks, mounds and stalagmites (the Den's round cave).
DEFAULT_RING = [
    {"dist": 0.5, "step": 0.9, "scale": [1.0, 1.4], "models": RING_MODELS},
    {"dist": 1.4, "step": 1.2, "scale": [1.3, 1.8], "models": RING_MODELS},
    {"dist": 2.5, "step": 1.5, "scale": [1.6, 2.4], "models": RING_MODELS},
]


def rock_ring(spec, entities, deps, seen, next_id):
    """Ring the circle with models, row by row from the edge outward. A map's "circle" may give its
    own "ring": rows of {"dist": tiles past the edge, "step": tiles between models, "scale": [min,
    max], "models": [paths], "turn": false to keep them facing the centre}. Default: cave rocks.
    Random placement uses a fixed seed, so every build makes the same map. HD units: 10 per tile."""
    import math, random
    rnd = random.Random(1)
    gw = max(p["x"] for p in spec["pieces"]) + 1
    gh = max(p["y"] for p in spec["pieces"]) + 1
    rw, rh = room_size(spec)
    W, H = gw * rw + 1, gh * rh + 1
    cx, cz, r = (W - 1) / 2 * 10, (H - 1) / 2 * 10, spec["circle"]["radius"] * 10
    rows = spec["circle"].get("ring") or DEFAULT_RING
    wanted = {m for row in rows for m in row["models"]}
    # Each model's settings (light, shadow, ground blend) as Blizzard's own Act I rooms use them.
    templates = {}
    for f in sorted((VANILLA / "hd" / "env" / "preset" / "act1").glob("*/*.json")):
        for e in json.loads(f.read_text(encoding="utf-8-sig")).get("entities") or []:
            for k in e.get("components") or []:
                if k.get("type") == "ModelDefinitionComponent" and k.get("filename") in wanted:
                    templates.setdefault(k["filename"], k)
        if len(templates) == len(wanted):
            break
    missing = wanted - set(templates)
    if missing:
        sys.exit(f"ring models not found in any Act I room: {sorted(missing)}")
    for path in sorted(wanted):
        if path not in seen["models"]:
            seen["models"].add(path)
            deps["models"].append({"path": path})
    for n, row in enumerate(rows):
        rr = r + row["dist"] * 10
        count = max(1, int(2 * math.pi * rr / (row["step"] * 10)))
        for k in range(count):
            a = 2 * math.pi * (k + rnd.random() * 0.6 * row.get("jitter", 1)) / count
            if row.get("turn", True):
                yaw = rnd.random() * 2 * math.pi
            else:
                yaw = -a + math.pi / 2  # face the centre
            sc = rnd.uniform(*row["scale"])
            model = rnd.choice(row["models"])
            name = f"ring_{n}_{k}"
            entities.append({"type": "Entity", "name": name, "id": next_id, "components": [
                {"type": "TransformDefinitionComponent", "name": f"{name}_Transform",
                 "position": {"x": cx + rr * math.cos(a), "y": 0.0, "z": cz + rr * math.sin(a)},
                 "orientation": {"x": 0.0, "y": math.sin(yaw / 2), "z": 0.0, "w": math.cos(yaw / 2)},
                 "scale": {"x": sc, "y": sc, "z": sc}, "inheritOnlyPosition": False},
                dict(templates[model], name=f"{name}_Model")]})
            next_id += 1
    return next_id


def inside_open(box, e):
    """Is this entity inside the "open" box (not on its edge)? HD units: 10 per tile."""
    x0, y0, x1, y1 = box
    for k in e.get("components") or []:
        if k.get("type") == "TransformDefinitionComponent" and k.get("position"):
            x, z = k["position"]["x"] / 10, k["position"]["z"] / 10
            return x0 + 0.6 < x < x1 - 0.6 and y0 + 0.6 < z < y1 - 0.6
    return False


def build_hd(spec):
    rw, rh = room_size(spec)
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
        dx, dz, suffix = p["x"] * rw * 10, p["y"] * rh * 10, f"p{p['x']}_{p['y']}"
        for k in KINDS:
            for d in (hd.get("dependencies") or {}).get(k) or []:
                if d.get("path") not in seen[k]:
                    seen[k].add(d.get("path"))
                    deps[k].append(d)
        for e in hd.get("entities") or []:
            c = move(e, dx, dz, suffix)
            if spec.get("circle") and is_solid_model(c):
                continue  # the circle gets its own rock ring below
            if spec.get("open") and is_solid_model(c) and inside_open(spec["open"], c):
                continue  # the open box's inner walls and pillars
            entities.append(c)
        ground = move(hd["terrain"], dx, dz, suffix)
        if terrain is None:
            terrain = ground
        else:
            ground["components"] = [c for c in ground.get("components") or [] if c.get("type") != "TerrainDefinitionComponent"]
            entities.append(ground)
        biome = biome or hd.get("biomeFilename", "")
    if spec.get("circle"):
        next_id = rock_ring(spec, entities, deps, seen, next_id)
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
