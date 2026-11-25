// Darkest Diablo — Ascend in the game: the arena, the boss, its loot, the portal (ascension.cpp).
// docs/ascend.md. The Campfire starts it and ends it; legacy.h carries the items home.
#pragma once

#include <D2RLPlugin/api.h>

#include <string>

namespace Ascension {

// Call once from D2RLoaderLoadPlugin.
auto Load(const D2RL::PluginContext* context) noexcept -> bool;

// The Campfire is about to load this hero to ascend, in this act. An empty name calls it off.
void Plan(const std::string& hero, int act) noexcept;

// A hero is on its way to ascend.
auto Planned() noexcept -> bool;

// The hero is in the game.
void OnReady(const D2RL::PluginContext* context, const std::string& hero) noexcept;
void OnLevelChanged(const D2RL::PluginContext* context, int levelId) noexcept;
void OnGameLeft() noexcept;

// Every monster's death (monstats row), so a boss whose body is gone at once counts too.
void OnMonsterDied(const D2RL::PluginContext* context, uint32_t classId) noexcept;

// A button of the portal's question ("ascend-yes", "ascend-no"). True when it was one.
auto OnAction(const D2RL::PluginContext* context, const char* action) noexcept -> bool;

// The level a hero should have for an act's boss: below it the Campfire warns.
auto AdvisedLevel(int act) noexcept -> int;

// In the arena, from the first step to leaving the game.
auto Active() noexcept -> bool;

}
