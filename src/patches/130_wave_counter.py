"""The wave counter at the top of the screen (src/plugin/waveoverlay.cpp, docs/wave-counter.md).

The plugin can show and hide widgets but not change their text, so every text the counter can
show is a ready-made widget in the game's own HUD layout (always open in a game), hidden until
the plugin shows the right ones: "Wave k / n" for every k <= n <= MAX_WAVES, "Next wave in s"
for s = 1..5, "Final wave: clear the arena" and "Arena cleared", over a dark plate.

The HUD panel sits at the bottom of the screen (its top is 412 units above the bottom of a
2160-high screen), so the counter's box is placed far above it, at the top centre.

The widgets are direct children of the HUD panel, not inside a box of their own: the plugin's
widget search only finds a stock panel's direct children (0.15.2 put them in a box, and the
plugin could not hide them). The layout's "visible" field is ignored, so the plugin hides them
all when a game starts.
"""
MAX_WAVES = 15        # the most waves an arena may have (WaveOverlay::MaxWaves in waveoverlay.h)
COUNTDOWN = 5         # WaveOverlay::CountdownSeconds
SCREEN_H = 2160       # layout units
HUD_TOP = 412         # hudpanelhd: anchor y 1, rect y -412
HUD_CENTRE_X = 1454   # hudpanelhd: rect x -1454 from the screen's centre
TOP_MARGIN = 40

STYLE = {"alignment": {"h": "center", "v": "center"}, "dropShadow": "$DefaultDropShadow"}


TOP = -(SCREEN_H - HUD_TOP) + TOP_MARGIN  # the counter's top edge, in the HUD panel's units


def text(name, y, height, value, color, size):
    style = dict(STYLE, fontColor=color, pointSize=size)
    return {"type": "TextBoxWidget", "name": name,
            "fields": {"rect": {"x": HUD_CENTRE_X - 700, "y": TOP + y, "width": 1400, "height": height},
                       "text": value, "style": style}}


def plate(name, height):
    return {"type": "NineTileImageWidget", "name": name,
            "fields": {"filename": "PANEL/TooltipBorder/TooltipBorder", "backgroundColor": [0.0, 0.0, 0.0, 0.7],
                       "tileScale": 1.0, "rect": {"x": HUD_CENTRE_X - 340, "y": TOP - 12, "width": 680, "height": height}}}


def patch(t):
    hud = t.layout("hudpanelhd")
    if hud.get("name") != "HUDPanel":
        raise RuntimeError("hudpanelhd: root is not HUDPanel; has the game changed?")
    if t.widget(hud, "DDCleared") is not None:
        return  # already added (the patch runs twice: once more for global/excel/base)
    kids = [plate("DDPlateShort", 124), plate("DDPlateTall", 196)]
    for n in range(1, MAX_WAVES + 1):
        for k in range(1, n + 1):
            kids.append(text(f"DDWave{k}of{n}", 0, 100, f"Wave {k} / {n}", "$FontColorGoldYellow", "$ExtraLargeFontSize"))
    for s in range(1, COUNTDOWN + 1):
        kids.append(text(f"DDNext{s}", 96, 76, f"Next wave in {s}", "$FontColorRed" if s <= 3 else "$FontColorWhite", "$LargeFontSize"))
    kids.append(text("DDFinal", 96, 76, "Final wave: clear the arena", "$FontColorWhite", "$LargeFontSize"))
    kids.append(text("DDCleared", 0, 100, "Arena cleared", "$FontColorGoldYellow", "$ExtraLargeFontSize"))
    hud.setdefault("children", []).extend(kids)
    print(f"  wave counter: {len(kids)} widgets in the HUD")
