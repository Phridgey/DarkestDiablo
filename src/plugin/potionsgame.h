// Darkest Diablo — the permanent potions in the game (potions.cpp). Rules: potions.h, docs/potions.md.
#pragma once

#include <D2RLPlugin/api.h>

#include <string>

namespace PotionsGame {

// Call once from D2RLoaderLoadPlugin, after StashGame::Load and before the wave counter (which takes
// whatever panels are left). Returns false only if the console command could not be added.
auto Load(const D2RL::PluginContext* context) noexcept -> bool;

// Give every UI message the others did not take to this. Returns true when the game must not see it.
auto OnUiMessage(const D2RL::PluginContext* context, const char* target, const char* command, const char* text) noexcept -> bool;

// Give LocalPlayerReady and GameLeft to this, after the Campfire.
void OnGameplayEvent(const D2RL::PluginContext* context, const D2RL::Lifecycle::GameplayEvent* event) noexcept;

// Server thread: a monster died.
void OnMonsterDeath(const D2RL::Lifecycle::MonsterDeathEvent* event) noexcept;

// Throwing, from loot_data.txt (131a_throw_skill.py): "throwskill" the skill's row; "throwmissile" a
// potion's missile ("none": what the skill sends off otherwise); "throwref" another skill's row and missile.
void SetThrowData(const std::string& kind, const std::string& name, int number);

// A stop was cleared and counted.
void OnStopCleared(const D2RL::PluginContext* context, const char* stop) noexcept;

// Fills every potion (GM window).
void Refill(const D2RL::PluginContext* context) noexcept;

// A route map page with the hero's affixes drawn in, and the console's line about them ("" none).
auto RoutePage(const std::string& page, int act) -> std::string;
auto RouteLine(int act) -> std::string;

}
