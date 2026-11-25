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

// Adds to a stat held in 256ths (life, mana, stamina) in every copy that has it, never past the copy's
// own `maxStat` and never lowering it: `add256` 256ths plus `share` of the maximum. Each copy's own value
// is read and written back at once. A copy without life left (dead) keeps none. Returns how many copies
// have the stat.
auto AddStat(const std::vector<uintptr_t>& copies, uint16_t stat, uint16_t maxStat, int add256, double share) -> int;

// True while the unit at `copy` is still the same one (kind, class/row, unit number).
auto SameUnit(uintptr_t copy, const Game::Unit& unit) -> bool;

// Changes a state's end frame (longer or shorter buffs/debuffs) and its own stat values.
auto SetStateEnd(const std::vector<uintptr_t>& copies, int state, uint32_t endFrame) -> int;
auto SetStateStat(const std::vector<uintptr_t>& copies, int state, uint16_t stat, int value) -> int;

// Puts the right-button skill back where it became ours (agreed 2026-11-25, CLAUDE.md); a skill the
// player picked meanwhile stays.
struct RightSkill { uintptr_t copy = 0; int before = -1, ours = -1; };  // skills.txt rows
void RestoreRight(const std::vector<RightSkill>& saved);

// The game's skill keys (agreed 2026-11-25 with SelectRight): the table of what each skill key holds, 32
// records of 28 bytes, as the hero's .ctl file has it. Found by that shape (slow: scans memory, 1-2 s); more than 4
// places that fit: none, to be safe. HoldSkill puts a skill on every key of every table and returns
// what was there; GiveBack puts it back.
struct SkillKeyTable { uintptr_t at = 0; std::vector<unsigned char> before {}; };
auto FindSkillKeys() -> std::vector<uintptr_t>;
auto HoldSkill(const std::vector<uintptr_t>& tables, int skill) -> std::vector<SkillKeyTable>;
void GiveBack(const std::vector<SkillKeyTable>& tables, int skill);

// One line per copy for the log: where it is, its stat list, life (to tell the copies apart).
auto CopiesReport(const Game::Unit& unit, const std::vector<uintptr_t>& copies) -> std::string;

}  // namespace GameWrite
