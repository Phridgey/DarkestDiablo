# Wave counter

While an arena runs, the top of the screen shows which wave you are on, Brotato style:

- **Wave 2 / 5**: waves spawned so far out of the arena's total.
- **Next wave in 5 … 1**: shown during the last 5 seconds before each wave (red from 3).
- **Final wave: clear the arena**: after the last wave has spawned.
- **Arena cleared**: for 5 seconds when the Town Portal scroll is given.

Leaving the arena or the game hides it.

## How it works

The plugin can show and hide widgets but not change their text, so every text the counter can
show is a ready-made widget: `src/patches/130_wave_counter.py` adds them, hidden, to the game's own
HUD layout (`hudpanelhd.json`, panel `HUDPanel`, which is open whenever you are in a game), in a
box `DDWaveCounter` at the top centre of the screen. Names: `DDWave<k>of<n>` (every k <= n <= 15),
`DDNext1`..`DDNext5`, `DDFinal`, `DDCleared`, and two dark plates `DDPlateShort` / `DDPlateTall`.
They use the game's own font and tooltip frame.

`src/plugin/waveoverlay.cpp` decides what should be up; a small timer thread checks five times a
second and asks the UI thread to show and hide the right widgets when that changes (and once a
second re-shows them, in case the game remade the HUD). When the hero enters a game every
counter widget is hidden once.

The arena code tells it what to show:

- `WaveOverlay::SetWaves(wave, total, nextWaveInMs)` when you enter an arena and after each wave,
- `WaveOverlay::Cleared()` when the arena is cleared,
- `WaveOverlay::Hide()` from `ResetArena()` (left the arena, left the game).

An arena with more than 15 waves shows no counter: raise `MAX_WAVES` in the patch and `MaxWaves`
in `waveoverlay.h` together.

D2RLoader's `OverlayService` (drawing text every frame) was tried first (0.15.0, 0.15.1): the
loader called our drawing every frame and accepted every call, but nothing reached the screen.

## Testing

Console `dd-wavehud` plays a fake 5-wave run of the counter anywhere in a game (no monsters
spawn): wave 1 after a second, then one every 10 seconds, then "Arena cleared".
