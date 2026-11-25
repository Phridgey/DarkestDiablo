// Darkest Diablo — the caravan's stash in the game (stash.cpp). Rules: stash.h, docs/stash.md.
#pragma once

#include <D2RLPlugin/api.h>

namespace StashGame {

// Call once from D2RLoaderLoadPlugin, after Campfire::Load and before the wave counter (which takes
// whatever panels are left). Returns false only if the console command could not be added.
auto Load(const D2RL::PluginContext* context) noexcept -> bool;

// Give every UI message the Campfire did not take to this. Returns true when the game must not see it.
auto OnUiMessage(const D2RL::PluginContext* context, const char* target, const char* command, const char* text) noexcept -> bool;

// Give LevelChanged and GameLeft to this.
void OnGameplayEvent(const D2RL::PluginContext* context, const D2RL::Lifecycle::GameplayEvent* event) noexcept;

// A stop was cleared and saved in the hero's route progress: an act may be done.
void OnStopCleared(const D2RL::PluginContext* context) noexcept;

// True when items can be put straight on the shared stash pages.
auto CanPutOnSharedPages() noexcept -> bool;

// The save's first page in the stash file (from 0). Right once a game's save file was looked at.
auto FirstSharedPage() noexcept -> uint32_t;

// True while the plugin itself turns stash pages (its messages are not worth logging).
auto TurningPages() noexcept -> bool;

}
