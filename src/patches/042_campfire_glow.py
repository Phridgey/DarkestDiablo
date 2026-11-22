"""The Campfire's selection glow: a soft gold light behind the picked hero's name plate and
behind the Enter button (Sam, 2026-11-20: gold and softer instead of the flat red box).

The pictures are drawn here at build time, so no art file sits in the repo. Each is a rounded
box (the size of the plate or the button) with a warm gold halo fading out around it. The
plugin (FireLayoutJson in src/plugin/campfire.cpp) shows them as ImageWidgets at these sizes,
so keep the sizes below in step with the plugin.

The aurora: soft patches of light all around the plate and the button, each slowly brightening
and fading on its own (the plugin shows one brightness level of each at a time, ShimmerStep in
campfire.cpp).

Pictures (both graphics qualities; the low one is half size):
  PANEL\\DarkestDiablo\\HeroGlow   -> hd/global/ui/panel/darkestdiablo/heroglow(.lowend).sprite
  PANEL\\DarkestDiablo\\EnterGlow  -> hd/global/ui/panel/darkestdiablo/enterglow(.lowend).sprite
  PANEL\\DarkestDiablo\\AuraH1..6  -> hd/global/ui/panel/darkestdiablo/aurah1..6(.lowend).sprite
  PANEL\\DarkestDiablo\\AuraV1..6  -> hd/global/ui/panel/darkestdiablo/aurav1..6(.lowend).sprite
"""
import math
import struct

# A vanilla one-frame RGBA sprite whose header we copy (the build already reads it).
TEMPLATE = "hd/global/ui/panel/inventory/background{}.sprite"
OUT = "hd/global/ui/panel/darkestdiablo/{}{}.sprite"

# name: (box width, box height, corner radius, margin around the box) in layout units.
# HeroGlow: the 280x92 name plate (PlateW/PlateH in the plugin). EnterGlow: FrontEnd_ButtonLarge at scale 1.35.
GLOWS = {
    "heroglow": (280, 92, 8, 56),
    "enterglow": (824, 162, 22, 72),
}

PALE = (255, 232, 160)   # near the frame
AMBER = (232, 156, 44)   # far out in the halo


def glow_pixels(box_w, box_h, radius, margin, scale=1.0):
    """(width, height, RGBA bytes) of a gold halo around a rounded box, straight alpha."""
    w = int(round((box_w + 2 * margin) * scale))
    h = int(round((box_h + 2 * margin) * scale))
    bx, by, r, m = box_w * scale / 2, box_h * scale / 2, radius * scale, margin * scale
    cx, cy = w / 2, h / 2
    out = bytearray(w * h * 4)
    i = 0
    for y in range(h):
        qy = abs(y + 0.5 - cy) - (by - r)
        for x in range(w):
            qx = abs(x + 0.5 - cx) - (bx - r)
            # Signed distance to the rounded box: < 0 inside, > 0 outside.
            d = math.hypot(max(qx, 0.0), max(qy, 0.0)) + min(max(qx, qy), 0.0) - r
            if d > 0:
                t = d / m
                if t >= 1:
                    i += 4
                    continue
                # A bright rim that falls off fast, plus a wide soft haze; both reach 0 at the edge.
                fade = (1 - t) ** 2
                a = (0.55 * math.exp(-d / (0.10 * m)) + 0.42 * math.exp(-(t * 1.8) ** 2)) * fade
                mix = min(1.0, t * 1.6)
            else:
                # Inside: warm light, strongest at the rim (mostly hidden by the plate or button).
                a = 0.30 + 0.70 * math.exp(d / (0.08 * m))
                mix = 0.0
            a = min(1.0, a)
            out[i] = int(PALE[0] + (AMBER[0] - PALE[0]) * mix)
            out[i + 1] = int(PALE[1] + (AMBER[1] - PALE[1]) * mix)
            out[i + 2] = int(PALE[2] + (AMBER[2] - PALE[2]) * mix)
            out[i + 3] = int(a * 255 + 0.5)
            i += 4
    return w, h, bytes(out)


# The aurora: soft patches of light along the edges, each drawn at LEVELS brightnesses (the
# plugin, AuraStep in campfire.cpp, shows one level at a time). H runs along a top or bottom
# edge, V along a side; sizes must match AuraHW/AuraHH/AuraVW/AuraVH in the plugin.
LEVELS = 6
AURA = {"aurah": (280, 130), "aurav": (130, 200)}


def aura_pixels(w, h, level, side, scale=1.0):
    """(width, height, RGBA bytes) of one soft patch of aurora light at a brightness level.
    Faint streaks across its length give it a curtain look; they shift a little per level,
    so the light seems to waver as the plugin moves between levels."""
    W, H = int(round(w * scale)), int(round(h * scale))
    peak = 0.13 * level
    warm = level / LEVELS  # brighter patches are paler, dimmer ones deeper amber
    out = bytearray(W * H * 4)
    i = 0
    for y in range(H):
        for x in range(W):
            u = (x + 0.5) / W * 2 - 1
            v = (y + 0.5) / H * 2 - 1
            along, across = (v, u) if side else (u, v)
            r = math.hypot(along, across * 1.15)
            if r >= 1:
                i += 4
                continue
            soft = math.exp(-(r / 0.5) ** 2) * (1 - r) ** 1.5
            streak = 0.78 + 0.22 * math.sin(along * 17.0 + level * 1.3) * math.sin(along * 7.0 - level * 0.7)
            a = min(1.0, peak * soft * streak * 1.6)
            out[i] = int(AMBER[0] + (PALE[0] - AMBER[0]) * warm)
            out[i + 1] = int(AMBER[1] + (PALE[1] - AMBER[1]) * warm)
            out[i + 2] = int(AMBER[2] + (PALE[2] - AMBER[2]) * warm)
            out[i + 3] = int(a * 255 + 0.5)
            i += 4
    return W, H, bytes(out)


def sprite(template, w, h, pixels):
    """A one-frame RGBA .sprite, header copied from a vanilla one of the same kind."""
    if template[:4] != b"SpA1":
        raise RuntimeError("The template picture is not a sprite file the build knows.")
    old_w, frames = struct.unpack_from("<I", template, 8)[0], struct.unpack_from("<I", template, 20)[0]
    if frames != 1:
        raise RuntimeError("The template picture has more than one frame; has the game changed?")
    header = bytearray(template[:40])
    if struct.unpack_from("<H", header, 6)[0] == old_w & 0xFFFF:
        struct.pack_into("<H", header, 6, w)  # frame width
    struct.pack_into("<I", header, 8, w)
    struct.pack_into("<I", header, 12, h)
    struct.pack_into("<I", header, 32, len(pixels))
    return bytes(header) + pixels


def patch(t):
    for suffix, scale in (("", 1.0), (".lowend", 0.5)):
        template = t.game_file(TEMPLATE.format(suffix))
        for name, (bw, bh, r, m) in GLOWS.items():
            w, h, px = glow_pixels(bw, bh, r, m, scale)
            t.add_file(OUT.format(name, suffix), sprite(template, w, h, px))
        for name, (aw, ah) in AURA.items():
            for level in range(1, LEVELS + 1):
                w, h, px = aura_pixels(aw, ah, level, name == "aurav", scale)
                t.add_file(OUT.format(f"{name}{level}", suffix), sprite(template, w, h, px))
    print(f"  campfire glow: {len(GLOWS) + len(AURA) * LEVELS} pictures, gold")
