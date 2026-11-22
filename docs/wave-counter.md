# Wave counter

While an arena runs, plain text near the top of the screen (under the enemy health bar) shows
which wave you are on, Brotato style:

- **Wave 2 / 5**: waves spawned so far out of the arena's total.
- **Next wave in 5 … 1**: shown during the last 5 seconds before each wave (red from 3).
- **Arena cleared**: for 5 seconds when the Town Portal scroll is given.

Leaving the arena or the game hides it.

## How it works

In a game the plugin can open and close its own panels, but it can't reach a panel's widgets to
hide or show them (`findPanel` gives Unavailable). So every piece of text is a tiny panel of its
own with one text box, opened or closed whole. `src/plugin/waveoverlay.cpp` makes their layouts at
load and registers them:

- `darkestdiablo/WaveK1` .. `WaveK8`: "Wave k", ending just left of the screen's centre,
- `darkestdiablo/WaveNext1` .. `WaveNext5`: "Next wave in s",
- `darkestdiablo/WaveN<n>`: "/ n", starting just right of the centre (together: "Wave 3 / 6"),
  only for the wave totals the arenas really have (the plugin passes them in at load; on main
  4, 6 and 8),
- `darkestdiablo/WaveCleared`: "Arena cleared".

On main that is 17 panels. A plugin may register at most 64, and the rest of the plugin uses
about 43 (the Campfire's panel pools alone take 38). The counter registers last and in that
order, the most common totals first; if the limit is reached it keeps what fit and the log
lists what was left out ("the plugin's panel limit left out: ..."). Arenas have at most 8 waves
today; for more, raise `MaxWaves` in `waveoverlay.h` and count the panels first.

A small timer thread checks five times a second what should be up and asks the UI thread to open
and close the right panels; once a second it also checks that the game hasn't closed one of them.

The arena code tells it what to show:

- `WaveOverlay::SetWaves(wave, total, nextWaveInMs)` when you enter an arena and after each wave,
- `WaveOverlay::Cleared()` when the arena is cleared,
- `WaveOverlay::Hide()` from `ResetArena()` (left the arena, left the game).

## What didn't work (D2RLoader 1.3.1)

- `OverlayService` (drawing text every frame), 0.15.0-0.15.1: accepted, but nothing on screen.
- Hidden widgets in the game's HUD layout (`hudpanelhd`), 0.15.2-0.15.4: `"visible": false` is
  ignored and the HUD panel can't be reached, so every text showed at once.
- One panel holding every text, 0.15.5: the panel opened, but its texts couldn't be hidden, so
  they all showed on top of each other.

## Testing

Console `dd-wavehud` plays a fake 5-wave run of the counter anywhere in a game (no monsters
spawn): wave 1 after a second, then one every 10 seconds, then "Arena cleared".
