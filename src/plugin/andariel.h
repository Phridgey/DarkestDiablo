// Andariel (the act's last stop): Catacombs level 4 on Blizzard's own map of her lair, built like
// Sam's Tower zone (zone.h). Packs come out as the hero nears them; two unique monsters, far
// apart, each drop one of her two keys; approaching her hall with both brings Andariel out. Her
// death clears the stop through the arenas' own rules (darkestdiablo_plugin.cpp, "clearOnKill").
// Reference: docs/design.md, src/patches/125_andariel.py.
#pragma once

#include <D2RLPlugin/api.h>

namespace Andariel {

// Called once from D2RLoaderLoadPlugin. Registers the console commands dd-andy and dd-andy-boss.
auto Load(const D2RL::PluginContext* context) noexcept -> bool;

// From the plugin's gameplay events. `open`: the hero may fight here now (the route allows the
// stop); a hero who may not gets an empty lair.
void OnLevelChanged(const D2RL::PluginContext* context, int levelId, bool open) noexcept;
void OnGameLeft() noexcept;

}  // namespace Andariel
