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
constexpr std::size_t UnitData      = 0x10;  // pointer: PlayerData (players: the name comes first)
constexpr std::size_t UnitPath      = 0x38;  // pointer: the unit's path
constexpr std::size_t PathDynamicX  = 0x02;  // uint16: players and monsters, in sub-tiles
constexpr std::size_t PathDynamicY  = 0x06;
constexpr std::size_t PathStaticX   = 0x10;  // uint32: objects, in sub-tiles
constexpr std::size_t PathStaticY   = 0x14;
constexpr uint32_t    TypePlayer    = 0;
constexpr uint32_t    TypeObject    = 2;
constexpr uint32_t    FireRow       = 39;     // objects.txt: RogueBonfire
constexpr uint32_t    FrozenBuild   = 95032;  // D2R_VERSION 3.4.95032

std::mutex             g_lock;
std::atomic<bool>      g_scanning { false };
uintptr_t              g_heroUnit = 0;
Pos                    g_fire;
std::string            g_report = "no scan yet";
uint64_t               g_scanStarted = 0;  // GetTickCount64 when the running scan began

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
		if (info.State == MEM_COMMIT && info.Type == MEM_PRIVATE && (info.Protect & (PAGE_READWRITE)) != 0 &&
		    (info.Protect & PAGE_GUARD) == 0) {
			for (uintptr_t block = start; block < end; block += Step) {
				const SIZE_T want = static_cast<SIZE_T>(std::min<uintptr_t>(Step + Overlap, end - block));
				SIZE_T got = 0;
				if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(block), buffer.data(), want, &got) && got >= 16) {
					chunk(block, buffer.data(), got);
				}
			}
		}
		at = end > at ? end : at + 0x1000;
	}
}

void Scan(std::string name) {
	const auto started = GetTickCount64();
	// Pass 1: where the hero's name sits (the start of its PlayerData), and every object unit
	// of the fire's row.
	std::vector<uintptr_t> names;
	std::vector<uintptr_t> fires;
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
		}
	});
	// Pass 2: the units that point at that PlayerData (players keep it at UnitData).
	// The names are sorted so each 8 bytes costs one range check and, rarely, a binary search
	// (comparing every value with every name made this pass take minutes).
	std::vector<uintptr_t> heroes;
	if (!names.empty()) {
		std::sort(names.begin(), names.end());
		const uintptr_t lowest = names.front(), highest = names.back();
		ForEachChunk([&](uintptr_t base, const unsigned char* bytes, SIZE_T size) {
			for (SIZE_T i = 0; i + 8 <= size && heroes.size() < 16; i += 8) {
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
	// The hero: the first unit whose position reads. The fire: the fire unit nearest to it.
	uintptr_t hero = 0;
	Pos heroPos;
	for (const uintptr_t h : heroes) {
		heroPos = HeroAt(h);
		if (heroPos.ok) {
			hero = h;
			break;
		}
	}
	Pos fire;
	long long best = -1;
	int fireUnits = 0;
	for (const uintptr_t f : fires) {
		const Pos p = FireAt(f);
		if (!p.ok) {
			continue;
		}
		++fireUnits;
		const long long dx = p.x - heroPos.x, dy = p.y - heroPos.y;
		const long long d = dx * dx + dy * dy;
		if (heroPos.ok && d < 200 * 200 && (best < 0 || d < best)) {
			best = d;
			fire = p;
		}
	}
	char line[512];
	std::snprintf(line, sizeof(line),
		"memory scan %llu ms: name found %d time(s), hero units %d, hero at %d,%d (%s); fire units %d of %d candidates, fire at %d,%d (%s)",
		static_cast<unsigned long long>(GetTickCount64() - started), static_cast<int>(names.size()), static_cast<int>(heroes.size()),
		heroPos.x, heroPos.y, heroPos.ok ? "ok" : "not found", fireUnits, static_cast<int>(fires.size()), fire.x, fire.y, fire.ok ? "ok" : "not found");
	const std::lock_guard<std::mutex> guard(g_lock);
	g_heroUnit = hero;
	g_fire = fire;
	g_report = line;
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

auto Report() -> std::string {
	const std::lock_guard<std::mutex> guard(g_lock);
	const std::string build = BuildFound();
	return "Darkest Diablo game build " + build + " from " + g_buildFile + (BuildMatches() ? " (frozen build)" : " (other build: reading is off)") + "; " + g_report +
	       (g_scanning ? " (" + std::to_string((GetTickCount64() - g_scanStarted) / 1000) + " s so far)" : std::string());
}

}  // namespace GameMemory
