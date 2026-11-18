// See gamememory.h and docs/game-memory.md. READ ONLY.
#include "gamememory.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace GameMemory {
namespace {

// Unit layout in D2R (the community's long-standing map of the game's unit structure; checked
// against this build in game, see docs/game-memory.md).
constexpr std::size_t UnitType      = 0x00;  // uint32: 0 player, 1 monster, 2 object
constexpr std::size_t UnitClass     = 0x04;  // uint32: class (players) or objects.txt row (objects)
constexpr std::size_t UnitId        = 0x08;  // uint32: the unit's number, which picks its bucket
constexpr std::size_t UnitData      = 0x10;  // pointer: PlayerData (players: the name comes first)
constexpr std::size_t UnitPath      = 0x38;  // pointer: the unit's path
constexpr std::size_t PathDynamicX  = 0x02;  // uint16: players and monsters, in sub-tiles
constexpr std::size_t PathDynamicY  = 0x06;
constexpr std::size_t PathStaticX   = 0x10;  // uint32: objects, in sub-tiles
constexpr std::size_t PathStaticY   = 0x14;
constexpr uint32_t    TypePlayer    = 0;
constexpr uint32_t    TypeObject    = 2;
constexpr uint32_t    FireRow       = 39;     // objects.txt: RogueBonfire
constexpr uint32_t    StashRow      = 267;    // objects.txt: bank (the town's stash chest)
constexpr uint32_t    FrozenBuild   = 95032;  // D2R_VERSION 3.4.95032
constexpr uintptr_t   Buckets       = 128;    // the unit list: per unit kind, 128 chains of units
constexpr std::size_t NextTries[]   = { 0x150, 0x158 };  // where a unit keeps the next unit of its chain

std::mutex             g_lock;
std::atomic<bool>      g_scanning { false };
uintptr_t              g_heroUnit = 0;
Pos                    g_fire;
Pos                    g_stash;
std::string            g_report = "no scan yet";
uint64_t               g_scanStarted = 0;  // GetTickCount64 when the running scan began
std::atomic<uint32_t>  g_scanPass { 0 };   // which pass of the slow search runs (1 or 2)
std::atomic<uint32_t>  g_scannedMb { 0 };  // megabytes read so far in this pass

// Where the game's unit list sits: a fixed place inside a game module, found once by the slow
// search and saved next to the plugin (darkestdiablo-unitlist.txt), so later games read it
// straight away.
struct UnitList {
	std::wstring module;      // file name of the module that holds it
	uintptr_t    offset = 0;  // from the module's start to the list of player units
	std::size_t  next = 0;    // where a unit keeps the next unit of its chain
};
UnitList g_list;
bool     g_listLoaded = false;

template <typename T>
auto Read(uintptr_t address, T& out) -> bool {
	SIZE_T got = 0;
	return address != 0 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), &out, sizeof(T), &got) && got == sizeof(T);
}

auto Plausible(int x, int y) -> bool {
	return x > 0 && y > 0 && x < 60000 && y < 60000;
}

auto HeroAt(uintptr_t unit) -> Pos {
	uint32_t type = 99;
	uintptr_t path = 0;
	uint16_t x = 0, y = 0;
	if (!Read(unit + UnitType, type) || type != TypePlayer || !Read(unit + UnitPath, path) ||
	    !Read(path + PathDynamicX, x) || !Read(path + PathDynamicY, y) || !Plausible(x, y)) {
		return {};
	}
	return { x, y, true };
}

auto FireAt(uintptr_t unit) -> Pos {
	uintptr_t path = 0;
	uint32_t x = 0, y = 0;
	if (!Read(unit + UnitPath, path) || !Read(path + PathStaticX, x) || !Read(path + PathStaticY, y) ||
	    !Plausible(static_cast<int>(x), static_cast<int>(y))) {
		return {};
	}
	return { static_cast<int>(x), static_cast<int>(y), true };
}

auto ListFile() -> std::wstring {
	HMODULE self = nullptr;
	wchar_t path[MAX_PATH] {};
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	        reinterpret_cast<LPCWSTR>(&ListFile), &self) || GetModuleFileNameW(self, path, MAX_PATH) == 0) {
		return {};
	}
	std::wstring file(path);
	const auto slash = file.find_last_of(L"\\/");
	return (slash == std::wstring::npos ? std::wstring() : file.substr(0, slash + 1)) + L"darkestdiablo-unitlist.txt";
}

void LoadList() {
	if (g_listLoaded) {
		return;
	}
	g_listLoaded = true;
	FILE* in = _wfopen(ListFile().c_str(), L"r");
	if (in == nullptr) {
		return;
	}
	wchar_t module[MAX_PATH] {};
	unsigned long long offset = 0, next = 0;
	unsigned build = 0;
	if (fwscanf(in, L"%u %259ls %llx %llx", &build, module, &offset, &next) == 4 && build == FrozenBuild) {
		g_list = { module, static_cast<uintptr_t>(offset), static_cast<std::size_t>(next) };
	}
	fclose(in);
}

void SaveList() {
	FILE* out = _wfopen(ListFile().c_str(), L"w");
	if (out == nullptr) {
		return;
	}
	fwprintf(out, L"%u %ls %llx %llx\n", FrozenBuild, g_list.module.c_str(), static_cast<unsigned long long>(g_list.offset),
		static_cast<unsigned long long>(g_list.next));
	fclose(out);
}

// Calls `unit(address)` for every unit of one kind (0 players, 2 objects) in the list at `base`.
template <typename F>
void ForEachListed(uintptr_t base, uint32_t kind, std::size_t next, F&& unit) {
	for (uintptr_t bucket = 0; bucket < Buckets; ++bucket) {
		uintptr_t at = 0;
		if (!Read(base + (kind * Buckets + bucket) * 8, at)) {
			return;
		}
		for (int steps = 0; at != 0 && steps < 4096; ++steps) {
			unit(at);
			if (!Read(at + next, at)) {
				break;
			}
		}
	}
}

// True when the hero's name (with its ending zero) starts the player data of `unit`.
auto NamedAs(uintptr_t unit, const std::string& name) -> bool {
	uintptr_t data = 0;
	char text[32] {};
	return name.size() < sizeof(text) && Read(unit + UnitData, data) && Read(data, text) &&
	       std::memcmp(text, name.c_str(), name.size() + 1) == 0;
}

// The object of one objects.txt row nearest `from`, from the list.
auto ListedObject(uintptr_t base, std::size_t next, uint32_t wanted, Pos from) -> Pos {
	Pos found;
	long long best = -1;
	ForEachListed(base, TypeObject, next, [&](uintptr_t unit) {
		uint32_t type = 99, row = 0;
		if (!Read(unit + UnitType, type) || type != TypeObject || !Read(unit + UnitClass, row) || row != wanted) {
			return;
		}
		const Pos p = FireAt(unit);
		const long long dx = p.x - from.x, dy = p.y - from.y;
		if (p.ok && (best < 0 || dx * dx + dy * dy < best)) {
			best = dx * dx + dy * dy;
			found = p;
		}
	});
	return found;
}

auto ListedFire(uintptr_t base, std::size_t next, Pos hero) -> Pos {
	return ListedObject(base, next, FireRow, hero);
}

// The quick way: hero and fire straight from the saved unit list. False if it isn't known.
auto FromList(const std::string& name, uintptr_t& hero, Pos& heroPos, Pos& fire, Pos& stash) -> bool {
	LoadList();
	if (g_list.module.empty()) {
		return false;
	}
	const HMODULE module = GetModuleHandleW(g_list.module.c_str());
	if (module == nullptr) {
		return false;
	}
	const uintptr_t base = reinterpret_cast<uintptr_t>(module) + g_list.offset;
	hero = 0;
	ForEachListed(base, TypePlayer, g_list.next, [&](uintptr_t unit) {
		if (hero == 0 && NamedAs(unit, name) && HeroAt(unit).ok) {
			hero = unit;
		}
	});
	if (hero == 0) {
		return false;
	}
	heroPos = HeroAt(hero);
	fire = ListedFire(base, g_list.next, heroPos);
	stash = fire.ok ? ListedObject(base, g_list.next, StashRow, fire) : Pos {};
	return true;
}

// After the slow search found the hero: find the unit list that holds it, inside a game module,
// and save where it is. The list entry for the hero sits at list + bucket * 8, the bucket being
// the hero's number mod 128. Every place in a module's writable memory that holds the hero's
// address is tried, and kept only if the list there also holds object units in their chains.
auto FindList(uintptr_t hero, Pos heroPos) -> std::string {
	uint32_t id = 0;
	if (!Read(hero + UnitId, id)) {
		return "unit list: hero number unreadable";
	}
	MEMORY_BASIC_INFORMATION info {};
	int hits = 0;
	for (uintptr_t at = 0x10000; at < 0x7FFFFFFF0000ULL;) {
		if (VirtualQuery(reinterpret_cast<LPCVOID>(at), &info, sizeof(info)) == 0) {
			break;
		}
		const uintptr_t start = reinterpret_cast<uintptr_t>(info.BaseAddress), end = start + info.RegionSize;
		// Game modules first; the loader may also have placed the game's code in plain memory, so
		// private blocks are searched too (the list there can't be saved for the next game yet).
		if (info.State == MEM_COMMIT && (info.Type == MEM_IMAGE || info.Type == MEM_PRIVATE) &&
		    (info.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) != 0 && (info.Protect & (PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE)) == 0) {
			constexpr std::size_t Words = 512 * 1024;  // read 4 MB at a time
			std::vector<uintptr_t> words(Words);
			for (uintptr_t block = start; block < end; block += Words * 8) {
			SIZE_T got = 0;
			const SIZE_T want = static_cast<SIZE_T>(std::min<uintptr_t>(Words * 8, end - block));
			ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(block), words.data(), want, &got);
			for (std::size_t i = 0; i < got / 8; ++i) {
				if (words[i] != hero) {
					continue;
				}
				++hits;
				const uintptr_t base = block + i * 8 - (id % Buckets) * 8;
				for (const std::size_t next : NextTries) {
					int objects = 0, wrong = 0;
					ForEachListed(base, TypeObject, next, [&](uintptr_t unit) {
						uint32_t type = 99;
						(Read(unit + UnitType, type) && type == TypeObject ? objects : wrong) += 1;
					});
					if (objects == 0 || wrong != 0) {
						continue;
					}
					wchar_t path[MAX_PATH] {};
					const auto module = reinterpret_cast<uintptr_t>(info.AllocationBase);
					if (info.Type != MEM_IMAGE || GetModuleFileNameW(reinterpret_cast<HMODULE>(info.AllocationBase), path, MAX_PATH) == 0) {
						char line[256];
						std::snprintf(line, sizeof(line), "unit list found in plain memory at block 0x%llx + 0x%llx (next at 0x%llx), %d objects; not saved",
							static_cast<unsigned long long>(module), static_cast<unsigned long long>(base - module),
							static_cast<unsigned long long>(next), objects);
						return line;
					}
					std::wstring leaf(path);
					const auto slash = leaf.find_last_of(L"\\/");
					leaf = slash == std::wstring::npos ? leaf : leaf.substr(slash + 1);
					g_list = { leaf, base - module, next };
					SaveList();
					const Pos fire = ListedFire(base, next, heroPos);
					char line[256];
					std::snprintf(line, sizeof(line), "unit list found: %ls + 0x%llx (next at 0x%llx), %d objects, fire %s; saved",
						leaf.c_str(), static_cast<unsigned long long>(base - module), static_cast<unsigned long long>(next), objects,
						fire.ok ? "found" : "not found");
					return line;
				}
			}
			}
		}
		at = end > at ? end : at + 0x1000;
	}
	return "unit list not found (" + std::to_string(hits) + " places hold the hero's address)";
}

// Calls `chunk(base, bytes, size)` for every readable, writable private block of our memory.
template <typename F>
void ForEachChunk(F&& chunk) {
	constexpr SIZE_T Step = 4 * 1024 * 1024;
	constexpr SIZE_T Overlap = 64;
	std::vector<unsigned char> buffer(Step + Overlap);
	MEMORY_BASIC_INFORMATION info {};
	for (uintptr_t at = 0x10000; at < 0x7FFFFFFF0000ULL;) {
		if (VirtualQuery(reinterpret_cast<LPCVOID>(at), &info, sizeof(info)) == 0) {
			break;
		}
		const uintptr_t start = reinterpret_cast<uintptr_t>(info.BaseAddress);
		const uintptr_t end = start + info.RegionSize;
		// Uncached and write-combined blocks (graphics memory) are skipped: reading them is very slow.
		if (info.State == MEM_COMMIT && info.Type == MEM_PRIVATE && (info.Protect & (PAGE_READWRITE)) != 0 &&
		    (info.Protect & (PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE)) == 0) {
			for (uintptr_t block = start; block < end; block += Step) {
				const SIZE_T want = static_cast<SIZE_T>(std::min<uintptr_t>(Step + Overlap, end - block));
				SIZE_T got = 0;
				if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(block), buffer.data(), want, &got) && got >= 16) {
					chunk(block, buffer.data(), got);
					g_scannedMb += static_cast<uint32_t>(want >> 20);
				}
			}
		}
		at = end > at ? end : at + 0x1000;
	}
}

void Scan(std::string name) {
	const auto started = GetTickCount64();
	{
		uintptr_t hero = 0;
		Pos heroPos, fire, stash;
		if (FromList(name, hero, heroPos, fire, stash)) {
			char line[256];
			std::snprintf(line, sizeof(line), "read from the unit list in %llu ms: hero at %d,%d, fire at %d,%d (%s), stash at %d,%d (%s)",
				static_cast<unsigned long long>(GetTickCount64() - started), heroPos.x, heroPos.y, fire.x, fire.y, fire.ok ? "ok" : "not found",
				stash.x, stash.y, stash.ok ? "ok" : "not found");
			const std::lock_guard<std::mutex> guard(g_lock);
			g_heroUnit = hero;
			g_fire = fire;
			g_stash = stash;
			g_report = line;
			g_scanning = false;
			return;
		}
	}
	// Pass 1: where the hero's name sits (the start of its PlayerData), and every object unit
	// of the fire's row.
	g_scanPass = 1;
	g_scannedMb = 0;
	std::vector<uintptr_t> names;
	std::vector<uintptr_t> fires;
	std::vector<uintptr_t> stashes;
	const std::size_t length = name.size() + 1;  // with its ending zero
	ForEachChunk([&](uintptr_t base, const unsigned char* bytes, SIZE_T size) {
		for (SIZE_T i = 0; i + 16 <= size; i += 8) {
			if (bytes[i] == static_cast<unsigned char>(name[0]) && names.size() < 64 && i + length <= size &&
			    std::memcmp(bytes + i, name.c_str(), length) == 0) {
				names.push_back(base + i);
			}
			uint32_t type, row;
			std::memcpy(&type, bytes + i, 4);
			std::memcpy(&row, bytes + i + 4, 4);
			if (type == TypeObject && row == FireRow && fires.size() < 512) {
				fires.push_back(base + i);
			}
			if (type == TypeObject && row == StashRow && stashes.size() < 512) {
				stashes.push_back(base + i);
			}
		}
	});
	// Pass 2: the units that point at that PlayerData (players keep it at UnitData).
	// The names are sorted so each 8 bytes costs one range check and, rarely, a binary search
	// (comparing every value with every name made this pass take minutes).
	const auto pass1 = GetTickCount64() - started;
	const uint32_t pass1Mb = g_scannedMb;
	g_scanPass = 2;
	g_scannedMb = 0;
	std::vector<uintptr_t> heroes;
	if (!names.empty()) {
		std::sort(names.begin(), names.end());
		const uintptr_t lowest = names.front(), highest = names.back();
		ForEachChunk([&](uintptr_t base, const unsigned char* bytes, SIZE_T size) {
			for (SIZE_T i = 0; i + 8 <= size && heroes.size() < 64; i += 8) {
				uintptr_t value;
				std::memcpy(&value, bytes + i, 8);
				if (value < lowest || value > highest || !std::binary_search(names.begin(), names.end(), value)) {
					continue;
				}
				if (base + i >= UnitData && HeroAt(base + i - UnitData).ok) {
					heroes.push_back(base + i - UnitData);
				}
			}
		});
	}
	// Several units point at the hero's data (copies the game keeps for other uses). The hero is
	// the one standing nearest a town fire; the fire is that fire.
	std::vector<Pos> firePos;
	for (const uintptr_t f : fires) {
		if (const Pos p = FireAt(f); p.ok) {
			firePos.push_back(p);
		}
	}
	uintptr_t hero = 0;
	Pos heroPos, fire;
	long long best = -1;
	std::string seen;
	for (const uintptr_t h : heroes) {
		const Pos p = HeroAt(h);
		if (!p.ok) {
			continue;
		}
		seen += (seen.empty() ? "" : " ") + std::to_string(p.x) + "," + std::to_string(p.y);
		if (hero == 0) {
			hero = h, heroPos = p;  // with no fire near anyone: the first that reads
		}
		for (const Pos& f : firePos) {
			const long long dx = f.x - p.x, dy = f.y - p.y;
			if (dx * dx + dy * dy < 200 * 200 && (best < 0 || dx * dx + dy * dy < best)) {
				best = dx * dx + dy * dy;
				hero = h, heroPos = p, fire = f;
			}
		}
	}
	std::string fireSeen;
	for (std::size_t i = 0; i < firePos.size() && i < 6; ++i) {
		fireSeen += (fireSeen.empty() ? "" : " ") + std::to_string(firePos[i].x) + "," + std::to_string(firePos[i].y);
	}
	char line[512];
	std::snprintf(line, sizeof(line),
		"memory scan %llu ms: name found %d time(s), hero units %d (at %s), hero at %d,%d (%s); fire units %d of %d candidates (at %s), fire at %d,%d (%s)",
		static_cast<unsigned long long>(GetTickCount64() - started), static_cast<int>(names.size()), static_cast<int>(heroes.size()),
		seen.c_str(), heroPos.x, heroPos.y, heroPos.ok ? "ok" : "not found", static_cast<int>(firePos.size()), static_cast<int>(fires.size()),
		fireSeen.c_str(), fire.x, fire.y, fire.ok ? "ok" : "not found");
	std::string report = line;
	report += "; pass 1 read " + std::to_string(pass1Mb) + " MB in " + std::to_string(pass1) + " ms";
	if (hero != 0) {
		// The unit list holds the real hero; try the chosen one first, then the others.
		std::string found = FindList(hero, heroPos);
		for (const uintptr_t h : heroes) {
			if (h != hero && found.rfind("unit list found", 0) != 0) {
				if (const Pos p = HeroAt(h); p.ok) {
					std::string other = FindList(h, p);
					if (other.rfind("unit list found", 0) == 0) {
						hero = h, heroPos = p;
						found = other + " (from another copy of the hero)";
						fire = Pos {};
					}
				}
			}
		}
		report += "; " + found;
		if (!fire.ok && !g_list.module.empty()) {
			if (const HMODULE module = GetModuleHandleW(g_list.module.c_str())) {
				fire = ListedFire(reinterpret_cast<uintptr_t>(module) + g_list.offset, g_list.next, heroPos);
			}
		}
	}
	// The town's stash chest: the one nearest the fire.
	Pos stash;
	if (fire.ok) {
		long long nearest = -1;
		for (const uintptr_t c : stashes) {
			const Pos p = FireAt(c);
			const long long dx = p.x - fire.x, dy = p.y - fire.y;
			if (p.ok && dx * dx + dy * dy < 200 * 200 && (nearest < 0 || dx * dx + dy * dy < nearest)) {
				nearest = dx * dx + dy * dy;
				stash = p;
			}
		}
	}
	report += "; stash " + (stash.ok ? "at " + std::to_string(stash.x) + "," + std::to_string(stash.y) : std::string("not found"));
	const std::lock_guard<std::mutex> guard(g_lock);
	g_heroUnit = hero;
	g_fire = fire;
	g_stash = stash;
	g_report = report;
	g_scanning = false;
}

}  // namespace

namespace {

// The file version of one file ("3.4.0.95032"), or "" when it can't be read.
auto FileVersion(const std::wstring& path) -> std::string {
	DWORD ignored = 0;
	const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
	if (size == 0) {
		return {};
	}
	std::vector<unsigned char> data(size);
	VS_FIXEDFILEINFO* info = nullptr;
	UINT infoSize = 0;
	if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()) ||
	    !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) || info == nullptr) {
		return {};
	}
	char text[64];
	std::snprintf(text, sizeof(text), "%u.%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
		HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
	return text;
}

}  // namespace

namespace {
std::string g_buildFile = "not found";  // which D2R.exe the version came from, for the log
}

// The game runs inside D2RLoader's process, and asking Windows for the process's own file (or
// even for the "D2R.exe" module) gave the loader's version (1.3.1.0) in game. So the version is
// read from the D2R.exe file in the game folder: next to the process's file, or up from this
// plugin's own folder (Mods\darkestdiablo\... sits inside the game folder).
auto BuildFound() -> std::string {
	static std::string found = [] {
		std::vector<std::wstring> folders;
		wchar_t path[MAX_PATH] {};
		auto addParents = [&](std::wstring p, int levels) {
			for (int i = 0; i < levels; ++i) {
				const auto slash = p.find_last_of(L"\\/");
				if (slash == std::wstring::npos) {
					return;
				}
				p.resize(slash);
				folders.push_back(p + L"\\");
			}
		};
		if (GetModuleFileNameW(nullptr, path, MAX_PATH) != 0) {
			addParents(path, 2);
		}
		HMODULE self = nullptr;
		if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		        reinterpret_cast<LPCWSTR>(&BuildFound), &self) && GetModuleFileNameW(self, path, MAX_PATH) != 0) {
			addParents(path, 6);
		}
		for (const auto& folder : folders) {
			const std::wstring file = folder + L"D2R.exe";
			if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) {
				continue;
			}
			if (std::string v = FileVersion(file); !v.empty()) {
				char narrow[MAX_PATH * 2] {};
				WideCharToMultiByte(CP_UTF8, 0, file.c_str(), -1, narrow, sizeof(narrow), nullptr, nullptr);
				g_buildFile = narrow;
				return v;
			}
		}
		return std::string("unknown");
	}();
	return found;
}

// D2R.exe's version numbers are 16 bits each, so build 95032 is stored as 95032 - 65536 = 29496
// ("3.4.29496.0"; Windows' own file details show "3.4.95032").
auto BuildMatches() -> bool {
	const std::string raw = "3.4." + std::to_string(FrozenBuild & 0xFFFF) + ".0";
	return BuildFound() == raw;
}

void StartScan(const std::string& heroName) {
	if (heroName.empty()) {
		const std::lock_guard<std::mutex> guard(g_lock);
		g_report = "no search: the hero's name is unknown";
		return;
	}
	if (!BuildMatches() || g_scanning.exchange(true)) {
		return;
	}
	{
		const std::lock_guard<std::mutex> guard(g_lock);
		g_scanStarted = GetTickCount64();
		g_report = "searching for " + heroName;
	}
	std::thread(Scan, heroName).detach();
}

auto Scanning() -> bool {
	return g_scanning;
}

auto Hero() -> Pos {
	const std::lock_guard<std::mutex> guard(g_lock);
	return g_heroUnit != 0 ? HeroAt(g_heroUnit) : Pos {};
}

auto Fire() -> Pos {
	const std::lock_guard<std::mutex> guard(g_lock);
	return g_fire;
}

auto Stash() -> Pos {
	const std::lock_guard<std::mutex> guard(g_lock);
	return g_stash;
}

auto Report() -> std::string {
	const std::lock_guard<std::mutex> guard(g_lock);
	const std::string build = BuildFound();
	return "Darkest Diablo game build " + build + " from " + g_buildFile + (BuildMatches() ? " (frozen build)" : " (other build: reading is off)") + "; " + g_report +
	       (g_scanning ? " (" + std::to_string((GetTickCount64() - g_scanStarted) / 1000) + " s so far, pass " + std::to_string(g_scanPass) +
	                         ", " + std::to_string(g_scannedMb) + " MB read)" : std::string());
}

}  // namespace GameMemory
