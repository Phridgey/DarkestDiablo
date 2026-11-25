"""The Campfire's selection glow: a soft gold light behind the picked hero's name and a thin, dim
one all around the Enter button (gold and soft, Sam, 2026-11-20; one clean picture each since
0.20.3, before that an aurora of many patches).

The pictures are drawn here at build time, so no art file sits in the repo. Each is drawn at LEVELS
brightnesses; the plugin shows one at a time (ShimmerStep in src/plugin/campfire.cpp), so the light
breathes. Keep the sizes below in step with the plugin.

Pictures (both graphics qualities; the low one is half size):
  PANEL\\DarkestDiablo\\HeroGlow1..5   -> hd/global/ui/panel/darkestdiablo/heroglow1..5(.lowend).sprite
  PANEL\\DarkestDiablo\\EnterGlow1..5  -> hd/global/ui/panel/darkestdiablo/enterglow1..5(.lowend).sprite
"""
import math
import struct

# A vanilla one-frame RGBA sprite whose header we copy (the build already reads it).
TEMPLATE = "hd/global/ui/panel/inventory/background{}.sprite"
OUT = "hd/global/ui/panel/darkestdiablo/{}{}.sprite"

LEVELS = 5
HERO = (392, 153)               # PlateGlowW, PlateGlowH
ENTER = (824, 196, 10, 40)      # EnterW, EnterH (the button as it stands in the game), corner radius, EnterMargin
HERO_PEAK, ENTER_PEAK = 0.42, 0.30
GOLD = (226, 168, 66)


def strength(level):
    """How bright a level is, 0.7 to 1."""
    return 0.7 + 0.3 * (level - 1) / (LEVELS - 1)


def hero_alpha(x, y, w, h):
    r = math.hypot(x / w * 2 - 1, y / h * 2 - 1)
    return HERO_PEAK * math.exp(-(r / 0.55) ** 2) * (1 - r) if r < 1 else 0.0


def enter_alpha(x, y, w, h):
    bw, bh, radius, margin = ENTER
    qx, qy = abs(x - w / 2) - (bw / 2 - radius), abs(y - h / 2) - (bh / 2 - radius)
    d = math.hypot(max(qx, 0.0), max(qy, 0.0)) + min(max(qx, qy), 0.0) - radius  # < 0 under the button
    if d < -6:
        return 0.0
    return ENTER_PEAK * max(0.0, 1 - max(d, 0.0) / margin) ** 2


def picture(alpha, w, h, scale):
    """(width, height, one alpha per pixel)."""
    W, H = int(round(w * scale)), int(round(h * scale))
    return W, H, [alpha((x + 0.5) / scale, (y + 0.5) / scale, w, h) for y in range(H) for x in range(W)]


def pixels(alphas, level):
    out = bytearray()
    for a in alphas:
        out += bytes((*GOLD, int(a * strength(level) * 255 + 0.5))) if a > 0 else bytes(4)
    return bytes(out)


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
    bw, bh, _, margin = ENTER
    for suffix, scale in (("", 1.0), (".lowend", 0.5)):
        template = t.game_file(TEMPLATE.format(suffix))
        for name, alpha, w, h in (("heroglow", hero_alpha, *HERO), ("enterglow", enter_alpha, bw + 2 * margin, bh + 2 * margin)):
            W, H, alphas = picture(alpha, w, h, scale)
            for level in range(1, LEVELS + 1):
                t.add_file(OUT.format(f"{name}{level}", suffix), sprite(template, W, H, pixels(alphas, level)))
    print(f"  campfire glow: {2 * LEVELS} pictures, gold")
