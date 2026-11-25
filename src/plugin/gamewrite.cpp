// See gamewrite.h. The only place in the plugin that writes to game memory.
#include "gamewrite.h"

#include "gamememory.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

namespace GameWrite {
namespace {

constexpr std::size_t UnitStatList = 0x88;  // same offsets as gamestate.cpp / gameapi.cpp
constexpr std::size_t ListUnit     = 0x08;
constexpr std::size_t ListState    = 0x1C;
constexpr std::size_t ListEndFrame = 0x20;
constexpr std::size_t ListStats    = 0x30;  // pointer, count
constexpr std::size_t StatSize     = 16;    // param u32, stat u32, value i32, pad

template <typename T>
auto Get(uintptr_t address, T& out) -> bool {
	return GameState::ReadBytes(address, &out, sizeof(T));
}

auto LooksPointer(uint64_t v) -> bool {
	return v >= 0x10000 && v < 0x7FFFFFFF0000ULL && (v & 7) == 0;
}

// Writes only into committed, writable, private memory (never code or files), and only on the
// frozen build.
template <typename T>
auto Put(uintptr_t address, const T& value) -> bool {
	if (!GameMemory::BuildMatches()) {
		return false;
	}
	MEMORY_BASIC_INFORMATION info {};
	if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) == 0 || info.State != MEM_COMMIT || info.Type != MEM_PRIVATE ||
	    (info.Protect & PAGE_READWRITE) == 0 || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
	    address + sizeof(T) > reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize) {
		return false;
	}
	SIZE_T put = 0;
	return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(address), &value, sizeof(T), &put) && put == sizeof(T);
}

// Every stats array (pointer + count pair) in the first 0x100 bytes of a stat list.
template <typename F>
void ForEachArray(uintptr_t list, F&& array) {
	uint64_t head[0x100 / 8] {};
	if (!LooksPointer(list) || !Get(list, head)) {
		return;
	}
	for (std::size_t i = 0; i + 1 < std::size(head); ++i) {
		if (LooksPointer(head[i]) && head[i + 1] > 0 && head[i + 1] <= 512) {
			array(static_cast<uintptr_t>(head[i]), static_cast<std::size_t>(head[i + 1]));
		}
	}
}

auto SetInArray(uintptr_t at, std::size_t count, uint16_t stat, uint16_t param, int32_t stored) -> int {
	int changed = 0;
	for (std::size_t i = 0; i < count; ++i) {
		uint32_t p = 0, id = 0;
		if (Get(at + i * StatSize, p) && Get(at + i * StatSize + 4, id) && id == stat && p == param &&
		    Put<int32_t>(at + i * StatSize + 8, stored)) {
			++changed;
		}
	}
	return changed;
}

// The state lists of a copy: blocks reachable from its stat list that point back at it.
auto StateLists(uintptr_t unit) -> std::vector<uintptr_t> {
	std::vector<uintptr_t> found;
	uintptr_t main = 0;
	if (!Get(unit + UnitStatList, main) || !LooksPointer(main)) {
		return found;
	}
	std::set<uintptr_t> seen { main };
	std::vector<uintptr_t> todo { main };
	for (int hop = 0; hop < 3 && !todo.empty() && found.size() < 64; ++hop) {
		std::vector<uintptr_t> next;
		for (const uintptr_t block : todo) {
			uint64_t words[0x100 / 8] {};
			if (!Get(block, words)) {
				continue;
			}
			for (std::size_t i = 7; i < std::size(words); ++i) {
				const uintptr_t p = static_cast<uintptr_t>(words[i]);
				uintptr_t owner = 0;
				uint32_t state = 0;
				if (LooksPointer(p) && seen.insert(p).second && Get(p + ListUnit, owner) && owner == unit && Get(p + ListState, state) && state > 0 && state < 1200) {
					found.push_back(p);
					next.push_back(p);
				}
			}
		}
		todo = std::move(next);
	}
	return found;
}

}  // namespace

auto Copies(const Game::Unit& unit) -> std::vector<uintptr_t> {
	std::vector<uintptr_t> out;
	if (!unit) {
		return out;
	}
	out.push_back(unit.address);
	const uint32_t want[3] = { static_cast<uint32_t>(unit.kind), unit.row, unit.id };
	constexpr SIZE_T Step = 4 * 1024 * 1024;
	std::vector<unsigned char> buffer(Step + 16);
	MEMORY_BASIC_INFORMATION info {};
	for (uintptr_t at = 0x10000; at < 0x7FFFFFFF0000ULL && out.size() < 16;) {
		if (VirtualQuery(reinterpret_cast<LPCVOID>(at), &info, sizeof(info)) == 0) {
			break;
		}
		const uintptr_t start = reinterpret_cast<uintptr_t>(info.BaseAddress), end = start + info.RegionSize;
		if (info.State == MEM_COMMIT && info.Type == MEM_PRIVATE && (info.Protect & PAGE_READWRITE) != 0 &&
		    (info.Protect & (PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE)) == 0) {
			for (uintptr_t block = start; block < end; block += Step) {
				const SIZE_T size = static_cast<SIZE_T>(std::min<uintptr_t>(Step, end - block));
				SIZE_T got = 0;
				if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(block), buffer.data(), size, &got) || got < 16) {
					continue;
				}
				for (SIZE_T i = 0; i + 12 <= got; i += 8) {  // units are 8-byte aligned
					if (std::memcmp(buffer.data() + i, want, sizeof(want)) != 0) {
						continue;
					}
					const uintptr_t found = block + i;
					uintptr_t stats = 0;
					// A player's class and number are small and turn up all over memory, so a copy must also
					// hold the player's name (the first thing in its player data).
					if (unit.kind == Game::Kind::Player && !unit.name.empty()) {
						uint64_t data = 0;
						char name[64] {};
						if (!Get(found + 0x10, data) || !LooksPointer(data) || !Get(static_cast<uintptr_t>(data), name) ||
						    std::strncmp(name, unit.name.c_str(), unit.name.size() + 1) != 0) {
							continue;
						}
					}
					if (found != unit.address && Get(found + UnitStatList, stats) && LooksPointer(stats)) {
						out.push_back(found);
					}
				}
			}
		}
		at = end > at ? end : at + 0x1000;
	}
	return out;
}

auto SetStat(const std::vector<uintptr_t>& copies, uint16_t stat, int value, uint16_t param) -> int {
	const bool fixedPoint = stat >= Game::StatId::Life && stat <= Game::StatId::MaxStamina;
	const int32_t stored = fixedPoint ? value * 256 : value;
	int changed = 0;
	for (const uintptr_t unit : copies) {
		uintptr_t list = 0;
		if (Get(unit + UnitStatList, list)) {
			ForEachArray(list, [&](uintptr_t at, std::size_t count) { changed += SetInArray(at, count, stat, param, stored); });
		}
	}
	return changed;
}

auto SameUnit(uintptr_t copy, const Game::Unit& unit) -> bool {
	uint32_t head[3] {};
	return unit && Get(copy, head) && head[0] == static_cast<uint32_t>(unit.kind) && head[1] == unit.row && head[2] == unit.id;
}

auto AddStat(const std::vector<uintptr_t>& copies, uint16_t stat, uint16_t maxStat, int add256, double share) -> int {
	struct Entry { uintptr_t at; int32_t value; };
	int found = 0;
	for (const uintptr_t unit : copies) {
		uintptr_t list = 0;
		if (!Get(unit + UnitStatList, list)) {
			continue;
		}
		std::vector<Entry> entries;
		int32_t maximum = 0;  // the total with items is the largest one
		ForEachArray(list, [&](uintptr_t at, std::size_t count) {
			for (std::size_t i = 0; i < count; ++i) {
				uint32_t p = 0, id = 0;
				int32_t v = 0;
				if (!Get(at + i * StatSize, p) || !Get(at + i * StatSize + 4, id) || !Get(at + i * StatSize + 8, v) || p != 0) {
					continue;
				}
				if (id == stat) {
					entries.push_back({ at + i * StatSize + 8, v });
				} else if (id == maxStat) {
					maximum = (std::max)(maximum, v);
				}
			}
		});
		// Nothing is written where the numbers don't look like a hero's.
		if (entries.empty() || maximum < 256 || maximum > 256 * 1000000) {
			continue;
		}
		++found;
		const int32_t gain = add256 + static_cast<int32_t>(maximum * share);
		for (const auto& entry : entries) {
			if (entry.value >= (stat == Game::StatId::Life ? 1 : 0) && entry.value < maximum) {
				Put<int32_t>(entry.at, (std::min)(maximum, entry.value + gain));
			}
		}
	}
	return found;
}

auto SetStateEnd(const std::vector<uintptr_t>& copies, int state, uint32_t endFrame) -> int {
	int changed = 0;
	for (const uintptr_t unit : copies) {
		for (const uintptr_t list : StateLists(unit)) {
			uint32_t s = 0;
			if (Get(list + ListState, s) && static_cast<int>(s) == state && Put<uint32_t>(list + ListEndFrame, endFrame)) {
				++changed;
			}
		}
	}
	return changed;
}

auto SetStateStat(const std::vector<uintptr_t>& copies, int state, uint16_t stat, int value) -> int {
	int changed = 0;
	for (const uintptr_t unit : copies) {
		for (const uintptr_t list : StateLists(unit)) {
			uint32_t s = 0;
			uint64_t at = 0, count = 0;
			if (Get(list + ListState, s) && static_cast<int>(s) == state && Get(list + ListStats, at) && Get(list + ListStats + 8, count) &&
			    LooksPointer(at) && count > 0 && count <= 64) {
				changed += SetInArray(static_cast<uintptr_t>(at), static_cast<std::size_t>(count), stat, 0, value);
			}
		}
	}
	return changed;
}

auto SelectRight(const std::vector<uintptr_t>& copies, int skill, std::vector<RightSkill>& saved) -> bool {
	struct Change { uintptr_t at, to; RightSkill undo; };
	std::vector<Change> changes;
	for (const uintptr_t copy : copies) {
		const auto places = Game::SkillsOf(copy);
		const uintptr_t to = places ? places->Find(skill) : 0;
		int before = -1;
		if (to != 0) {
			for (const auto& [id, node] : places->skills) {
				before = node == places->right ? id : before;
			}
		}
		if (to == 0 || before < 0) {
			return false;
		}
		changes.push_back({ places->rightAt, to, { copy, before, skill } });
	}
	if (changes.size() < 2) {
		return false;
	}
	saved.clear();
	for (const auto& change : changes) {
		if (!Put<uint64_t>(change.at, change.to)) {
			RestoreRight(saved);
			saved.clear();
			return false;
		}
		saved.push_back(change.undo);
	}
	return true;
}

void RestoreRight(const std::vector<RightSkill>& saved) {
	for (const auto& s : saved) {
		const auto places = Game::SkillsOf(s.copy);
		const uintptr_t to = places ? places->Find(s.before) : 0;
		if (to != 0 && places->right == places->Find(s.ours)) {
			Put<uint64_t>(places->rightAt, to);
		}
	}
}

auto CopiesReport(const Game::Unit& unit, const std::vector<uintptr_t>& copies) -> std::string {
	std::string text = std::to_string(copies.size()) + " copies of unit #" + std::to_string(unit.id);
	for (const uintptr_t at : copies) {
		uintptr_t list = 0, path = 0;
		Get(at + UnitStatList, list);
		Get(at + 0x38, path);
		const Game::Unit u = GameState::At(at, unit.kind);
		char line[200];
		std::snprintf(line, sizeof(line), "; %llx (stats %llx, path %llx, life %d, at %d,%d, %zu states)", static_cast<unsigned long long>(at),
			static_cast<unsigned long long>(list), static_cast<unsigned long long>(path), u ? GameState::Life(u) : -1, u.x, u.y, StateLists(at).size());
		text += line;
	}
	return text;
}

}  // namespace GameWrite
