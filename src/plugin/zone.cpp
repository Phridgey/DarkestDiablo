// The Tower zone (prototype). See zone.h and docs/linear-zone.md.
//
// Town: about 1.5 s after the hero arrives, a red portal (objects.txt row 60, PortalPermanent: the
// red portal that stays open, as to the Cow Level; it has no destination of its own) is put up a few steps from the fire.
// "cheat ospawn" places objects at the hero's feet, so the hero is moved there and straight back
// ("location"), as the traveler's portal does (campfire.cpp). When the hero walks into the portal
// the plugin sends them to the zone with "goto". The game can't make a real linked portal.
//
// Zone (the Tower's last floor, level 31): the hero arrives in the south loop. Packs of Tower
// monsters come out as the hero nears each pack spot (the game only spawns near the hero), and
// an elite (a superunique of our own, src/patches/120_linear_zone.py) in each side room. Each
// elite drops a Tower seal. The north room starts empty except for the altar (the Tower's tome).
// Reaching it with both seals takes them and brings out Blood Raven; his death gives the Town
// Portal scroll.
//
// Seals: once a second the plugin looks through everything the hero owns. A seal in the stash
// or the Cube vanishes (the game can't refuse an item in the stash), and so does any seal the
// hero didn't pick up in this game (they were carried out of an earlier game: the game saves
// items on quitting, before the plugin could take them).
//
// Polling runs on one background thread every 200 ms with the game-state API (gameapi.h);
// console commands run on the UI thread and item work on the game thread.
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
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace Zone {

namespace {

// ---- Numbers to tune after the first test ------------------------------------------------
constexpr int ZoneLevelId   = 31;      // ZONE_ID in src/patches/120_linear_zone.py ("Act 1 - Jail 3")
constexpr int TownLevelId   = 1;       // Rogue Encampment
constexpr int OriginX       = 3500;    // levels.txt OffsetX/Y of level 31 (kept by the patch)
constexpr int OriginY       = 1600;

struct Cell { int x, y; };             // a map cell (tile) of src/maps/tower_zone.json (31 x 31)
constexpr Cell Entry        = { 13, 26 };  // the south loop: where the hero arrives
constexpr Cell Altar        = { 13, 6 };   // the middle of the north room
constexpr Cell RavenAt      = { 13, 4 };   // where Blood Raven comes out
constexpr Cell EliteAt[2]   = { { 5, 21 }, { 22, 21 } };  // the west and east side rooms
struct Pack { Cell at; const char* monster; int count; };
constexpr Pack Packs[] = {
	{ { 13, 22 }, "dd_tower_fallen3",   5 },   // the hall, south end
	{ { 13, 16 }, "dd_tower_goatman3",  4 },   // the hall, north end
	{ { 13, 18 }, "dd_tower_cr_archer3", 3 },
	{ { 6, 7 },   "dd_tower_wraith1",   3 },   // the north-west wing
	{ { 18, 7 },  "dd_tower_wraith1",   3 },   // the north-east wing
	{ { 4, 20 },  "dd_tower_fallen3",   4 },   // with the west elite
	{ { 23, 20 }, "dd_tower_goatman3",  3 },   // with the east elite
};
constexpr int PackCount     = static_cast<int>(sizeof(Packs) / sizeof(Packs[0]));
constexpr int SpawnReach    = 45;      // sub-tiles: a pack or elite comes out when the hero is this close
constexpr int AltarShow     = 60;      // sub-tiles: the altar is put up when the hero is this close
constexpr int AltarReach    = 4;       // sub-tiles: the hero at the altar ("clicking" it walks them there)
constexpr int AltarRow      = 8;       // objects.txt row: TowerTome (the Tower's book on a stand)
constexpr uint32_t RavenRow = 267;     // monstats row of bloodraven
constexpr int PortalRow     = 60;      // objects.txt row: PortalPermanent (red, stays open; CainPortal 189 closed after a few seconds)
constexpr int FireRow       = 39;      // objects.txt row: the town fire
constexpr int PortalFromFireX = -9;    // where the red portal stands, in sub-tiles from the fire
constexpr int PortalFromFireY = 6;
constexpr int PortalReach   = 4;       // the hero this close to the portal (sub-tiles) steps through
constexpr int PortalArm     = 8;       // ...after having been at least this far from it once
constexpr int TownDelayMs   = 1500;    // the town must be loaded before anything is spawned
constexpr int ZoneDelayMs   = 1000;

const D2RL::ThreadService*    g_threads   = nullptr;
const D2RL::InventoryService* g_inventory = nullptr;
const D2RL::ItemService*      g_items     = nullptr;
GiveFn                        g_give      = nullptr;
std::mutex                    g_mutex;    // guards everything below

// From loot_data.txt ("towerelite <n> <superunique row> <seal code>", 120_linear_zone.py).
int      g_eliteRow[2] = { -1, -1 };
uint32_t g_sealCode[2] = { 0, 0 };

int  g_level = -1;
// Town
bool g_portalUp = false;
int  g_portalX = 0, g_portalY = 0;
bool g_armed = false;
bool g_travelling = false;
// Zone (kept for the whole game: the level keeps its monsters while the game lasts)
bool g_packOut[PackCount] {};
bool g_eliteOut[2] {};
bool g_altarUp = false;
int  g_altarX = 0, g_altarY = 0;
bool g_altarArmed = true;              // reset once the hero steps away from the altar
bool g_ravenOut = false, g_ravenSeen = false, g_cleared = false;
// Seals the hero picked up in this game (any other seal vanishes)
bool g_sealEarned[2] {};
int  g_tick = 0;
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

// A red portal already standing within `radius` sub-tiles of (x, y): the town keeps it while loaded.
auto FindPortal(int x, int y, int radius, int& foundX, int& foundY) -> bool {
	for (const Game::Unit& o : Game::Objects(-1, static_cast<uint32_t>(PortalRow))) {
		if (o.placed && Distance(o.x, o.y, x, y) <= radius) {
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
	RunConsole(context, "cheat ospawn 60");  // objects.txt row 60: PortalPermanent (red, stays open)
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
		context->LogWarn("Zone: the red portal did not appear (\"cheat ospawn 60\" refused?).");
	}
}

void __cdecl Travel(const D2RL::PluginContext* context, void*) noexcept {
	char command[32];
	std::snprintf(command, sizeof(command), "goto %d", ZoneLevelId);
	RunConsole(context, command);
}

// ---- Zone: arrival, packs, elites, altar, Blood Raven ------------------------------------

auto CellX(const Cell& c) noexcept -> int { return SubTile(OriginX, c.x); }
auto CellY(const Cell& c) noexcept -> int { return SubTile(OriginY, c.y); }

void __cdecl PlaceHero(const D2RL::PluginContext* context, void* visit) noexcept {
	std::unique_lock lock(g_mutex);
	if (reinterpret_cast<uintptr_t>(visit) != g_visit.load() || g_level != ZoneLevelId) {
		return;
	}
	const Game::Unit hero = Game::Hero();
	const int x = CellX(Entry), y = CellY(Entry);
	if (hero && Distance(hero.x, hero.y, x, y) <= 15) {
		Log(context, "Zone: arrived at %d,%d, already at the entry.", hero.x, hero.y);
		return;
	}
	Log(context, "Zone: arrived at %d,%d; moving to the entry (%d,%d).", hero ? hero.x : 0, hero ? hero.y : 0, x, y);
	lock.unlock();
	char command[64];
	std::snprintf(command, sizeof(command), "location %d,%d", x, y);
	RunConsole(context, command);
}

// UI thread. `what` encodes the job: pack k = k, elite n = 100 + n, Blood Raven = 200.
void __cdecl Spawn(const D2RL::PluginContext* context, void* what) noexcept {
	const int job = static_cast<int>(reinterpret_cast<uintptr_t>(what));
	char command[96] {};
	if (job < PackCount) {
		const Pack& p = Packs[job];
		std::snprintf(command, sizeof(command), "cheat mspawn %s,%d @ %d,%d", p.monster, p.count, CellX(p.at), CellY(p.at));
	} else if (job < 200) {
		const int n = job - 100;
		if (g_eliteRow[n] < 0) {
			context->LogWarn("Zone: the elites' rows are missing from loot_data.txt; rebuild the mod.");
			return;
		}
		std::snprintf(command, sizeof(command), "cheat monster super:%d @ %d,%d", g_eliteRow[n], CellX(EliteAt[n]), CellY(EliteAt[n]));
	} else {
		std::snprintf(command, sizeof(command), "cheat mspawn bloodraven,1 @ %d,%d", CellX(RavenAt), CellY(RavenAt));
	}
	RunConsole(context, command);
}

// UI thread. The altar: hero there, spawn, hero back (as the red portal).
void __cdecl PlaceAltar(const D2RL::PluginContext* context, void*) noexcept {
	const Game::Unit hero = Game::Hero();
	if (!hero) {
		return;
	}
	char command[64];
	std::snprintf(command, sizeof(command), "location %d,%d", CellX(Altar), CellY(Altar));
	RunConsole(context, command);
	std::snprintf(command, sizeof(command), "cheat ospawn %d", AltarRow);
	RunConsole(context, command);
	std::snprintf(command, sizeof(command), "location %d,%d", hero.x, hero.y);
	RunConsole(context, command);
}

void __cdecl GiveScroll(const D2RL::PluginContext* context, void*) noexcept {
	if (g_give != nullptr) {
		g_give(context);
	}
}

// ---- Seals -------------------------------------------------------------------------------

auto SealIndex(uint32_t code) noexcept -> int {
	for (int n = 0; n < 2; ++n) {
		if (g_sealCode[n] != 0 && g_sealCode[n] == code) {
			return n;
		}
	}
	return -1;
}

struct Found { D2RL::ItemHandle item; int seal; D2RL::Items::ItemContainer where; };

auto __cdecl CollectSeals(const D2RL::PluginContext*, const D2RL::Items::ItemInfo* item, void* out) noexcept -> D2RL::Inventory::IterationAction {
	const int seal = item != nullptr ? SealIndex(item->code) : -1;
	if (seal >= 0) {
		static_cast<std::vector<Found>*>(out)->push_back({ item->handle, seal, item->container });
	}
	return D2RL::Inventory::IterationAction::Continue;
}

auto FindSeals(const D2RL::PluginContext* context, D2RL::PlayerHandle& player) -> std::vector<Found> {
	std::vector<Found> found;
	if (g_inventory == nullptr || g_inventory->getLocalPlayer(context, &player) != D2RL::Inventory::Result::Success) {
		return found;
	}
	const D2RL::Inventory::ItemFilter filter {
		.structSize    = D2RL::Inventory::ItemFilterSize,
		.containerMask = D2RL::Items::AllItemContainers,
	};
	g_inventory->forEachInventoryItem(context, player, &filter, CollectSeals, &found);
	return found;
}

void Destroy(const D2RL::PluginContext* context, D2RL::PlayerHandle player, const Found& f, const char* why) noexcept {
	const auto result = g_items->destroyItem(context, player, f.item, D2RL::Items::SocketedItemPolicy::DestroyContents);
	Log(context, "Zone: seal %d (container %u) vanishes: %s -> %s.", f.seal + 1, static_cast<unsigned>(f.where), why,
		result == D2RL::Items::Result::Success ? "done" : "failed");
}

// A seal put in the stash or Cube jumps back to the inventory (the game can't refuse it there).
// No room: it stays until there is, and is tried again every second.
void MoveBack(const D2RL::PluginContext* context, D2RL::PlayerHandle player, const Found& f) noexcept {
	D2RL::Items::ExistingItemOperation op {};
	op.structSize = D2RL::Items::ExistingItemOperationSize;
	op.kind = D2RL::Items::ExistingItemOperationKind::Move;
	op.item = f.item;
	op.move.destination = {
		.structSize = D2RL::Items::ItemDestinationSize,
		.container  = D2RL::Items::ItemContainer::Inventory,
		.placement  = D2RL::Items::Placement::Automatic,
	};
	const D2RL::Items::ExistingItemTransaction transaction {
		.structSize     = D2RL::Items::ExistingItemTransactionSize,
		.player         = player,
		.operationCount = 1,
		.operations     = &op,
	};
	D2RL::Items::ExistingItemTransactionResult result { .structSize = D2RL::Items::ExistingItemTransactionResultSize };
	const auto r = g_items->executeExistingItemTransaction(context, &transaction, &result);
	static int s_lastFailure = -1;  // log a failure once, not every second
	if (r == D2RL::Items::Result::Success) {
		Log(context, "Zone: seal %d jumped back from container %u to the inventory.", f.seal + 1, static_cast<unsigned>(f.where));
		s_lastFailure = -1;
	} else if (s_lastFailure != static_cast<int>(r)) {
		s_lastFailure = static_cast<int>(r);
		Log(context, "Zone: seal %d in container %u can't go back to the inventory yet (code %d; no room?).", f.seal + 1,
			static_cast<unsigned>(f.where), static_cast<int>(r));
	}
}

// Game thread, once a second.
void __cdecl CheckSeals(const D2RL::PluginContext* context, void*) noexcept {
	if (g_items == nullptr || g_sealCode[0] == 0) {
		return;
	}
	D2RL::PlayerHandle player = D2RL::InvalidPlayerHandle;
	const std::vector<Found> found = FindSeals(context, player);
	using C = D2RL::Items::ItemContainer;
	bool kept[2] = { false, false };  // one of each seal at most: any further copy vanishes
	for (const Found& f : found) {
		if (f.where == C::Cursor || f.where == C::Ground) {
			continue;  // in the hand or on the floor: wait until it is put somewhere
		}
		bool earned = false;
		{
			const std::lock_guard lock(g_mutex);
			if (!g_sealEarned[f.seal] && g_level == ZoneLevelId && f.where == C::Inventory) {
				g_sealEarned[f.seal] = true;  // picked up here, in this game
				Log(context, "Zone: seal %d picked up.", f.seal + 1);
			}
			earned = g_sealEarned[f.seal];
		}
		if (!earned) {
			Destroy(context, player, f, "carried over from an earlier game");
		} else if (kept[f.seal]) {
			Destroy(context, player, f, "only one of each seal");
		} else if (f.where == C::PersonalStash || f.where == C::SharedStash || f.where == C::Cube || f.where == C::CustomPage) {
			kept[f.seal] = true;
			MoveBack(context, player, f);
		} else {
			kept[f.seal] = true;
		}
	}
}

// Game thread. The hero is at the altar: with both seals in the inventory, take them and bring
// out Blood Raven.
void __cdecl UseAltar(const D2RL::PluginContext* context, void*) noexcept {
	D2RL::PlayerHandle player = D2RL::InvalidPlayerHandle;
	const std::vector<Found> found = FindSeals(context, player);
	const Found* have[2] = { nullptr, nullptr };
	for (const Found& f : found) {
		if (f.where == D2RL::Items::ItemContainer::Inventory) {
			have[f.seal] = &f;
		}
	}
	if (have[0] == nullptr || have[1] == nullptr) {
		Log(context, "Zone: altar used without both seals (west %s, east %s).", have[0] ? "yes" : "no", have[1] ? "yes" : "no");
		context->WriteConsoleMessage("The altar needs both Tower seals.");
		return;
	}
	Destroy(context, player, *have[0], "given to the altar");
	Destroy(context, player, *have[1], "given to the altar");
	{
		const std::lock_guard lock(g_mutex);
		g_ravenOut = true;
	}
	Log(context, "Zone: both seals given to the altar; Blood Raven comes out.");
	g_threads->runOnUiThread(context, Spawn, reinterpret_cast<void*>(static_cast<uintptr_t>(200)));
}

// ---- The poll ----------------------------------------------------------------------------

void TickTown(const D2RL::PluginContext* context, const Game::Unit& hero) {
	if (!g_portalUp || g_travelling) {
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
}

void TickZone(const D2RL::PluginContext* context, const Game::Unit& hero) {
	auto close = [&](const Cell& c, int reach) { return Distance(hero.x, hero.y, CellX(c), CellY(c)) <= reach; };
	for (int k = 0; k < PackCount; ++k) {
		if (!g_packOut[k] && close(Packs[k].at, SpawnReach)) {
			g_packOut[k] = true;
			Log(context, "Zone: pack %d comes out (%d %s).", k + 1, Packs[k].count, Packs[k].monster);
			g_threads->runOnUiThread(context, Spawn, reinterpret_cast<void*>(static_cast<uintptr_t>(k)));
		}
	}
	for (int n = 0; n < 2; ++n) {
		if (!g_eliteOut[n] && close(EliteAt[n], SpawnReach)) {
			g_eliteOut[n] = true;
			Log(context, "Zone: elite %d comes out.", n + 1);
			g_threads->runOnUiThread(context, Spawn, reinterpret_cast<void*>(static_cast<uintptr_t>(100 + n)));
		}
	}
	if (!g_altarUp && close(Altar, AltarShow)) {
		g_altarUp = true;
		g_altarX = CellX(Altar), g_altarY = CellY(Altar);
		Log(context, "Zone: the altar goes up at %d,%d.", g_altarX, g_altarY);
		g_threads->runOnUiThread(context, PlaceAltar, nullptr);
		return;
	}
	if (g_altarUp && !g_ravenOut) {
		// Where the altar really stands (a blocked spot moves it).
		for (const Game::Unit& o : Game::Objects(-1, static_cast<uint32_t>(AltarRow))) {
			if (o.placed && Distance(o.x, o.y, CellX(Altar), CellY(Altar)) <= 15) {
				g_altarX = o.x, g_altarY = o.y;
				break;
			}
		}
		const double d = Distance(hero.x, hero.y, g_altarX, g_altarY);
		if (!g_altarArmed && d > AltarReach + 4) {
			g_altarArmed = true;
		} else if (g_altarArmed && d <= AltarReach) {
			g_altarArmed = false;  // once per visit to the altar
			g_threads->runOnGameThread(context, UseAltar, nullptr);
		}
	}
	if (g_ravenOut && !g_cleared) {
		bool found = false, alive = false;
		for (const Game::MonsterDetails& m : Game::Monsters(-1, false)) {
			if (m.unit.row == RavenRow) {
				found = true, alive = m.alive;
				break;
			}
		}
		if (found && !g_ravenSeen) {
			g_ravenSeen = true;
			Log(context, "Zone: Blood Raven is out.");
		}
		if (g_ravenSeen && found && !alive) {
			g_cleared = true;
			Log(context, "Zone: Blood Raven is dead; cleared.");
			g_threads->runOnUiThread(context, GiveScroll, nullptr);
		}
	}
}

// Background thread, every 200 ms.
void Tick(const D2RL::PluginContext* context) {
	std::unique_lock lock(g_mutex);
	if (g_level < 0) {
		return;
	}
	if (++g_tick % 5 == 0 && g_items != nullptr) {
		g_threads->runOnGameThread(context, CheckSeals, nullptr);
	}
	const Game::Unit hero = Game::Hero();
	if (!hero) {
		return;
	}
	if (g_level == TownLevelId) {
		TickTown(context, hero);
	} else if (g_level == ZoneLevelId) {
		TickZone(context, hero);
	}
}

void ReadEliteRows(const D2RL::PluginContext* context) noexcept {
	namespace fs = std::filesystem;
	try {
		std::vector<fs::path> places;  // the same places as ReadLootData (darkestdiablo_plugin.cpp)
		if (context->pluginDirectory != nullptr) {
			places.push_back(fs::path(context->pluginDirectory).parent_path());
			places.push_back(fs::path(context->pluginDirectory));
		}
		if (context->modDirectory != nullptr) {
			places.push_back(fs::path(context->modDirectory) / "d2rloader");
			places.push_back(fs::path(context->modDirectory).parent_path() / "d2rloader");
		}
		std::error_code ec;
		for (const auto& place : places) {
			if (!fs::exists(place / "loot_data.txt", ec)) {
				continue;
			}
			std::ifstream in(place / "loot_data.txt");
			std::string line;
			while (std::getline(in, line)) {
				std::istringstream words(line);
				std::string kind, code;
				int n = -1, row = -1;
				if (words >> kind && kind == "towerelite" && words >> n >> row >> code && n >= 0 && n < 2 && code.size() >= 2 && code.size() <= 4) {
					char c[4] = { ' ', ' ', ' ', ' ' };
					for (size_t i = 0; i < code.size(); ++i) {
						c[i] = code[i];
					}
					g_eliteRow[n] = row;
					g_sealCode[n] = D2RL::Items::MakeItemCode(c[0], c[1], c[2], c[3]);
				}
			}
			break;
		}
	} catch (...) {
	}
	Log(context, "Zone: elite rows %d and %d.", g_eliteRow[0], g_eliteRow[1]);
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
	RunConsole(command->plugin, "cheat ospawn 60");
	const GameMemory::Pos fire = FirePos();
	char line[128];
	std::snprintf(line, sizeof(line), "Red portal at %d,%d; from the fire: %d,%d.", hero.x, hero.y,
		fire.ok ? hero.x - fire.x : 0, fire.ok ? hero.y - fire.y : 0);
	command->plugin->WriteConsoleMessage(line);
	command->plugin->LogInfo(line);
	return D2RL::ConsoleCommandResult::Handled;
}

// Spawns Blood Raven now (test), without the seals.
auto BossCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	{
		const std::lock_guard lock(g_mutex);
		if (g_level != ZoneLevelId) {
			command->plugin->WriteConsoleMessage("Stand in the Tower zone first.");
			return D2RL::ConsoleCommandResult::Failed;
		}
		g_ravenOut = true, g_ravenSeen = false, g_cleared = false;
	}
	Spawn(command->plugin, reinterpret_cast<void*>(static_cast<uintptr_t>(200)));
	return D2RL::ConsoleCommandResult::Handled;
}

// Prints the hero's map cell in the zone, and where the seals are (test).
auto SealsCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	const Game::Unit hero = Game::Hero();
	char line[160];
	std::snprintf(line, sizeof(line), "Hero at %d,%d (zone cell %d,%d). Seals picked up this game: west %s, east %s. Elite rows %d, %d.",
		hero.x, hero.y, hero.x / 5 - OriginX, hero.y / 5 - OriginY, g_sealEarned[0] ? "yes" : "no", g_sealEarned[1] ? "yes" : "no",
		g_eliteRow[0], g_eliteRow[1]);
	command->plugin->WriteConsoleMessage(line);
	command->plugin->LogInfo(line);
	return D2RL::ConsoleCommandResult::Handled;
}

}  // namespace

auto Load(const D2RL::PluginContext* context, GiveFn giveTownPortal) noexcept -> bool {
	if (context->QueryService(&g_threads) != D2RL::ServiceQueryResult::Success || !D2RL::HasThreadServiceField(g_threads, D2RL::ThreadServiceRequiredSize)) {
		context->LogWarn("Zone: thread service missing; the zone is off.");
		g_threads = nullptr;
		return true;  // the rest of the mod still works
	}
	if (context->QueryService(&g_inventory) != D2RL::ServiceQueryResult::Success || !D2RL::HasInventoryServiceField(g_inventory, D2RL::InventoryServiceRequiredSize) ||
		context->QueryService(&g_items) != D2RL::ServiceQueryResult::Success || !D2RL::HasItemServiceField(g_items, D2RL::ItemServiceRequiredSize)) {
		context->LogWarn("Zone: item services missing; the seals can't be checked.");
		g_inventory = nullptr, g_items = nullptr;
	}
	g_give = giveTownPortal;
	ReadEliteRows(context);
	std::thread([context] {
		for (;;) {
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			Tick(context);
		}
	}).detach();
	return context->RegisterConsoleCommand("dd-zone", ZoneCommand, "Go to the Tower zone (test).") &&
		context->RegisterConsoleCommand("dd-zone-portal", PortalCommand, "Put the zone's red portal where you stand (test).") &&
		context->RegisterConsoleCommand("dd-zone-boss", BossCommand, "Bring out Blood Raven now, without the seals (test).") &&
		context->RegisterConsoleCommand("dd-zone-seals", SealsCommand, "Show where you stand in the Tower zone and which seals you picked up (test).");
}

void OnLevelChanged(const D2RL::PluginContext* context, int levelId) noexcept {
	const std::lock_guard lock(g_mutex);
	++g_visit;
	g_level = levelId;
	g_travelling = false;
	g_portalUp = false;
	g_altarArmed = true;
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
	// A new game makes the zone fresh: everything comes out again, and seals must be won again.
	for (bool& b : g_packOut) b = false;
	for (bool& b : g_eliteOut) b = false;
	for (bool& b : g_sealEarned) b = false;
	g_altarUp = false, g_altarArmed = true;
	g_ravenOut = g_ravenSeen = g_cleared = false;
}

}  // namespace Zone
