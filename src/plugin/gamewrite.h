// Game-state WRITES: changing values in the running game (movement speed, buffs, debuffs, magic
// find...). Agreed by both of us on 2026-11-19 (CLAUDE.md): this is the only file that writes,
// and it is off on any build other than D2R_VERSION. Reference: docs/game-state-api.md, "Changing values".
//
// What a write can and can't do (the safe subset):
//   - It changes a stat entry the unit already has (e.g. velocitypercent while slowed, or magic
//     find when an item gives some). It never adds entries: that needs the game's own memory
//     allocator, and a wrong size would corrupt the game.
//   - The game keeps more than one copy of a player: the one the screen draws (the unit list the
//     API reads) and the game rules' own copy. Writes go to every copy found (Copies()), or the
//     rules' copy ignores them. The game also recalculates totals when items or states change, so
//     a feature that wants a value to stay must write it again on a timer.
#pragma once

#include "gameapi.h"

#include <cstdint>
#include <string>
#include <vector>

namespace GameWrite {

// Every copy of this unit in memory (same kind, class/row and unit number), the given one first.
// Slow (scans memory, about 1-2 s): call once per game and keep the result while the unit lives.
auto Copies(const Game::Unit& unit) -> std::vector<uintptr_t>;

// Sets stat `stat` (param) to `value` in every stat array of every copy that already holds it
// (own and with-items totals). Life, mana and stamina are whole numbers (stored as 256ths).
// Returns how many entries were changed (0: the unit doesn't have this stat now).
auto SetStat(const std::vector<uintptr_t>& copies, uint16_t stat, int value, uint16_t param = 0) -> int;

// Changes a state's end frame (longer or shorter buffs/debuffs) and its own stat values.
auto SetStateEnd(const std::vector<uintptr_t>& copies, int state, uint32_t endFrame) -> int;
auto SetStateStat(const std::vector<uintptr_t>& copies, int state, uint16_t stat, int value) -> int;

// One line per copy for the log: where it is, its stat list, life (to tell the copies apart).
auto CopiesReport(const Game::Unit& unit, const std::vector<uintptr_t>& copies) -> std::string;

}  // namespace GameWrite
