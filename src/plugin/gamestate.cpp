// See gamestate.h and docs/game-memory.md. READ ONLY.
#include "gamestate.h"

#include "gamememory.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <set>

namespace GameState {
namespace {

// Unit layout (the community's map of the D2R unit structure, as used by MapAssist and D2RMH;
// the first rows were checked in game for 0.11.34, see docs/game-memory.md).
constexpr std::size_t UnitType   = 0x00;  // uint32: the kind
constexpr std::size_t UnitData   = 0x10;  // pointer: players' PlayerData starts with the name
constexpr std::size_t UnitPath   = 0x38;  // pointer: where the unit stands
constexpr std::size_t UnitStats  = 0x88;  // pointer: the unit's stat list
constexpr std::size_t DynamicX   = 0x02;  // uint16 (players, monsters, missiles), sub-tiles
constexpr std::size_t DynamicY   = 0x06;
constexpr std::size_t DynamicRoom = 0x20; // pointer: the room the unit is in
constexpr std::size_t StaticRoom = 0x00;  // (objects, items, tiles)
constexpr std::size_t StaticX    = 0x10;  // uint32, sub-tiles
constexpr std::size_t StaticY    = 0x14;
constexpr std::size_t RoomOuter  = 0x18;  // room -> the bigger room record
constexpr std::size_t OuterLevel = 0x90;  // bigger room -> its level
constexpr std::size_t LevelId    = 0x1F8; // uint32: levels.txt Id
constexpr uint16_t    LifeStat   = 6;     // itemstatcost.txt: hitpoints, in 256ths (7: maximum)

constexpr uintptr_t   Slots      = 128;   // per kind: 128 chains, picked by the unit's number
constexpr uint32_t    Kinds      = 6;
constexpr std::size_t NextTries[] = { 0x150, 0x158 };  // where a unit keeps the next of its chain

// Where the slow search found the list on our own PCs (frozen build, D2RLoader 1.3.1-beta).
constexpr wchar_t     KnownModule[] = L"D2RLoader.exe";
constexpr uintptr_t   KnownOffset   = 0x2a23910;
constexpr std::size_t KnownNext     = 0x150;
constexpr unsigned    FrozenBuild   = 95032;

std::mutex  g_lock;
uintptr_t   g_list = 0;
std::size_t g_next = 0;
uint64_t    g_lastTry = 0;
std::string g_status = "the unit list was not looked for yet";

template <typename T>
auto Get(uintptr_t address, T& out) -> bool {
	SIZE_T got = 0;
	return address >= 0x10000 && address < 0x7FFFFFFF0000ULL &&
	       ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), &out, sizeof(T), &got) && got == sizeof(T);
}

auto Pointer(uintptr_t p) -> bool {
	return p >= 0x10000 && p < 0x7FFFFFFF0000ULL && (p & 7) == 0;
}

auto Plausible(long long x, long long y) -> bool {
	return x > 0 && y > 0 && x < 60000 && y < 60000;
}

auto Dynamic(Kind kind) -> bool {
	return kind == Kind::Player || kind == Kind::Monster || kind == Kind::Missile;
}

auto ReadUnit(uintptr_t address, Kind kind) -> Unit {
	uint32_t head[4] {};  // kind, row, id, mode
	if (!Get(address + UnitType, head) || head[0] != static_cast<uint32_t>(kind)) {
		return {};
	}
	Unit unit;
	unit.address = address;
	unit.kind = kind;
	unit.row = head[1];
	unit.id = head[2];
	unit.mode = head[3];
	uintptr_t path = 0;
	if (Get(address + UnitPath, path) && Pointer(path)) {
		if (Dynamic(kind)) {
			uint16_t x = 0, y = 0;
			unit.placed = Get(path + DynamicX, x) && Get(path + DynamicY, y) && Plausible(x, y);
			unit.x = x, unit.y = y;
		} else {
			uint32_t x = 0, y = 0;
			unit.placed = Get(path + StaticX, x) && Get(path + StaticY, y) && Plausible(x, y);
			unit.x = static_cast<int>(x), unit.y = static_cast<int>(y);
		}
		if (!unit.placed) {
			unit.x = unit.y = 0;
		}
	}
	if (kind == Kind::Player) {
		uintptr_t data = 0;
		char text[16] {};
		if (Get(address + UnitData, data) && Get(data, text)) {
			text[sizeof(text) - 1] = 0;
			unit.name = text;
		}
	}
	return unit;
}

// Calls `seen(address)` for every unit of one kind in the list at `list`. Stops after `limit`.
template <typename F>
void Walk(uintptr_t list, std::size_t next, Kind kind, F&& seen, int limit = 20000) {
	uintptr_t heads[Slots] {};
	if (!Get(list + static_cast<uintptr_t>(kind) * Slots * 8, heads)) {
		return;
	}
	for (const uintptr_t head : heads) {
		uintptr_t at = head;
		for (int steps = 0; Pointer(at) && steps < 4096 && limit > 0; ++steps, --limit) {
			seen(at);
			if (!Get(at + next, at)) {
				break;
			}
		}
	}
}

// True when `list` holds a unit list: every unit reached in the player, monster and object
// chains is of its chain's kind, and at least one player stands somewhere with a name.
auto Looks(uintptr_t list, std::size_t next, int& units) -> bool {
	uintptr_t all[Kinds * Slots] {};
	if (!Get(list, all)) {
		return false;
	}
	for (const uintptr_t p : all) {
		if (p != 0 && !Pointer(p)) {
			return false;
		}
	}
	int wrong = 0, players = 0;
	units = 0;
	for (uint32_t k = 0; k < Kinds; ++k) {
		const Kind kind = static_cast<Kind>(k);
		Walk(list, next, kind, [&](uintptr_t at) {
			uint32_t type = 99;
			if (!Get(at + UnitType, type) || type != k) {
				++wrong;
				return;
			}
			++units;
			if (kind == Kind::Player) {
				const Unit u = ReadUnit(at, kind);
				players += u.placed && !u.name.empty() && static_cast<unsigned char>(u.name[0]) >= 0x20 ? 1 : 0;
			}
		}, 2000);
	}
	return wrong == 0 && players > 0;
}

auto PluginFolderFile(const wchar_t* leaf) -> std::wstring {
	HMODULE self = nullptr;
	wchar_t path[MAX_PATH] {};
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	        reinterpret_cast<LPCWSTR>(&PluginFolderFile), &self) || GetModuleFileNameW(self, path, MAX_PATH) == 0) {
		return {};
	}
	std::wstring file(path);
	const auto slash = file.find_last_of(L"\\/");
	return (slash == std::wstring::npos ? std::wstring() : file.substr(0, slash + 1)) + leaf;
}

// The saved place: "<build> <module file> <offset hex> <next hex>".
struct Saved {
	std::wstring module;
	uintptr_t    offset = 0;
	std::size_t  next = 0;
};

auto LoadSaved() -> Saved {
	Saved saved;
	FILE* in = _wfopen(PluginFolderFile(L"darkestdiablo-unitlist.txt").c_str(), L"r");
	if (in == nullptr) {
		return saved;
	}
	wchar_t module[MAX_PATH] {};
	unsigned long long offset = 0, next = 0;
	unsigned build = 0;
	if (fwscanf(in, L"%u %259ls %llx %llx", &build, module, &offset, &next) == 4 && build == FrozenBuild) {
		saved = { module, static_cast<uintptr_t>(offset), static_cast<std::size_t>(next) };
	}
	fclose(in);
	return saved;
}

// Saves the place when it is inside a module (a plain memory block moves every start).
void Save(uintptr_t list, std::size_t next) {
	MEMORY_BASIC_INFORMATION info {};
	wchar_t path[MAX_PATH] {};
	if (VirtualQuery(reinterpret_cast<LPCVOID>(list), &info, sizeof(info)) == 0 || info.Type != MEM_IMAGE ||
	    GetModuleFileNameW(reinterpret_cast<HMODULE>(info.AllocationBase), path, MAX_PATH) == 0) {
		return;
	}
	std::wstring leaf(path);
	const auto slash = leaf.find_last_of(L"\\/");
	leaf = slash == std::wstring::npos ? leaf : leaf.substr(slash + 1);
	FILE* out = _wfopen(PluginFolderFile(L"darkestdiablo-unitlist.txt").c_str(), L"w");
	if (out == nullptr) {
		return;
	}
	fwprintf(out, L"%u %ls %llx %llx\n", FrozenBuild, leaf.c_str(),
		static_cast<unsigned long long>(list - reinterpret_cast<uintptr_t>(info.AllocationBase)), static_cast<unsigned long long>(next));
	fclose(out);
}

auto Writable(uintptr_t address) -> bool {
	MEMORY_BASIC_INFORMATION info {};
	return VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) != 0 && info.State == MEM_COMMIT &&
	       (info.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) != 0 && (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0;
}

auto Executable(DWORD protect) -> bool {
	return (protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0 && (protect & PAGE_GUARD) == 0;
}

// One address the game's code reads units from, with how many instructions use it and whether
// one of them is the exact instruction the community's tools look for.
struct Candidate {
	int  uses = 0;
	bool exact = false;
};

// Looks through the game's code for "mov reg, [base + slot * 8 + distance]": REX.W 8B, then a
// ModRM byte with a 32-bit distance and an index register (xx 84), then a scale of 8 (11xxxxxx),
// then the distance. MapAssist and D2RMH look for one such instruction: 48 03 C7 49 8B 8C C6
// (add rax, rdi; mov rcx, [r14 + rax * 8 + distance]), where r14 holds the game module's start.
// The list is then at module start + distance. Code blocks inside a module count from the
// module's start; code in plain memory (the loader may map the game itself) from its block's.
void CodeCandidates(std::map<uintptr_t, Candidate>& found, int& codeMb) {
	std::set<uintptr_t> starts;
	if (const HMODULE main = GetModuleHandleW(nullptr)) {
		starts.insert(reinterpret_cast<uintptr_t>(main));
	}
	if (const HMODULE game = GetModuleHandleW(L"D2R.exe")) {
		starts.insert(reinterpret_cast<uintptr_t>(game));
	}
	constexpr SIZE_T Step = 4 * 1024 * 1024, Overlap = 16;
	std::vector<unsigned char> buffer(Step + Overlap);
	MEMORY_BASIC_INFORMATION info {};
	for (uintptr_t at = 0x10000; at < 0x7FFFFFFF0000ULL;) {
		if (VirtualQuery(reinterpret_cast<LPCVOID>(at), &info, sizeof(info)) == 0) {
			break;
		}
		const uintptr_t start = reinterpret_cast<uintptr_t>(info.BaseAddress), end = start + info.RegionSize;
		const uintptr_t owner = reinterpret_cast<uintptr_t>(info.AllocationBase);
		const bool ours = info.Type == MEM_IMAGE ? starts.count(owner) != 0 : info.Type == MEM_PRIVATE;
		if (info.State == MEM_COMMIT && ours && Executable(info.Protect) && (info.Protect & PAGE_NOCACHE) == 0) {
			for (uintptr_t block = start; block < end; block += Step) {
				const SIZE_T want = static_cast<SIZE_T>(std::min<uintptr_t>(Step + Overlap, end - block));
				SIZE_T got = 0;
				if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(block), buffer.data(), want, &got) || got < 16) {
					continue;
				}
				codeMb += static_cast<int>(got >> 20);
				const unsigned char* b = buffer.data();
				for (SIZE_T i = 0; i + 8 <= got; ++i) {
					if ((b[i] & 0xF8) != 0x48 || b[i + 1] != 0x8B || (b[i + 2] & 0xC7) != 0x84 || (b[i + 3] >> 6) != 3) {
						continue;
					}
					int32_t distance = 0;
					std::memcpy(&distance, b + i + 4, 4);
					if (distance < 0x1000) {
						continue;
					}
					auto& c = found[owner + static_cast<uintptr_t>(distance)];
					++c.uses;
					c.exact = c.exact || (i >= 3 && b[i - 3] == 0x48 && b[i - 2] == 0x03 && b[i - 1] == 0xC7 && b[i] == 0x49 && b[i + 2] == 0x8C &&
					                      b[i + 3] == 0xC6);
				}
			}
		}
		at = end > at ? end : at + 0x1000;
	}
}

auto Hex(uintptr_t v) -> std::string {
	char text[32];
	std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(v));
	return text;
}

// Where an address sits, for the log: "D2RLoader.exe + 0x2a23910" or "plain memory".
auto Where(uintptr_t address) -> std::string {
	MEMORY_BASIC_INFORMATION info {};
	wchar_t path[MAX_PATH] {};
	if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) == 0 || info.Type != MEM_IMAGE ||
	    GetModuleFileNameW(reinterpret_cast<HMODULE>(info.AllocationBase), path, MAX_PATH) == 0) {
		return "plain memory " + Hex(address);
	}
	std::wstring leaf(path);
	const auto slash = leaf.find_last_of(L"\\/");
	leaf = slash == std::wstring::npos ? leaf : leaf.substr(slash + 1);
	char narrow[MAX_PATH] {};
	WideCharToMultiByte(CP_UTF8, 0, leaf.c_str(), -1, narrow, sizeof(narrow), nullptr, nullptr);
	return std::string(narrow) + " + " + Hex(address - reinterpret_cast<uintptr_t>(info.AllocationBase));
}

auto TryPlace(uintptr_t list, int& units) -> std::size_t {
	if (!Pointer(list) || !Writable(list)) {
		return 0;
	}
	for (const std::size_t next : NextTries) {
		if (Looks(list, next, units)) {
			return next;
		}
	}
	return 0;
}

// Caller holds g_lock.
auto FindLocked() -> bool {
	g_lastTry = GetTickCount64();
	if (!GameMemory::BuildMatches()) {
		g_status = "unit list: off (the game is not the frozen build)";
		return false;
	}
	const uint64_t started = GetTickCount64();
	int units = 0;

	// 1. The game's own code tells where the list is.
	std::map<uintptr_t, Candidate> found;
	int codeMb = 0;
	CodeCandidates(found, codeMb);
	uintptr_t best = 0;
	std::size_t bestNext = 0;
	Candidate bestWhy;
	int bestUnits = 0, fits = 0;
	for (const auto& [list, why] : found) {
		int n = 0;
		const std::size_t next = TryPlace(list, n);
		if (next == 0) {
			continue;
		}
		++fits;
		// The exact instruction first, then the address most of the code uses.
		if (best == 0 || (why.exact && !bestWhy.exact) || (why.exact == bestWhy.exact && why.uses > bestWhy.uses)) {
			best = list, bestNext = next, bestWhy = why, bestUnits = n;
		}
	}
	if (best != 0) {
		g_list = best, g_next = bestNext;
		Save(best, bestNext);
		g_status = "unit list found from the game's code at " + Where(best) + " (next at " + Hex(bestNext) + ", used by " +
		           std::to_string(bestWhy.uses) + " instruction(s)" + (bestWhy.exact ? ", the map tools' one among them" : "") + "; " +
		           std::to_string(fits) + " of " + std::to_string(found.size()) + " addresses fit; " + std::to_string(bestUnits) + " units; " +
		           std::to_string(codeMb) + " MB of code read in " + std::to_string(GetTickCount64() - started) + " ms)";
		return true;
	}

	// 2. The place saved by an earlier game, then the one found on our PCs.
	const Saved saved = LoadSaved();
	const Saved tries[] = { saved, { KnownModule, KnownOffset, KnownNext } };
	for (const Saved& place : tries) {
		if (place.module.empty()) {
			continue;
		}
		const HMODULE module = GetModuleHandleW(place.module.c_str());
		if (module == nullptr) {
			continue;
		}
		const uintptr_t list = reinterpret_cast<uintptr_t>(module) + place.offset;
		if (const std::size_t next = TryPlace(list, units); next != 0) {
			g_list = list, g_next = next;
			g_status = "unit list read from the saved place " + Where(list) + " (the game's code gave none of " +
			           std::to_string(found.size()) + " addresses, " + std::to_string(codeMb) + " MB read); " + std::to_string(units) + " units";
			return true;
		}
	}
	g_status = "unit list not found yet (" + std::to_string(found.size()) + " addresses from " + std::to_string(codeMb) +
	           " MB of game code, none fit; in game with the hero loaded it should)";
	return false;
}

template <typename F>
void ForEach(Kind kind, F&& seen) {
	uintptr_t list = 0;
	std::size_t next = 0;
	{
		const std::lock_guard<std::mutex> guard(g_lock);
		list = g_list, next = g_next;
	}
	if (list == 0) {
		return;
	}
	Walk(list, next, kind, [&](uintptr_t at) {
		if (const Unit u = ReadUnit(at, kind)) {
			seen(u);
		}
	});
}

}  // namespace

auto Ready() -> bool {
	const std::lock_guard<std::mutex> guard(g_lock);
	if (g_list != 0) {
		return true;
	}
	if (g_lastTry != 0 && GetTickCount64() - g_lastTry < 5000) {
		return false;  // looked moments ago
	}
	return FindLocked();
}

auto Find() -> bool {
	const std::lock_guard<std::mutex> guard(g_lock);
	g_list = 0;
	return FindLocked();
}

auto Status() -> std::string {
	const std::lock_guard<std::mutex> guard(g_lock);
	return g_status;
}

void Adopt(uintptr_t list, std::size_t next) {
	const std::lock_guard<std::mutex> guard(g_lock);
	if (g_list == 0) {
		g_list = list, g_next = next;
		Save(list, next);
		g_status = "unit list taken from the slow search at " + Where(list);
	}
}

auto Units(Kind kind) -> std::vector<Unit> {
	std::vector<Unit> all;
	if (Ready()) {
		ForEach(kind, [&](const Unit& u) { all.push_back(u); });
	}
	return all;
}

auto Units(Kind kind, uint32_t row) -> std::vector<Unit> {
	std::vector<Unit> some;
	if (Ready()) {
		ForEach(kind, [&](const Unit& u) {
			if (u.row == row) {
				some.push_back(u);
			}
		});
	}
	return some;
}

auto Player(const std::string& name) -> Unit {
	Unit found;
	if (Ready()) {
		ForEach(Kind::Player, [&](const Unit& u) {
			if (!found && u.placed && (name.empty() || u.name == name)) {
				found = u;
			}
		});
	}
	return found;
}

auto Nearest(Kind kind, uint32_t row, int x, int y) -> Unit {
	Unit found;
	long long best = -1;
	if (Ready()) {
		ForEach(kind, [&](const Unit& u) {
			if (!u.placed || (row != AnyRow && u.row != row)) {
				return;
			}
			const long long dx = u.x - x, dy = u.y - y;
			if (best < 0 || dx * dx + dy * dy < best) {
				best = dx * dx + dy * dy;
				found = u;
			}
		});
	}
	return found;
}

auto Refresh(const Unit& unit) -> Unit {
	if (!unit) {
		return {};
	}
	Unit now = ReadUnit(unit.address, unit.kind);
	return now && now.id == unit.id ? now : Unit {};
}

// The stat list keeps its stats as (pointer, count) pairs, one for the unit's own stats and one
// with items and skills added. The map tools' places for them (0x30 and 0x80) gave nothing on
// our build (0.11.51 test), so every 8-byte step of the list's first 0x100 bytes is tried as such a
// pair, and the first one holding life (stat 6) with maximum life (stat 7) at or above it wins.
namespace {

struct Stat { uint16_t param, id; int32_t value; };

// The stats array chosen as above; `where` is its pair's distance from the list's start.
auto StatsOf(const Unit& unit, std::size_t& where) -> std::vector<Stat> {
	if (!unit || (unit.kind != Kind::Player && unit.kind != Kind::Monster)) {
		return {};
	}
	uintptr_t list = 0;
	uint64_t head[0x100 / 8] {};
	if (!Get(unit.address + UnitStats, list) || !Pointer(list) || !Get(list, head)) {
		return {};
	}
	std::vector<Stat> fallback;
	for (std::size_t i = 0; i + 1 < std::size(head); ++i) {
		const uintptr_t stats = static_cast<uintptr_t>(head[i]);
		const uint64_t count = head[i + 1] & 0xFFFFFFFFu;
		if (!Pointer(stats) || count == 0 || count > 512) {
			continue;
		}
		std::vector<Stat> all(static_cast<std::size_t>(count));
		SIZE_T got = 0;
		if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(stats), all.data(), all.size() * sizeof(Stat), &got) ||
		    got != all.size() * sizeof(Stat)) {
			continue;
		}
		int life = -1, most = -1;
		for (const Stat& st : all) {
			if (st.param == 0 && st.id == LifeStat) {
				life = st.value >> 8;
			} else if (st.param == 0 && st.id == LifeStat + 1) {
				most = st.value >> 8;
			}
		}
		if (life >= 0 && most >= life) {
			where = i * 8;
			return all;
		}
		if (life >= 0 && fallback.empty()) {
			fallback = all;
			where = i * 8;
		}
	}
	return fallback;
}

auto StatValue(const std::vector<Stat>& all, uint16_t id) -> int {
	for (const Stat& st : all) {
		if (st.param == 0 && st.id == id) {
			return st.value;
		}
	}
	return -1;
}

}  // namespace

auto Life(const Unit& unit) -> int {
	std::size_t where = 0;
	const int value = StatValue(StatsOf(unit, where), LifeStat);
	return value < 0 ? -1 : value >> 8;
}

auto CharacterLevel(const Unit& unit) -> int {
	std::size_t where = 0;
	return StatValue(StatsOf(unit, where), 12);  // itemstatcost.txt: level
}

auto StatsReport(const Unit& unit) -> std::string {
	uintptr_t list = 0;
	if (!unit || !Get(unit.address + UnitStats, list)) {
		return "stats: unit unreadable";
	}
	std::size_t where = 0;
	const int life = Life(unit);
	StatsOf(unit, where);
	std::string text = "stats: list at unit + 0x88 = " + Hex(list) + ", life " + std::to_string(life) +
	                   (life >= 0 ? " from the pair at list + " + Hex(where) : std::string()) + "; list starts";
	uint64_t head[0x100 / 8] {};
	if (Pointer(list) && Get(list, head)) {
		for (const uint64_t v : head) {
			text += " " + Hex(static_cast<uintptr_t>(v));
		}
	} else {
		text += " unreadable";
	}
	return text;
}

auto Level(const Unit& unit) -> int {
	uintptr_t path = 0, room = 0, outer = 0, level = 0;
	uint32_t id = 0;
	if (!unit || !Get(unit.address + UnitPath, path) || !Pointer(path) ||
	    !Get(path + (Dynamic(unit.kind) ? DynamicRoom : StaticRoom), room) || !Pointer(room) || !Get(room + RoomOuter, outer) ||
	    !Pointer(outer) || !Get(outer + OuterLevel, level) || !Pointer(level) || !Get(level + LevelId, id) || id == 0 || id > 1000) {
		return -1;
	}
	return static_cast<int>(id);
}

auto KindName(Kind kind) -> const char* {
	switch (kind) {
		case Kind::Player: return "player";
		case Kind::Monster: return "monster";
		case Kind::Object: return "object";
		case Kind::Missile: return "missile";
		case Kind::Item: return "item";
		case Kind::Tile: return "tile";
	}
	return "unit";
}

}  // namespace GameState
