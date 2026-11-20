#pragma once
// The wave counter at the top of the screen (waveoverlay.cpp): "Wave 2 / 5" while an arena runs,
// a "Next wave in 5" countdown during the last seconds before each wave, and "Arena cleared" at
// the end. It draws with D2RLoader's OverlayService (text and boxes over the game's own UI), so it
// changes every frame without one layout per number. Display only: it takes no clicks or keys.
//
// All functions are safe from any thread; the drawing itself runs on the loader's render thread.

#include <D2RLPlugin/api.h>
#include <cstdint>

namespace WaveOverlay {

inline constexpr int CountdownSeconds = 5;  // the countdown shows during the last 5 s before a wave

// Called once from D2RLoaderLoadPlugin. Returns false only if the overlay service is there but
// refused the callback; without the service the mod runs as before, just without the counter.
auto Load(const D2RL::PluginContext* context) noexcept -> bool;

// `wave` of `total` waves have spawned (0 = none yet). `nextWaveInMs` is the time until the next
// wave spawns, or -1 if no more waves come.
void SetWaves(int wave, int total, int nextWaveInMs) noexcept;

// The arena is cleared: shows "Arena cleared" for a few seconds, then hides.
void Cleared() noexcept;

// Hides the counter (left the arena or the game).
void Hide() noexcept;

// Test: runs a fake 5-wave arena on screen (no monsters), 10 s per wave. Console `dd-wavehud`.
void Demo() noexcept;

}
