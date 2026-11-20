# Wave counter

While an arena runs, the top of the screen shows which wave you are on, Brotato style:

- **Wave 2 / 5**: waves spawned so far out of the arena's total.
- **Next wave in 5 … 1**: shown during the last 5 seconds before each wave (red from 3).
  Before the first wave it reads "Starts in 1".
- **Final wave: clear the arena**: after the last wave has spawned.
- **Arena cleared**: for 5 seconds when the Town Portal scroll is given, then it fades out.

Leaving the arena or the game hides it.

## How it works

`src/plugin/waveoverlay.cpp` draws with D2RLoader's `OverlayService` (SDK `overlay.h`): every frame
the loader calls our draw function after the game's own UI, and we draw a dark box with gold and
white text. Because it is drawn fresh each frame, the numbers can change freely (layout widgets
can't change their text, see CLAUDE.md). Sizes are set for a 1080p screen and scale with the
window height. The text uses the loader's font, not Diablo's; the V1 service offers no other font
and no images.

The arena code tells it what to show:

- `WaveOverlay::SetWaves(wave, total, nextWaveInMs)` when you enter an arena and after each wave,
- `WaveOverlay::Cleared()` when the arena is cleared,
- `WaveOverlay::Hide()` from `ResetArena()` (left the arena, left the game).

If the loader has no overlay service, the mod runs as before without the counter (log line
"Wave counter: overlay service missing").

## Testing

Console `dd-wavehud` plays a fake 5-wave run of the counter anywhere (no monsters spawn): wave 1
after a second, then one every 10 seconds, then "Arena cleared".
