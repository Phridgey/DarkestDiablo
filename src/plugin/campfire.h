// Darkest Diablo — the Campfire: save slots, the hero roster, recruiting, permanent death.
// The rules live in camp.h; this is the part that talks to the game. See docs/campfire.md.
#pragma once

#include <D2RLPlugin/api.h>

#include <filesystem>

namespace Campfire {

// The mod version, shown in the log, the console and the corner of the menus. One place only.
inline constexpr char Version[] = "0.17.37";

// Call once from D2RLoaderLoadPlugin. Returns false only if a console command could not be added;
// missing optional services just switch parts of the Campfire off (see the log).
auto Load(const D2RL::PluginContext* context) noexcept -> bool;

// Give every UI message to this first. Returns true when the game must not see the message.
auto OnUiMessage(const D2RL::PluginContext* context, const char* target, const char* command, const char* text) noexcept -> bool;

// Give GameJoined, LocalPlayerReady, LevelChanged, PlayerLevelChanged, PlayerResurrected and
// GameLeft to this.
void OnGameplayEvent(const D2RL::PluginContext* context, const D2RL::Lifecycle::GameplayEvent* event) noexcept;

// The game's difficulty (0 Normal, 1 Nightmare, 2 Hell) as seen in game, e.g. from a monster's
// death. The save remembers the hardest one played, for the save slot screen.
void NoteDifficulty(const D2RL::PluginContext* context, uint32_t difficulty) noexcept;

// The route progress file (route.h) of the hero in the current game. A game not started from the
// Campfire uses "_unknown". Empty when the save folder isn't known.
auto RouteProgressFile() noexcept -> std::filesystem::path;

}
