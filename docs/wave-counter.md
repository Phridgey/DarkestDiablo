# Wave counter

While an arena runs, the top of the screen shows which wave you are on, Brotato style:

- **Wave 2 / 5**: waves spawned so far out of the arena's total.
- **Next wave in 5 … 1**: shown during the last 5 seconds before each wave (red from 3).
- **Final wave: clear the arena**: after the last wave has spawned.
- **Arena cleared**: for 5 seconds when the Town Portal scroll is given.

Leaving the arena or the game hides it.

## How it works

The plugin can show and hide widgets but not change their text, so every text the counter can
show is a ready-made widget in a small panel of our own, `darkestdiablo/WaveCounter`, anchored at
the top centre of the screen. `src/plugin/waveoverlay.cpp` writes its layout at load (`Layout()`)
and registers it as a resource. Names: `DDWave<k>of<n>` (every k <= n <= 15), `DDNext1`..`DDNext5`,
`DDFinal`, `DDCleared`, and two dark plates `DDPlateShort` / `DDPlateTall`, in the game's own font
and tooltip frame. The panel has no click catcher, so the game under it keeps taking clicks.

A small timer thread checks five times a second what should be up and asks the UI thread to
update when that changes: it opens the panel (and hides all its texts, since widgets start
shown), shows the right ones, and closes the panel when nothing is to be shown.

The arena code tells it what to show:

- `WaveOverlay::SetWaves(wave, total, nextWaveInMs)` when you enter an arena and after each wave,
- `WaveOverlay::Cleared()` when the arena is cleared,
- `WaveOverlay::Hide()` from `ResetArena()` (left the arena, left the game).

An arena with more than 15 waves shows no counter: raise `MaxWaves` in `waveoverlay.h`.

## What didn't work (D2RLoader 1.3.1)

- `OverlayService` (drawing text every frame), 0.15.0-0.15.1: the loader called our drawing every
  frame and accepted every call, but nothing reached the screen.
- Hidden widgets added to the game's HUD layout (`hudpanelhd`), 0.15.2-0.15.4: the layout's
  `"visible": false` is ignored, and `findPanel("HUDPanel")` gives Unavailable, so they could
  never be hidden.

## Testing

Console `dd-wavehud` plays a fake 5-wave run of the counter anywhere in a game (no monsters
spawn): wave 1 after a second, then one every 10 seconds, then "Arena cleared".
