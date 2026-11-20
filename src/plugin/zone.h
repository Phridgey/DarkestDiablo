// The Tower zone (prototype): a red portal by the town fire leads to the Forgotten Tower's last
// floor (src/patches/120_linear_zone.py). Packs fight the hero on the way, an elite in each side
// room drops a Tower seal, and with both seals the altar in the last room brings out Blood Raven.
// Reference: docs/linear-zone.md.
#pragma once

#include <D2RLPlugin/api.h>

namespace Zone {

// Called once from D2RLoaderLoadPlugin. `giveTownPortal` queues the arenas' Town Portal scroll.
// Registers the console commands dd-zone, dd-zone-portal, dd-zone-boss and dd-zone-seals.
using GiveFn = void (*)(const D2RL::PluginContext* context) noexcept;
auto Load(const D2RL::PluginContext* context, GiveFn giveTownPortal) noexcept -> bool;

// From the plugin's gameplay events.
void OnLevelChanged(const D2RL::PluginContext* context, int levelId) noexcept;
void OnGameLeft() noexcept;

}  // namespace Zone
