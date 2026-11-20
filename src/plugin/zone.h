// The linear zone (prototype): a red portal by the town fire leads into a short cave
// (src/patches/120_linear_zone.py) that the hero crosses from west to east, through the game's
// own random monster packs. Reaching the last room brings out an elite (Bishibosh); his death
// gives a Town Portal scroll. Reference: docs/linear-zone.md.
#pragma once

#include <D2RLPlugin/api.h>

namespace Zone {

// Called once from D2RLoaderLoadPlugin. `giveTownPortal` queues the arenas' Town Portal scroll.
// Registers the console commands dd-zone, dd-zone-portal and dd-zone-boss.
using GiveFn = void (*)(const D2RL::PluginContext* context) noexcept;
auto Load(const D2RL::PluginContext* context, GiveFn giveTownPortal) noexcept -> bool;

// From the plugin's gameplay events.
void OnLevelChanged(const D2RL::PluginContext* context, int levelId) noexcept;
void OnGameLeft() noexcept;

}  // namespace Zone
