#pragma once
// Mercenary full gear (mercgear.cpp): lets a mercenary wear an amulet, two rings, gloves, a belt
// and boots. The one change to the game's code the plugin makes (agreed by both of us, 2026-11-23;
// CLAUDE.md, docs/game-memory.md).

#include <D2RLPlugin/api.h>

namespace MercGear {

// Called once from D2RLoaderLoadPlugin, with the "merc" item type's number from loot_data.txt
// (997_merc_gear_type.py; -1 when missing). Returns true when the game now accepts the gear.
auto Install(const D2RL::PluginContext* context, int mercType) noexcept -> bool;

}
