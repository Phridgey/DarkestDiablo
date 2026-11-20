// The linear zone (prototype). See zone.h and docs/linear-zone.md.
//
// Town: about 1.5 s after the hero arrives, a red portal (objects.txt row 189, CainPortal: the
// red Tristram portal, a dummy the game never operates) is put up a few steps from the fire.
// "cheat ospawn" places objects at the hero's feet, so the hero is moved there and straight back
// ("location"), as the traveler's portal does (campfire.cpp). When the hero walks into the portal
// the plugin sends them to the zone with "goto". The game can't make a real linked portal.
//
// Zone: on arrival the hero is put at the west end. When they reach the last room, Bishibosh
// (superuniques.txt row 0, a Fallen Shaman with his Fallen) is spawned a little ahead of them:
// the game only spawns near the hero. His death gives the Town Portal scroll.
//
// Everything is polled from one background thread every 200 ms with the game-state API
// (gameapi.h); console commands run on the UI thread.
#include "zone.h"
#include "gameapi.h"
#include "gamememory.h"
#include <D2RLPlugin/core_exports.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <thread>

namespace Zone {

namespace {

// ---- Numbers to tune after the first test ------------------------------------------------
constexpr int ZoneLevelId   = 31;      // ZONE_ID in src/patches/120_linear_zone.py ("Act 1 - Jail 3")
constexpr int TownLevelId   = 1;       // Rogue Encampment
constexpr int OriginX       = 3500;    // levels.txt OffsetX/Y of level 31 (kept by the patch)
constexpr int OriginY       = 1600;
constexpr int RoomTiles     = 24;      // src/maps/linear_zone.json: 6 rooms of 24 tiles, west to east
constexpr int Rooms         = 6;
constexpr int StartTileX    = 16;      // where the hero arrives: the first room, by its east opening
constexpr int StartTileY    = 12;
constexpr int BossRoomEnter = (Rooms - 1) * RoomTiles + 3;  // hero tile x from which the boss comes out
constexpr int BossAhead     = 12;      // sub-tiles east of the hero where the boss is spawned
constexpr int BossRow       = 0;       // superuniques.txt row: Bishibosh
constexpr int PortalRow     = 189;     // objects.txt row: CainPortal (red)
constexpr int FireRow       = 39;      // objects.txt row: the town fire
constexpr int PortalFromFireX = -9;    // where the red portal stands, in sub-tiles from the fire
constexpr int PortalFromFireY = 6;
constexpr int PortalReach   = 4;       // the hero this close to the portal (sub-tiles) steps through
constexpr int PortalArm     = 8;       // ...after having been at least this far from it once
constexpr int TownDelayMs   = 1500;    // the town must be loaded before anything is spawned
constexpr int ZoneDelayMs   = 1000;
constexpr int BossTries     = 3;
constexpr int BossWaitMs    = 3000;    // boss not seen this long after a spawn: spawn again

const D2RL::ThreadService* g_threads = nullptr;
GiveFn                     g_give    = nullptr;
std::mutex                 g_mutex;    // guards everything below

int  g_level = -1;
// Town
bool g_portalUp = false;
int  g_portalX = 0, g_portalY = 0;
bool g_armed = false;
bool g_travelling = false;
// Zone
bool g_bossOut = false;
int  g_bossTries = 0;
std::chrono::steady_clock::time_point g_bossAt {};
bool g_bossSeen = false;
uint32_t g_bossId = 0;
bool g_cleared = false;
std::atomic<uintptr_t> g_visit { 1 };  // changes on every level change, so old timers do nothing

void Log(const D2RL::PluginContext* context, const char* format, ...) noexcept {
	char message[256] {};
	va_list args;
	va_start(args, format);
	std::vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	context->LogInfo(message);
}

// UI thread only.
auto RunConsole(const D2RL::PluginContext* context, const char* command) noexcept -> bool {
	namespace CE = D2RL::CoreExports;
	static CE::ExecuteConsoleCommandFn execute = nullptr;
	if (execute == nullptr) {
		const HMODULE core = GetModuleHandleA(CE::CoreDllName);
		execute = core == nullptr ? nullptr :
			reinterpret_cast<CE::ExecuteConsoleCommandFn>(GetProcAddress(core, CE::ExecuteConsoleCommandInfo.name));
	}
	if (execute == nullptr) {
		context->LogWarn("Zone: ExecuteConsoleCommand not found in D2RCore.");
		return false;
	}
	const bool ok = execute(command);
	Log(context, "Zone: console %s -> %s", command, ok ? "sent" : "refused");
	return ok;
}

auto SubTile(int origin, int tile) noexcept -> int { return (origin + tile) * 5 + 2; }

auto Distance(int ax, int ay, int bx, int by) noexcept -> double {
	const double dx = ax - bx, dy = ay - by;
	return std::sqrt(dx * dx + dy * dy);
}

// Runs `callback` on the UI thread after `delayMs`, unless the hero changed level meanwhile.
void Later(const D2RL::PluginContext* context, int delayMs, D2RL::Threads::Callback callback) noexcept {
	const uintptr_t visit = g_visit.load();
	std::thread([context, delayMs, callback, visit] {
		std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
		if (visit == g_visit.load() && g_threads != nullptr) {
			g_threads->runOnUiThread(context, callback, reinterpret_cast<void*>(visit));
		}
	}).detach();
}

// ---- Town: the red portal ----------------------------------------------------------------

auto FirePos() -> GameMemory::Pos {
	const auto fires = Game::Objects(-1, static_cast<uint32_t>(FireRow));
	if (!fires.empty() && fires.front().placed) {
		return { fires.front().x, fires.front().y, true };
	}
	return GameMemory::Fire();
}

// A red portal already standing within `near` sub-tiles of (x, y): the town keeps it while loaded.
auto FindPortal(int x, int y, int near, int& foundX, int& foundY) -> bool {
	for (const Game::Unit& o : Game::Objects(-1, static_cast<uint32_t>(PortalRow))) {
		if (o.placed && Distance(o.x, o.y, x, y) <= near) {
			foundX = o.x, foundY = o.y;
			return true;
		}
	}
	return false;
}

// UI thread. Put the portal up at (x, y): hero there, spawn, hero back.
void SpawnPortalAt(const D2RL::PluginContext* context, int x, int y, int heroX, int heroY) noexcept {
	char command[64];
	std::snprintf(command, sizeof(command), "location %d,%d", x, y);
	RunConsole(context, command);
	RunConsole(context, "cheat ospawn 189");  // objects.txt row 189: CainPortal (red)
	std::snprintf(command, sizeof(command), "location %d,%d", heroX, heroY);
	RunConsole(context, command);
}

void __cdecl NotePortal(const D2RL::PluginContext* context, void* visit) noexcept;

void __cdecl PlacePortal(const D2RL::PluginContext* context, void* visit) noexcept {
	std::unique_lock lock(g_mutex);
	if (reinterpret_cast<uintptr_t>(visit) != g_visit.load() || g_level != TownLevelId) {
		return;
	}
	const GameMemory::Pos fire = FirePos();
	const Game::Unit hero = Game::Hero();
	if (!fire.ok || !hero) {
		Log(context, "Zone: no red portal (fire %s, hero %s).", fire.ok ? "found" : "not found", hero ? "found" : "not found");
		return;
	}
	const int x = fire.x + PortalFromFireX, y = fire.y + PortalFromFireY;
	int fx = 0, fy = 0;
	if (FindPortal(x, y, 6, fx, fy)) {
		g_portalUp = true, g_portalX = fx, g_portalY = fy;
		g_armed = Distance(hero.x, hero.y, fx, fy) > PortalArm;
		Log(context, "Zone: red portal still standing at %d,%d.", fx, fy);
		return;
	}
	Log(context, "Zone: red portal at %d,%d (fire %d,%d, hero %d,%d).", x, y, fire.x, fire.y, hero.x, hero.y);
	g_portalX = x, g_portalY = y;
	lock.unlock();  // console commands run without the lock (they may raise game events)
	SpawnPortalAt(context, x, y, hero.x, hero.y);
	Later(context, 400, NotePortal);
}

// UI thread. Where the portal really stands (a blocked spot moves it).
void __cdecl NotePortal(const D2RL::PluginContext* context, void* visit) noexcept {
	const std::lock_guard lock(g_mutex);
	if (reinterpret_cast<uintptr_t>(visit) != g_visit.load()) {
		return;
	}
	int fx = 0, fy = 0;
	if (FindPortal(g_portalX, g_portalY, 30, fx, fy)) {
		g_portalUp = true, g_portalX = fx, g_portalY = fy;
		const Game::Unit hero = Game::Hero();
		g_armed = hero && Distance(hero.x, hero.y, fx, fy) > PortalArm;
		Log(context, "Zone: red portal found at %d,%d.", fx, fy);
	} else {
		context->LogWarn("Zone: the red portal did not appear (\"cheat ospawn 189\" refused?).");
	}
}

void __cdecl Travel(const D2RL::PluginContext* context, void*) noexcept {
	char command[32];
	std::snprintf(command, sizeof(command), "goto %d", ZoneLevelId);
	RunConsole(context, command);
}

// ---- Zone: arrival, boss, clear ----------------------------------------------------------

void __cdecl PlaceHero(const D2RL::PluginContext* context, void* visit) noexcept {
	std::unique_lock lock(g_mutex);
	if (reinterpret_cast<uintptr_t>(visit) != g_visit.load() || g_level != ZoneLevelId) {
		return;
	}
	const Game::Unit hero = Game::Hero();
	const int x = SubTile(OriginX, StartTileX), y = SubTile(OriginY, StartTileY);
	if (hero && hero.x / 5 - OriginX <= RoomTiles) {
		Log(context, "Zone: arrived at %d,%d, already in the first room.", hero.x, hero.y);
		return;
	}
	Log(context, "Zone: arrived at %d,%d; moving to the west end (%d,%d).", hero ? hero.x : 0, hero ? hero.y : 0, x, y);
	lock.unlock();
	char command[64];
	std::snprintf(command, sizeof(command), "location %d,%d", x, y);
	RunConsole(context, command);
}

void __cdecl SpawnBoss(const D2RL::PluginContext* context, void*) noexcept {
	const Game::Unit hero = Game::Hero();
	if (!hero) {
		return;
	}
	char command[64];
	std::snprintf(command, sizeof(command), "cheat monster super:%d @ %d,%d", BossRow, hero.x + BossAhead, hero.y);
	RunConsole(context, command);
}

void __cdecl GiveScroll(const D2RL::PluginContext* context, void*) noexcept {
	if (g_give != nullptr) {
		g_give(context);
	}
}

// Background thread, every 200 ms.
void Tick(const D2RL::PluginContext* context) {
	std::unique_lock lock(g_mutex);
	if (g_level == TownLevelId && g_portalUp && !g_travelling) {
		const Game::Unit hero = Game::Hero();
		if (!hero) {
			return;
		}
		const double d = Distance(hero.x, hero.y, g_portalX, g_portalY);
		if (!g_armed && d > PortalArm) {
			g_armed = true;
		} else if (g_armed && d <= PortalReach) {
			g_travelling = true;
			Log(context, "Zone: hero stepped into the red portal (%.1f sub-tiles); going to the zone.", d);
			g_threads->runOnUiThread(context, Travel, nullptr);
		}
		return;
	}
	if (g_level != ZoneLevelId || g_cleared) {
		return;
	}
	const Game::Unit hero = Game::Hero();
	if (!hero) {
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	const int tileX = hero.x / 5 - OriginX;
	if (!g_bossOut && tileX >= BossRoomEnter) {
		g_bossOut = true;
		g_bossTries = 1;
		g_bossAt = now;
		Log(context, "Zone: hero reached the last room (tile %d); the boss comes out.", tileX);
		g_threads->runOnUiThread(context, SpawnBoss, nullptr);
		return;
	}
	if (!g_bossOut) {
		return;
	}
	bool found = false, alive = false;
	for (const Game::MonsterDetails& m : Game::Monsters(-1, false)) {
		if (m.superUnique && m.superUniqueId == BossRow && (g_bossId == 0 || m.unit.id == g_bossId)) {
			found = true, alive = m.alive;
			if (!g_bossSeen) {
				g_bossSeen = true, g_bossId = m.unit.id;
				Log(context, "Zone: the boss is out at %d,%d (unit %u).", m.unit.x, m.unit.y, m.unit.id);
			}
			break;
		}
	}
	// Not found: dead and gone, or out of the loaded area if the hero walked back west.
	if (g_bossSeen && ((found && !alive) || (!found && tileX >= BossRoomEnter - RoomTiles))) {
		g_cleared = true;
		Log(context, "Zone: the boss is dead (%s); cleared.", found ? "corpse" : "gone");
		g_threads->runOnUiThread(context, GiveScroll, nullptr);
		return;
	}
	if (!g_bossSeen && now - g_bossAt > std::chrono::milliseconds(BossWaitMs)) {
		if (g_bossTries >= BossTries) {
			g_cleared = true;  // stop trying; "dd-zone-boss" spawns him by hand
			context->LogWarn("Zone: the boss could not be spawned; type dd-zone-boss to try again.");
			return;
		}
		++g_bossTries;
		g_bossAt = now;
		Log(context, "Zone: boss not seen; spawning again (try %d).", g_bossTries);
		g_threads->runOnUiThread(context, SpawnBoss, nullptr);
	}
}

// ---- Console -----------------------------------------------------------------------------

auto ZoneCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	Travel(command->plugin, nullptr);
	return D2RL::ConsoleCommandResult::Handled;
}

// Puts the red portal where the hero stands (to try other spots); prints its offset from the fire.
auto PortalCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	const Game::Unit hero = Game::Hero();
	{
		const std::lock_guard lock(g_mutex);
		if (g_level != TownLevelId || !hero) {
			command->plugin->WriteConsoleMessage("Stand in the Rogue Encampment first.");
			return D2RL::ConsoleCommandResult::Failed;
		}
		g_portalUp = true, g_portalX = hero.x, g_portalY = hero.y, g_armed = false;
	}
	RunConsole(command->plugin, "cheat ospawn 189");
	const GameMemory::Pos fire = FirePos();
	char line[128];
	std::snprintf(line, sizeof(line), "Red portal at %d,%d; from the fire: %d,%d.", hero.x, hero.y,
		fire.ok ? hero.x - fire.x : 0, fire.ok ? hero.y - fire.y : 0);
	command->plugin->WriteConsoleMessage(line);
	command->plugin->LogInfo(line);
	return D2RL::ConsoleCommandResult::Handled;
}

auto BossCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	{
		const std::lock_guard lock(g_mutex);
		if (g_level != ZoneLevelId) {
			command->plugin->WriteConsoleMessage("Stand in the linear zone first.");
			return D2RL::ConsoleCommandResult::Failed;
		}
		g_bossOut = true, g_bossTries = BossTries, g_bossAt = std::chrono::steady_clock::now();
		g_bossSeen = false, g_bossId = 0, g_cleared = false;
	}
	SpawnBoss(command->plugin, nullptr);
	return D2RL::ConsoleCommandResult::Handled;
}

}  // namespace

auto Load(const D2RL::PluginContext* context, GiveFn giveTownPortal) noexcept -> bool {
	if (context->QueryService(&g_threads) != D2RL::ServiceQueryResult::Success || !D2RL::HasThreadServiceField(g_threads, D2RL::ThreadServiceRequiredSize)) {
		context->LogWarn("Zone: thread service missing; the linear zone is off.");
		g_threads = nullptr;
		return true;  // the rest of the mod still works
	}
	g_give = giveTownPortal;
	std::thread([context] {
		for (;;) {
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			Tick(context);
		}
	}).detach();
	return context->RegisterConsoleCommand("dd-zone", ZoneCommand, "Go to the linear zone (test).") &&
		context->RegisterConsoleCommand("dd-zone-portal", PortalCommand, "Put the zone's red portal where you stand (test).") &&
		context->RegisterConsoleCommand("dd-zone-boss", BossCommand, "Spawn the linear zone's boss next to you (test).");
}

void OnLevelChanged(const D2RL::PluginContext* context, int levelId) noexcept {
	const std::lock_guard lock(g_mutex);
	++g_visit;
	g_level = levelId;
	g_travelling = false;
	g_portalUp = false;
	if (g_threads == nullptr) {
		return;
	}
	if (levelId == TownLevelId) {
		Later(context, TownDelayMs, PlacePortal);
	} else if (levelId == ZoneLevelId) {
		Later(context, ZoneDelayMs, PlaceHero);
	}
}

void OnGameLeft() noexcept {
	const std::lock_guard lock(g_mutex);
	++g_visit;
	g_level = -1;
	g_portalUp = g_travelling = false;
	// A new game makes a new zone with new monsters: the boss comes out again.
	g_bossOut = g_bossSeen = g_cleared = false;
	g_bossTries = 0;
	g_bossId = 0;
}

}  // namespace Zone
