// Andariel (the act's last stop). See andariel.h.
//
// Level 37 (Catacombs level 4) on Blizzard's map of her lair, without its monsters and doors
// (src/maps/andariel.json). The hero arrives by the stairs in the east room. As the hero nears
// each spot, a pack of Catacombs monsters comes out (the game only spawns near the hero), and so
// does each of the two uniques with a small escort: one in the south room, one at the far west of
// the hall below her throne. Each unique drops one of her keys (125_andariel.py). Walking into her
// hall with both keys in the inventory takes them and brings Andariel out at the far end.
//
// Keys: as the Tower's seals (zone.cpp), once a second the plugin looks through everything the hero
// owns. A key put in the stash or the Cube jumps back to the inventory; a second copy vanishes; and
// a key the hero didn't pick up in this game vanishes (carried out of an earlier game).
//
// Polling runs on one background thread every 200 ms with the game-state API (gameapi.h);
// console commands run on the UI thread and item work on the game thread.
#include "andariel.h"
#include "gameapi.h"
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

namespace Andariel {

namespace {

// ---- Spots and numbers (map cells of src/maps/andariel.json, 23 x 32; not yet checked in game) ----
constexpr int LevelId       = 37;      // ARENA_ID in src/patches/125_andariel.py
constexpr int OriginX       = 4500;    // levels.txt OffsetX/Y of level 37
constexpr int OriginY       = 1900;

struct Cell { int x, y; };
constexpr Cell Entry        = { 19, 25 };  // the east room, below the stairs
constexpr Cell HallDoor     = { 9, 13 };   // inside her hall: reaching it with both keys brings her out
constexpr Cell AndarielAt   = { 9, 4 };    // the far (north) end of her hall
struct Keeper { Cell at; const char* type; const char* escort; int escortCount; };
constexpr Keeper Keepers[2] = {
	{ { 9, 28 }, "dd_andy_fallenshaman4_k1", "fallen4", 4 },  // the south room
	{ { 3, 17 }, "dd_andy_arach1_k2",        "arach1",  3 },  // the west end of the hall below her throne
};
struct Pack { Cell at; const char* monster; int count; };
constexpr Pack Packs[] = {
	{ { 12, 25 }, "zombie3",  5 },   // the south room, east side
	{ { 13, 29 }, "bighead2", 4 },   // the south room, south-east corner
	{ { 9, 20 },  "zombie3",  4 },   // the passage up to the hall
	{ { 12, 17 }, "vampire5", 2 },   // the hall below her throne
	{ { 16, 18 }, "bighead1", 4 },   // the hall's east alcove
};
constexpr int PackCount     = static_cast<int>(sizeof(Packs) / sizeof(Packs[0]));
constexpr int SpawnReach    = 50;      // sub-tiles: a pack or unique comes out when the hero is this close
constexpr int HallReach     = 20;      // sub-tiles from HallDoor: the hero is in her hall
constexpr int ArriveDelayMs = 1000;

const D2RL::ThreadService*    g_threads   = nullptr;
const D2RL::InventoryService* g_inventory = nullptr;
const D2RL::ItemService*      g_items     = nullptr;
std::mutex                    g_mutex;    // guards everything below

// From loot_data.txt ("andykey <n> <code>", 125_andariel.py; "monclass <Id> <class>", 999).
uint32_t g_keyCode[2] = { 0, 0 };
int      g_keeperClass[2] = { -1, -1 };

int  g_level = -1;
bool g_open = false;                   // the route lets this hero fight here
bool g_packOut[PackCount] {};
bool g_keeperOut[2] {};
bool g_andyOut = false;
bool g_hallArmed = true;               // reset once the hero steps out of her hall
bool g_keyEarned[2] {};
bool g_keeperSeen[2] {};               // the unique was seen alive in this visit
bool g_keyGiven[2] {};                 // its key was handed over at its death (2026-11-22)
int  g_keeperTries[2] {};              // spawn attempts so far
int  g_tick = 0;
std::atomic<uintptr_t> g_visit { 1 };

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
		context->LogWarn("Andariel: ExecuteConsoleCommand not found in D2RCore.");
		return false;
	}
	const bool ok = execute(command);
	Log(context, "Andariel: console %s -> %s", command, ok ? "sent" : "refused");
	return ok;
}

auto CellX(const Cell& c) noexcept -> int { return (OriginX + c.x) * 5 + 2; }
auto CellY(const Cell& c) noexcept -> int { return (OriginY + c.y) * 5 + 2; }

auto Distance(int ax, int ay, int bx, int by) noexcept -> double {
	const double dx = ax - bx, dy = ay - by;
	return std::sqrt(dx * dx + dy * dy);
}

void Later(const D2RL::PluginContext* context, int delayMs, D2RL::Threads::Callback callback) noexcept {
	const uintptr_t visit = g_visit.load();
	std::thread([context, delayMs, callback, visit] {
		std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
		if (visit == g_visit.load() && g_threads != nullptr) {
			g_threads->runOnUiThread(context, callback, reinterpret_cast<void*>(visit));
		}
	}).detach();
}

// UI thread. The hero arrives wherever "goto" puts them; move them to the east room.
void __cdecl PlaceHero(const D2RL::PluginContext* context, void* visit) noexcept {
	std::unique_lock lock(g_mutex);
	if (reinterpret_cast<uintptr_t>(visit) != g_visit.load() || g_level != LevelId) {
		return;
	}
	const Game::Unit hero = Game::Hero();
	const int x = CellX(Entry), y = CellY(Entry);
	if (hero && Distance(hero.x, hero.y, x, y) <= 25) {
		return;
	}
	lock.unlock();
	char command[64];
	std::snprintf(command, sizeof(command), "location %d,%d", x, y);
	RunConsole(context, command);
}

// UI thread. `what`: pack k = k, unique n = 100 + n, Andariel = 200.
void __cdecl Spawn(const D2RL::PluginContext* context, void* what) noexcept {
	const int job = static_cast<int>(reinterpret_cast<uintptr_t>(what));
	char command[96] {};
	if (job < PackCount) {
		const Pack& p = Packs[job];
		std::snprintf(command, sizeof(command), "cheat mspawn %s,%d @ %d,%d", p.monster, p.count, CellX(p.at), CellY(p.at));
		RunConsole(context, command);
	} else if (job < 200) {
		const Keeper& k = Keepers[job - 100];
		std::snprintf(command, sizeof(command), "cheat mspawn %s,%d @ %d,%d", k.escort, k.escortCount, CellX(k.at), CellY(k.at));
		RunConsole(context, command);
		if (g_keeperClass[job - 100] < 0) {
			context->LogWarn("Andariel: the uniques' classes are missing from loot_data.txt; rebuild the mod.");
			return;
		}
		// The console takes a unique's class number; its own minions would drop nothing useful.
		RunConsole(context, "cheat nominions 1");
		std::snprintf(command, sizeof(command), "cheat monster unique:%d @ %d,%d", g_keeperClass[job - 100], CellX(k.at), CellY(k.at));
		RunConsole(context, command);
		RunConsole(context, "cheat resetnominions");
	} else {
		std::snprintf(command, sizeof(command), "cheat mspawn andariel,1 @ %d,%d", CellX(AndarielAt), CellY(AndarielAt));
		RunConsole(context, command);
	}
}

// ---- Keys --------------------------------------------------------------------------------

auto KeyIndex(uint32_t code) noexcept -> int {
	for (int n = 0; n < 2; ++n) {
		if (g_keyCode[n] != 0 && g_keyCode[n] == code) {
			return n;
		}
	}
	return -1;
}

struct Found { D2RL::ItemHandle item; int key; D2RL::Items::ItemContainer where; };

auto __cdecl CollectKeys(const D2RL::PluginContext*, const D2RL::Items::ItemInfo* item, void* out) noexcept -> D2RL::Inventory::IterationAction {
	const int key = item != nullptr ? KeyIndex(item->code) : -1;
	if (key >= 0) {
		static_cast<std::vector<Found>*>(out)->push_back({ item->handle, key, item->container });
	}
	return D2RL::Inventory::IterationAction::Continue;
}

auto FindKeys(const D2RL::PluginContext* context, D2RL::PlayerHandle& player) -> std::vector<Found> {
	std::vector<Found> found;
	if (g_inventory == nullptr || g_inventory->getLocalPlayer(context, &player) != D2RL::Inventory::Result::Success) {
		return found;
	}
	const D2RL::Inventory::ItemFilter filter {
		.structSize    = D2RL::Inventory::ItemFilterSize,
		.containerMask = D2RL::Items::AllItemContainers,
	};
	g_inventory->forEachInventoryItem(context, player, &filter, CollectKeys, &found);
	return found;
}

void Destroy(const D2RL::PluginContext* context, D2RL::PlayerHandle player, const Found& f, const char* why) noexcept {
	const auto result = g_items->destroyItem(context, player, f.item, D2RL::Items::SocketedItemPolicy::DestroyContents);
	Log(context, "Andariel: key %d (container %u) vanishes: %s -> %s.", f.key + 1, static_cast<unsigned>(f.where), why,
		result == D2RL::Items::Result::Success ? "done" : "failed");
}

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
	g_items->executeExistingItemTransaction(context, &transaction, &result);
}

// Game thread: a unique died; its key goes straight into the inventory. Its loot drops it too,
// but that drop failed in play (2026-11-22); a second copy vanishes in CheckKeys.
void __cdecl GiveKey(const D2RL::PluginContext* context, void* which) noexcept {
	const int n = static_cast<int>(reinterpret_cast<uintptr_t>(which));
	if (g_items == nullptr || g_inventory == nullptr || n < 0 || n > 1 || g_keyCode[n] == 0) {
		return;
	}
	D2RL::PlayerHandle player = D2RL::InvalidPlayerHandle;
	for (const Found& f : FindKeys(context, player)) {
		if (f.key == n) {
			return;  // already has it (picked up from the drop)
		}
	}
	if (player == D2RL::InvalidPlayerHandle && g_inventory->getLocalPlayer(context, &player) != D2RL::Inventory::Result::Success) {
		return;
	}
	D2RL::Items::ItemCreateSpec spec {
		.structSize      = D2RL::Items::ItemCreateSpecSize,
		.code            = g_keyCode[n],
		.quality         = D2RL::Items::Quality::Normal,
		.qualityRecordId = D2RL::Items::RandomQualityRecord,
		.itemLevel       = 1,
		.seedMode        = D2RL::Items::SeedMode::Random,
		.quantity        = D2RL::Items::DefaultValue,
		.durability      = D2RL::Items::DefaultValue,
		.destination     = {
			.structSize = D2RL::Items::ItemDestinationSize,
			.container  = D2RL::Items::ItemContainer::Inventory,
			.placement  = D2RL::Items::Placement::Automatic,
		},
	};
	D2RL::ItemHandle item = D2RL::InvalidItemHandle;
	{
		const std::lock_guard lock(g_mutex);
		g_keyEarned[n] = true;
	}
	const auto result = g_items->createItem(context, player, &spec, &item);
	Log(context, "Andariel: key %d handed over -> %s (code %d).", n + 1, result == D2RL::Items::Result::Success ? "done" : "failed",
		static_cast<int>(result));
	if (result == D2RL::Items::Result::Success) {
		context->WriteConsoleMessage(n == 0 ? "You take the Vial of Maiden's Venom." : "You take the Shard of Anguish.");
	}
}

// UI thread: a unique that didn't come out (a crowded or blocked spot) is spawned next to the hero.
void __cdecl RespawnKeeper(const D2RL::PluginContext* context, void* which) noexcept {
	const int n = static_cast<int>(reinterpret_cast<uintptr_t>(which));
	const Game::Unit hero = Game::Hero();
	if (!hero || n < 0 || n > 1 || g_keeperClass[n] < 0) {
		return;
	}
	char command[96] {};
	RunConsole(context, "cheat nominions 1");
	std::snprintf(command, sizeof(command), "cheat monster unique:%d @ %d,%d", g_keeperClass[n], hero.x + (n == 0 ? 8 : -8), hero.y + 8);
	RunConsole(context, command);
	RunConsole(context, "cheat resetnominions");
}

// Game thread, once a second.
void __cdecl CheckKeys(const D2RL::PluginContext* context, void*) noexcept {
	if (g_items == nullptr || g_keyCode[0] == 0) {
		return;
	}
	D2RL::PlayerHandle player = D2RL::InvalidPlayerHandle;
	const std::vector<Found> found = FindKeys(context, player);
	using C = D2RL::Items::ItemContainer;
	bool kept[2] = { false, false };
	for (const Found& f : found) {
		if (f.where == C::Cursor || f.where == C::Ground) {
			continue;
		}
		bool earned = false;
		{
			const std::lock_guard lock(g_mutex);
			if (!g_keyEarned[f.key] && g_level == LevelId && f.where == C::Inventory) {
				g_keyEarned[f.key] = true;
				Log(context, "Andariel: key %d picked up.", f.key + 1);
			}
			earned = g_keyEarned[f.key];
		}
		if (!earned) {
			Destroy(context, player, f, "carried over from an earlier game");
		} else if (kept[f.key]) {
			Destroy(context, player, f, "only one of each key");
		} else {
			kept[f.key] = true;
			if (f.where == C::PersonalStash || f.where == C::SharedStash || f.where == C::Cube || f.where == C::CustomPage) {
				MoveBack(context, player, f);
			}
		}
	}
}

// Game thread. The hero walked into her hall: with both keys, take them and bring her out.
void __cdecl EnterHall(const D2RL::PluginContext* context, void*) noexcept {
	D2RL::PlayerHandle player = D2RL::InvalidPlayerHandle;
	const std::vector<Found> found = FindKeys(context, player);
	const Found* have[2] = { nullptr, nullptr };
	for (const Found& f : found) {
		if (f.where == D2RL::Items::ItemContainer::Inventory) {
			have[f.key] = &f;
		}
	}
	if (have[0] == nullptr || have[1] == nullptr) {
		context->WriteConsoleMessage("The Maiden of Anguish waits for her Venom and her Shard.");
		return;
	}
	Destroy(context, player, *have[0], "given up in her hall");
	Destroy(context, player, *have[1], "given up in her hall");
	{
		const std::lock_guard lock(g_mutex);
		g_andyOut = true;
	}
	Log(context, "Andariel: both keys given up; she comes out.");
	g_threads->runOnUiThread(context, Spawn, reinterpret_cast<void*>(static_cast<uintptr_t>(200)));
}

// ---- The poll ----------------------------------------------------------------------------

void TickLair(const D2RL::PluginContext* context, const Game::Unit& hero) {
	auto close = [&](const Cell& c, int reach) { return Distance(hero.x, hero.y, CellX(c), CellY(c)) <= reach; };
	for (int k = 0; k < PackCount; ++k) {
		if (!g_packOut[k] && close(Packs[k].at, SpawnReach)) {
			g_packOut[k] = true;
			g_threads->runOnUiThread(context, Spawn, reinterpret_cast<void*>(static_cast<uintptr_t>(k)));
		}
	}
	for (int n = 0; n < 2; ++n) {
		if (!g_keeperOut[n] && close(Keepers[n].at, SpawnReach)) {
			g_keeperOut[n] = true;
			g_keeperTries[n] = 1;
			Log(context, "Andariel: unique %d comes out.", n + 1);
			g_threads->runOnUiThread(context, Spawn, reinterpret_cast<void*>(static_cast<uintptr_t>(100 + n)));
		}
	}
	// Each unique once out: alive (seen), dead (its key is handed over), or never seen while the hero
	// is near its spot (spawned again next to the hero, every 3 s, up to 3 more times).
	if ((g_tick % 5) == 0 && (g_keeperOut[0] || g_keeperOut[1])) {
		bool alive[2] = { false, false }, dead[2] = { false, false };
		try {
			for (const Game::MonsterDetails& m : Game::Monsters(-1, false)) {
				for (int n = 0; n < 2; ++n) {
					if (g_keeperClass[n] >= 0 && m.unit.row == static_cast<uint32_t>(g_keeperClass[n])) {
						(m.alive ? alive[n] : dead[n]) = true;
					}
				}
			}
		} catch (...) {
		}
		for (int n = 0; n < 2; ++n) {
			if (!g_keeperOut[n] || g_keyGiven[n]) {
				continue;
			}
			if (alive[n]) {
				g_keeperSeen[n] = true;
			} else if (dead[n]) {
				g_keyGiven[n] = true;
				Log(context, "Andariel: unique %d is dead.", n + 1);
				g_threads->runOnGameThread(context, GiveKey, reinterpret_cast<void*>(static_cast<uintptr_t>(n)));
			} else if (!g_keeperSeen[n] && (g_tick % 15) == 0 && g_keeperTries[n] < 4 && close(Keepers[n].at, SpawnReach)) {
				++g_keeperTries[n];
				Log(context, "Andariel: unique %d not found; spawning it again next to the hero.", n + 1);
				g_threads->runOnUiThread(context, RespawnKeeper, reinterpret_cast<void*>(static_cast<uintptr_t>(n)));
			}
		}
	}
	if (g_andyOut) {
		return;
	}
	const bool inHall = close(HallDoor, HallReach);
	if (!g_hallArmed && !inHall) {
		g_hallArmed = true;
	} else if (g_hallArmed && inHall) {
		g_hallArmed = false;  // once per walk into the hall
		g_threads->runOnGameThread(context, EnterHall, nullptr);
	}
}

void Tick(const D2RL::PluginContext* context) {
	std::unique_lock lock(g_mutex);
	if (g_level < 0) {
		return;
	}
	if (++g_tick % 5 == 0 && g_items != nullptr) {
		g_threads->runOnGameThread(context, CheckKeys, nullptr);
	}
	if (g_level != LevelId || !g_open) {
		return;
	}
	const Game::Unit hero = Game::Hero();
	if (hero) {
		TickLair(context, hero);
	}
}

void ReadLootLines(const D2RL::PluginContext* context) noexcept {
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
				std::string kind;
				words >> kind;
				if (kind == "andykey") {
					int n = -1;
					std::string code;
					if (words >> n >> code && n >= 0 && n < 2 && code.size() >= 2 && code.size() <= 4) {
						char c[4] = { ' ', ' ', ' ', ' ' };
						for (size_t i = 0; i < code.size(); ++i) {
							c[i] = code[i];
						}
						g_keyCode[n] = D2RL::Items::MakeItemCode(c[0], c[1], c[2], c[3]);
					}
				} else if (kind == "monclass") {
					std::string id;
					int cls = -1;
					if (words >> id >> cls) {
						for (int n = 0; n < 2; ++n) {
							if (id == Keepers[n].type) {
								g_keeperClass[n] = cls;
							}
						}
					}
				}
			}
			break;
		}
	} catch (...) {
	}
	Log(context, "Andariel: unique classes %d and %d.", g_keeperClass[0], g_keeperClass[1]);
}

// ---- Console -----------------------------------------------------------------------------

auto GoCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	char line[32];
	std::snprintf(line, sizeof(line), "goto %d", LevelId);
	RunConsole(command->plugin, line);
	return D2RL::ConsoleCommandResult::Handled;
}

auto BossCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	{
		const std::lock_guard lock(g_mutex);
		if (g_level != LevelId) {
			command->plugin->WriteConsoleMessage("Stand in Andariel's lair first.");
			return D2RL::ConsoleCommandResult::Failed;
		}
		g_andyOut = true;
	}
	Spawn(command->plugin, reinterpret_cast<void*>(static_cast<uintptr_t>(200)));
	return D2RL::ConsoleCommandResult::Handled;
}

}  // namespace

auto Load(const D2RL::PluginContext* context) noexcept -> bool {
	if (context->QueryService(&g_threads) != D2RL::ServiceQueryResult::Success || !D2RL::HasThreadServiceField(g_threads, D2RL::ThreadServiceRequiredSize)) {
		context->LogWarn("Andariel: thread service missing; her lair stays empty.");
		g_threads = nullptr;
		return true;
	}
	if (context->QueryService(&g_inventory) != D2RL::ServiceQueryResult::Success || !D2RL::HasInventoryServiceField(g_inventory, D2RL::InventoryServiceRequiredSize) ||
		context->QueryService(&g_items) != D2RL::ServiceQueryResult::Success || !D2RL::HasItemServiceField(g_items, D2RL::ItemServiceRequiredSize)) {
		context->LogWarn("Andariel: item services missing; the keys can't be checked.");
		g_inventory = nullptr, g_items = nullptr;
	}
	ReadLootLines(context);
	std::thread([context] {
		for (;;) {
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			Tick(context);
		}
	}).detach();
	return context->RegisterConsoleCommand("dd-andy", GoCommand, "Go to Andariel's lair (test).") &&
		context->RegisterConsoleCommand("dd-andy-boss", BossCommand, "Bring out Andariel now, without the keys (test).");
}

void OnLevelChanged(const D2RL::PluginContext* context, int levelId, bool open) noexcept {
	const std::lock_guard lock(g_mutex);
	++g_visit;
	g_level = levelId;
	g_hallArmed = true;
	if (levelId == LevelId) {
		g_open = open;
		if (g_threads != nullptr) {
			Later(context, ArriveDelayMs, PlaceHero);
		}
	}
}

void OnGameLeft() noexcept {
	const std::lock_guard lock(g_mutex);
	++g_visit;
	g_level = -1;
	g_open = false;
	// A new game makes the lair fresh: everything comes out again, and the keys must be won again.
	for (bool& b : g_packOut) b = false;
	for (bool& b : g_keeperOut) b = false;
	for (bool& b : g_keyEarned) b = false;
	for (bool& b : g_keeperSeen) b = false;
	for (bool& b : g_keyGiven) b = false;
	for (int& n : g_keeperTries) n = 0;
	g_andyOut = false;
	g_hallArmed = true;
}

}  // namespace Andariel
