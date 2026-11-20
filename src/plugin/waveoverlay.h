#pragma once
// The wave counter at the top of the screen (waveoverlay.cpp): "Wave 2 / 5" while an arena runs,
// a "Next wave in 5" countdown during the last seconds before each wave, and "Arena cleared" at
// the end. Every text is a ready-made widget in a small panel of our own at the top centre
// (darkestdiablo/WaveCounter, made in waveoverlay.cpp); the plugin shows the right ones.
// (D2RLoader 1.3.1's OverlayService took our drawing without errors but nothing reached the
// screen, 0.15.1; the game's HUD panel can't be reached by WidgetService, 0.15.4.)
//
// All functions are safe from any thread; widgets are only touched on the UI thread.

#include <D2RLPlugin/api.h>
#include <cstdint>

namespace WaveOverlay {

inline constexpr int CountdownSeconds = 5;   // the countdown shows during the last 5 s before a wave
inline constexpr int MaxWaves         = 15;  // the most waves the counter has texts for

// Called once from D2RLoaderLoadPlugin. Without the widget or thread services the mod runs as
// before, just without the counter (always returns true).
auto Load(const D2RL::PluginContext* context) noexcept -> bool;

// The hero is in a game: the panel is closed and reopened fresh when next needed.
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
