"""How much of each arena map the hero can walk to (2026-11-24: "at least a third of a map").

Reads the built maps (build/maps/.../darkest_*.ds1, made by tools/build.py) and finds the largest
area of floor connected without crossing a wall, as a share of the whole map. Maps under a third
are listed as too closed.

    python tools/check_reach.py            # every built arena map
    python tools/check_reach.py --all      # every map, not only the closed ones

Limits: walls are read from the map's wall layers. Caves (Act I caves, the Spider Cavern...) keep
their walls in the tile sheets instead (CLAUDE.md, lessons), so they look fully open here; objects
(trees, braziers) aren't counted either. A map that passes still needs a look in the game.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import compose_map as c  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
# Wall orientations (.ds1) and the tile edges they close: W = left (west) edge, N = upper edge.
BLOCK = {1: "W", 2: "N", 3: "WN", 5: "N", 6: "N", 7: "WN", 8: "W", 9: "W"}
ENOUGH = 1 / 3


def reachable(m):
    """(largest connected floor area, its first cell, map area)."""
    W, H = m["width"], m["height"]
    floor_layer = m["floors"][0]

    def floor(x, y):
        return 0 <= x < W - 1 and 0 <= y < H - 1 and floor_layer[y * W + x] and not floor_layer[y * W + x] & 0x20000

    west, north = set(), set()
    for walls, orients in zip(m["walls"], m["orients"]):
        for i, (w, o) in enumerate(zip(walls, orients)):
            if w and o in BLOCK:
                if "W" in BLOCK[o]:
                    west.add(i)
                if "N" in BLOCK[o]:
                    north.add(i)
    seen, best = set(), (0, None)
    for y in range(H):
        for x in range(W):
            if (x, y) in seen or not floor(x, y):
                continue
            stack, size = [(x, y)], 0
            seen.add((x, y))
            while stack:
                cx, cy = stack.pop()
                size += 1
                i = cy * W + cx
                for dx, dy, blocked in ((1, 0, i + 1 in west), (-1, 0, i in west), (0, 1, i + W in north), (0, -1, i in north)):
                    n = (cx + dx, cy + dy)
                    if not blocked and floor(*n) and n not in seen:
                        seen.add(n)
                        stack.append(n)
            best = max(best, (size, (x, y)))
    return best[0], best[1], (W - 1) * (H - 1)


def main():
    show_all = "--all" in sys.argv
    maps = sorted((ROOT / "build" / "maps").rglob("darkest_*.ds1"))
    if not maps:
        sys.exit("No built maps: run python tools/build.py first.")
    closed = 0
    for path in maps:
        size, _, area = reachable(c.ds1_parse(path.read_bytes()))
        share = size / area if area else 0
        if share < ENOUGH:
            closed += 1
        if show_all or share < ENOUGH:
            print(f"{'TOO CLOSED' if share < ENOUGH else 'ok':10s} {path.stem:28s} {size:5d} of {area:5d} tiles ({share:.0%})")
    print(f"{closed} of {len(maps)} maps under a third reachable.")


if __name__ == "__main__":
    main()
