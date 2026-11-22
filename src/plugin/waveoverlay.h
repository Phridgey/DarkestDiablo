#pragma once
// The wave counter (waveoverlay.cpp): "Wave 2 / 5" near the top of the screen while an arena
// runs, "Next wave in 5" during the last seconds before each wave, and "Arena cleared" at the
// end. Plain text, no frame.
//
// In a game the plugin can open and close its own panels but can't reach their widgets
// (WidgetService findPanel gives Unavailable, 0.15.5), so a widget can't be hidden there. So each
// piece of text is a small panel of its own, opened or closed whole: "Wave k" (k = 1..MaxWaves),
// "Next wave in s" (s = 1..5), "Arena cleared", and "/ n" only for the wave totals the arenas
// really have (main: 4, 6, 8, so 17 panels). A plugin may register at most 64 panels and the rest
// of the plugin uses about 43, so if the limit is hit the counter registers what fits, most
// needed first, and the log says what was left out.
//
// All functions are safe from any thread; panels are only opened and closed on the UI thread.

#include <D2RLPlugin/api.h>
#include <cstdint>

namespace WaveOverlay {

inline constexpr int CountdownSeconds = 5;  // the countdown shows during the last 5 s before a wave
inline constexpr int MaxWaves         = 8;  // the most waves the counter can show (Tree of Inifuss has 8)

// Called once from D2RLoaderLoadPlugin with the arenas' wave totals (any order, repeats allowed;
// the most common get their "/ n" first). Without the panel, resource or thread services the mod
// runs as before, just without the counter (always returns true).
auto Load(const D2RL::PluginContext* context, const int* totals, int count) noexcept -> bool;

// The hero is in a game: the counter's panels are checked again (the game closes panels when it
// loads a game).
void OnPlayerReady() noexcept;

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
