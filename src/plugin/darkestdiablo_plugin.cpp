// Darkest Diablo — the D2RLoader plugin (the mod's code layer).
//
// Current features:
// - Arenas (Den of Evil, Burial Grounds, Tree of Inifuss): entering one starts its waves, one every 10 s; the
//   last wave has the boss (Corpsefire, Blood Raven). When everything is dead, a Town Portal
//   scroll goes into the inventory. Spawning uses the "cheat mspawn" / "cheat monster" commands.
// - Console commands (Ctrl + ` with enable_console on in d2rloader/config/d2rloader.toml):
//     dd           show that the plugin is running
//     dd-tp        drop a Town Portal scroll now (for testing)
//     dd-waves     restart the waves of the arena you stand in
//     dd-map       open or close the route map
// - Route map: clicking a waypoint in town opens the route map instead of Blizzard's list.
// - Waypoints: when the player is ready in a game, the plugin runs D2RLoader's "cheat waypoints"
//   command, so a new character can reach the Den by waypoint on first load.
//   For now this unlocks every waypoint. Per-stop unlocks wait for a waypoint service
//   (D2RLoader/PluginSDK issue #9).
// - The Campfire (campfire.cpp): save slots, the hero roster, recruiting, permanent death.

#include <D2RLPlugin/api.h>
#include <D2RLPlugin/core_exports.h>
#include "campfire.h"
#include "route.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

static constexpr D2RL::PluginInfo DarkestDiabloInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "darkestdiablo",
	.name        = "Darkest Diablo",
	.version     = Campfire::Version,
	.author      = "Darkest Diablo team",
	.description = "Roguelite rules for the Darkest Diablo mod.",
	.flags       = D2RL::PluginFlags::Shared,
};

static constexpr int32_t DenOfEvilLevelId = 13; // levels.txt Id of our Den arena (a spare cave level, not the
                                                // real Den, so the quest and its free respec never happen)

static const D2RL::InventoryService* g_inventory = nullptr;
static const D2RL::ItemService*      g_items     = nullptr;
static const D2RL::ThreadService*    g_threads   = nullptr;
static int32_t                       g_levelId   = -1;  // the level the local player is in

// An item code ("tsc", "ass") as the game's four-byte, space-padded value.
static uint32_t ItemCode(const char* text) noexcept {
	char c[4] = { ' ', ' ', ' ', ' ' };
	for (int i = 0; i < 4 && text[i] != '\0'; ++i) {
		c[i] = text[i];
	}
	return D2RL::Items::MakeItemCode(c[0], c[1], c[2], c[3]);
}

// Game thread: put one item in the player's inventory, or on the ground if the inventory is full.
// (A ground drop as the first choice crashed the game once, so the inventory comes first.)
static void GiveItem(const D2RL::PluginContext* context, const char* code, const char* label) noexcept {
	D2RL::PlayerHandle player = D2RL::InvalidPlayerHandle;
	char message[160] {};
	if (g_inventory->getLocalPlayer(context, &player) != D2RL::Inventory::Result::Success) {
		std::snprintf(message, sizeof(message), "%s: could not find the player.", label);
		context->LogWarn(message);
		return;
	}
	D2RL::Items::ItemCreateSpec spec {
		.structSize      = D2RL::Items::ItemCreateSpecSize,
		.code            = ItemCode(code),
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
	auto result = g_items->createItem(context, player, &spec, &item);
	if (result == D2RL::Items::Result::Success) {
		std::snprintf(message, sizeof(message), "%s: put it in the inventory.", label);
		context->LogInfo(message);
		return;
	}
	spec.destination.container = D2RL::Items::ItemContainer::Ground;
	const auto fallback = g_items->createItem(context, player, &spec, &item);
	std::snprintf(message, sizeof(message), "%s: inventory failed (code %d); ground drop %s (code %d).",
		label, static_cast<int>(result), fallback == D2RL::Items::Result::Success ? "worked" : "failed too", static_cast<int>(fallback));
	context->LogWarn(message);
}

// Game thread: the Town Portal scroll ("tsc") that ends every arena.
static void __cdecl DropTownPortal(const D2RL::PluginContext* context, void*) noexcept {
	GiveItem(context, "tsc", "Town Portal");
}

// ---- The plugin's own drops ----------------------------------------------------------------
// tools/build.py writes d2rloader/loot_data.txt from the patches (035_inifuss_loot.py):
//   "drop <level id> <monster class> <item level normal> <nightmare> <hell>"  when that monster
//       dies in that level, one extra set or unique item at that item level
//   "unique <item code> <level>" / "set <item code> <level>"                  the candidates
struct DropRule  { int32_t levelId; uint32_t monsterClass; int itemLevel[3]; };
struct Candidate { std::string code; int level; bool set; };
static std::vector<DropRule>  g_dropRules;
static std::vector<Candidate> g_candidates;
static std::atomic<int>       g_pendingDropLevel { 0 };  // item level of a queued drop, 0 = none

static void ReadLootData(const D2RL::PluginContext* context) noexcept {
	namespace fs = std::filesystem;
	try {
		std::vector<fs::path> places;  // the same places the Campfire looks for camp_data.txt
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
				if (kind == "drop") {
					DropRule rule {};
					if (words >> rule.levelId >> rule.monsterClass >> rule.itemLevel[0] >> rule.itemLevel[1] >> rule.itemLevel[2]) {
						g_dropRules.push_back(rule);
					}
				} else if (kind == "unique" || kind == "set") {
					Candidate c { {}, 0, kind == "set" };
					if (words >> c.code >> c.level) {
						g_candidates.push_back(c);
					}
				}
			}
			break;
		}
	} catch (...) {
		g_dropRules.clear();
		g_candidates.clear();
	}
	char message[128] {};
	std::snprintf(message, sizeof(message), "Loot: %d guaranteed drop(s), %d set/unique candidates.",
		static_cast<int>(g_dropRules.size()), static_cast<int>(g_candidates.size()));
	context->LogInfo(message);
}

// Game thread: one set or unique item at the queued item level. Picks random candidates at or
// below that level and lets the game choose a matching set or unique for the base; a base whose
// uniques already dropped this game fails, so it tries others. Inventory first, then the ground
// (as with the Town Portal scroll).
static void __cdecl DropGuaranteed(const D2RL::PluginContext* context, void*) noexcept {
	const int itemLevel = g_pendingDropLevel.exchange(0);
	if (itemLevel <= 0) {
		return;
	}
	D2RL::PlayerHandle player = D2RL::InvalidPlayerHandle;
	if (g_inventory->getLocalPlayer(context, &player) != D2RL::Inventory::Result::Success) {
		context->LogWarn("Guaranteed drop: could not find the player.");
		return;
	}
	std::vector<const Candidate*> fits;
	for (const auto& c : g_candidates) {
		if (c.level <= itemLevel) {
			fits.push_back(&c);
		}
	}
	std::shuffle(fits.begin(), fits.end(), std::mt19937(std::random_device {}()));
	char message[160] {};
	int tries = 0;
	for (const Candidate* c : fits) {
		if (++tries > 30) {
			break;
		}
		D2RL::Items::ItemCreateSpec spec {
			.structSize      = D2RL::Items::ItemCreateSpecSize,
			.code            = ItemCode(c->code.c_str()),
			.quality         = c->set ? D2RL::Items::Quality::Set : D2RL::Items::Quality::Unique,
			.qualityRecordId = D2RL::Items::RandomQualityRecord,
			.itemLevel       = static_cast<uint32_t>(itemLevel),
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
		auto result = g_items->createItem(context, player, &spec, &item);
		if (result != D2RL::Items::Result::Success) {
			spec.destination.container = D2RL::Items::ItemContainer::Ground;
			result = g_items->createItem(context, player, &spec, &item);
		}
		if (result == D2RL::Items::Result::Success) {
			std::snprintf(message, sizeof(message), "Guaranteed drop: a %s %s (item level %d), try %d.",
				c->set ? "set" : "unique", c->code.c_str(), itemLevel, tries);
			context->LogInfo(message);
			return;
		}
	}
	std::snprintf(message, sizeof(message), "Guaranteed drop: nothing could be made at item level %d (%d candidates, %d tries).",
		itemLevel, static_cast<int>(fits.size()), tries - 1);
	context->LogWarn(message);
}

// Runs a console command through D2RCore's ExecuteConsoleCommand export (UI thread only).
static bool RunConsole(const D2RL::PluginContext* context, const char* command) noexcept {
	namespace CE = D2RL::CoreExports;
	static CE::ExecuteConsoleCommandFn execute = nullptr;
	if (execute == nullptr) {
		const HMODULE core = GetModuleHandleA(CE::CoreDllName);
		execute = core == nullptr ? nullptr :
			reinterpret_cast<CE::ExecuteConsoleCommandFn>(GetProcAddress(core, CE::ExecuteConsoleCommandInfo.name));
	}
	if (execute == nullptr) {
		context->LogWarn("ExecuteConsoleCommand not found in D2RCore.");
		return false;
	}
	const bool ok = execute(command);
	char message[160] {};
	std::snprintf(message, sizeof(message), "Console: %s -> %s", command, ok ? "sent" : "refused");
	context->LogInfo(message);
	return ok;
}

// ---- Arenas ------------------------------------------------------------------------------
// Each arena stop is one room. Entering it starts its waves: wave 1 after a second, then one
// every 10 s. When the last wave has spawned and everything is dead (the boss included), a Town
// Portal scroll is given.
// Spawning uses the "cheat mspawn" / "cheat monster" console commands at world sub-tiles.

struct Spot { int tx, ty; };      // tile inside the arena's map
struct Group {
	const char* command;              // "mspawn <id>,<count>" or "monster super:<row>"
	const char* what;                 // MonStats Id, or "super:<row>"
	int         count;                // how many die before this group is done
	Spot        at;
};
struct Wave {
	Group    groups[3];
	int      groupCount;
	uint32_t onlyClass;               // unused for now (kept so the tables stay readable)
	uint32_t bossClass = 0xFFFFFFFFU; // if set, the wave also needs this monster dead (any kill order)
};
struct Arena {
	const char* name;
	int32_t     levelId;
	int         originX, originY;     // levels.txt OffsetX/Y: the map's world tile position
	const Wave* waves;
	int         waveCount;
};

static constexpr uint32_t AnyClass       = 0xFFFFFFFFU;
static constexpr uint32_t ZombieClass    = 5;    // monstats row of zombie1 (Corpsefire's minions; he has his own type)
static constexpr uint32_t BloodRavenClass = 267; // monstats row of bloodraven
static constexpr uint32_t CorpsefireClass = 751;  // monstats row of dd_corpsefire, Corpsefire's own type
                                                  // (CORPSEFIRE_CLASS in src/patches/010_den_of_evil.py)
static constexpr uint32_t TreeheadClass   = 752;  // monstats row of dd_treehead, Treehead WoodFist's own type
                                                  // (TREEHEAD_CLASS in src/patches/035_inifuss_loot.py)
static constexpr uint32_t RakanishuClass  = 759;  // monstats row of dd_rakanishu, Rakanishu's own type
                                                  // (RAKANISHU_CLASS in src/patches/035_inifuss_loot.py)

// Den of Evil (level 13): 2x2 open cave rooms (src/maps/den_of_evil.json). The waypoint stands in
// the bottom-left hall at tile 16,41 (about 24x6 open tiles). Fallen come from the hall's west end
// (5,40) and east end (24,40) in turn, zombies from the pocket north of it (11,31). Shamans appear
// close to the player (19,40) so they can be killed before they raise many Fallen: in the old small
// room they raised 220 Fallen from about 21. Spots picked from the map's walkable tiles (the tile
// sheets' walk flags); test them in game with "cheat mspawn". 43 monsters before Corpsefire.
static constexpr Wave DenWaves[] = {
	{ { { "mspawn", "fallen1", 6, { 5, 40 } }, { "mspawn", "zombie1", 3, { 11, 31 } } }, 2, AnyClass },
	{ { { "mspawn", "fallen1", 4, { 24, 40 } }, { "mspawn", "zombie1", 3, { 11, 31 } }, { "mspawn", "fallenshaman1", 1, { 19, 40 } } }, 3, AnyClass },
	{ { { "mspawn", "fallen1", 4, { 5, 40 } }, { "mspawn", "zombie1", 3, { 11, 31 } }, { "mspawn", "fallenshaman1", 2, { 19, 40 } } }, 3, AnyClass },
	{ { { "mspawn", "fallen1", 4, { 24, 40 } }, { "mspawn", "zombie1", 3, { 11, 31 } }, { "mspawn", "fallenshaman1", 2, { 19, 40 } } }, 3, AnyClass },
	{ { { "mspawn", "fallen1", 3, { 5, 40 } }, { "mspawn", "zombie1", 4, { 11, 31 } }, { "mspawn", "fallenshaman1", 1, { 19, 40 } } }, 3, AnyClass },
	// Corpsefire + 5 minions, at the hall's east side. The stop is cleared when everything is dead,
	// Corpsefire included: leftover Fallen used to count the Den clear while he lived.
	{ { { "monster", "super:40", 6, { 22, 41 } } }, 1, AnyClass, CorpsefireClass },
};

// Burial Grounds (level 9, a reused cave level; see src/patches/020_burial_grounds.py). The
// waypoint is in the entry room (level 10); its stairs come out at the graveyard's crypt door
// (tile 12,27). The fight is on the open plaza by the hanging tree (about tile 12,17). Many
// graveyard tiles refuse spawns ("Monster not created"); these plaza spots were tested in game
// from the console: east (16,17), north-east (15,14), south (11,21), south-west (7,20).
static constexpr Wave BurialWaves[] = {
	{ { { "mspawn", "zombie1", 5, { 16, 17 } }, { "mspawn", "zombie1", 5, { 7, 20 } } }, 2, AnyClass },
	{ { { "mspawn", "zombie1", 6, { 15, 14 } }, { "mspawn", "skeleton1", 5, { 11, 21 } } }, 2, AnyClass },
	{ { { "mspawn", "zombie1", 5, { 7, 20 } }, { "mspawn", "skeleton1", 8, { 16, 17 } } }, 2, AnyClass },
	// Blood Raven and 6 zombies. The stop is cleared when everything is dead, she included.
	{ { { "mspawn", "bloodraven", 1, { 15, 14 } }, { "mspawn", "zombie1", 6, { 11, 21 } } }, 2, AnyClass, BloodRavenClass },
};

// Tree of Inifuss (level 11, a reused cave level; see src/patches/030_inifuss.py). The waypoint
// is in the entry room (level 12); its stairs come out at the cave mouth west of the tree (about
// tile 3,12). First-guess spots around the tree, to be tested in game: east (20,12), north
// (12,4), south (12,20), south-east (18,18).
// The MF run: every monster is a "dd_mf_" copy that drops from tables with 1.4 times the magic,
// rare, set and unique chance and no keys (src/patches/035_inifuss_loot.py). Rakanishu and
// Treehead use them too. Treehead also drops one extra set or unique (see loot_data.txt).
static constexpr Wave InifussWaves[] = {
	{ { { "mspawn", "dd_mf_fallen1", 5, { 20, 12 } }, { "mspawn", "dd_mf_fallenshaman1", 2, { 12, 20 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_mf_fallen1", 5, { 12, 4 } }, { "mspawn", "dd_mf_fallenshaman1", 2, { 18, 18 } } }, 2, AnyClass },
	{ { { "monster", "super:3", 9, { 20, 12 } } }, 1, AnyClass, RakanishuClass },   // Rakanishu + 8 Carvers
	{ { { "mspawn", "dd_mf_cr_lancer1", 4, { 12, 4 } }, { "mspawn", "dd_mf_cr_lancer1", 4, { 12, 20 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_mf_cr_lancer1", 5, { 20, 12 } }, { "mspawn", "dd_mf_cr_lancer1", 3, { 18, 18 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_mf_brute2", 2, { 12, 4 } }, { "mspawn", "dd_mf_quillrat1", 6, { 12, 20 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_mf_brute2", 2, { 20, 12 } }, { "mspawn", "dd_mf_quillrat1", 6, { 18, 18 } } }, 2, AnyClass },
	// Treehead WoodFist + 2 Brutes. Won only when both Rakanishu and Treehead are dead, any order.
	{ { { "monster", "super:4", 3, { 12, 4 } } }, 1, AnyClass, TreeheadClass },
};

static constexpr Arena Arenas[] = {
	{ "Den of Evil",    DenOfEvilLevelId, 1500, 2500, DenWaves,    static_cast<int>(sizeof(DenWaves) / sizeof(DenWaves[0])) },
	{ "Burial Grounds", 9,                1500, 1300, BurialWaves, static_cast<int>(sizeof(BurialWaves) / sizeof(BurialWaves[0])) },
	{ "Tree of Inifuss", 11,              1500, 1900, InifussWaves, static_cast<int>(sizeof(InifussWaves) / sizeof(InifussWaves[0])) },
};

static constexpr int     ArrivalDelayMs = 1000;   // wave 1: right on arrival the map isn't loaded yet
static constexpr int     WaveIntervalMs = 10000;  // each later wave: 10 s after the previous one spawned
static constexpr int     MissingSpawnSlack = 3;   // clear even if this many spawns failed (the boss must still die)
static std::atomic<int>      g_arena { -1 };      // index into Arenas, -1 = none running
static std::atomic<int>      g_wave { 0 };        // waves spawned so far
static std::atomic<int>      g_remaining { 0 };   // spawned monsters not yet dead (all waves)
// The bosses to wait for: every spawned wave with a bossClass adds one (Tree of Inifuss: Rakanishu
// and Treehead). The arena is cleared only when each of them has died.
static constexpr int         MaxBosses = 4;
static std::atomic<uint32_t> g_bosses[MaxBosses] {};
static std::atomic<int>      g_bossCount { 0 };
static std::atomic<uint32_t> g_bossesDead { 0 };   // bit i: g_bosses[i] has died

static auto AllBossesDead() noexcept -> bool {
	const int count = g_bossCount.load();
	return g_bossesDead.load() == (count >= 32 ? 0xFFFFFFFFU : (1U << count) - 1U);
}
static std::atomic<bool>     g_portalDropped { false };
static std::atomic<int>      g_finalKills { 0 };  // deaths since the last wave spawned
static std::atomic<int>      g_finalSize { 0 };   // monsters in the last wave
static std::atomic<uintptr_t> g_run { 1 };        // changes on every reset, so old timers do nothing

static auto FindArena(int32_t levelId) noexcept -> int {
	for (int i = 0; i < static_cast<int>(sizeof(Arenas) / sizeof(Arenas[0])); ++i) {
		if (Arenas[i].levelId == levelId) {
			return i;
		}
	}
	return -1;
}

static void ResetArena() noexcept {
	++g_run;
	g_arena = -1;
	g_wave = 0;
	g_remaining = 0;
	g_bossCount = 0;
	g_bossesDead = 0;
	g_portalDropped = false;
	g_finalKills = 0;
	g_finalSize = 0;
}

static void __cdecl StartNextWave(const D2RL::PluginContext* context, void* run) noexcept;

// Runs `callback` on the UI thread after `delayMs`, unless the arena was reset meanwhile.
static void Later(const D2RL::PluginContext* context, int delayMs, D2RL::Threads::Callback callback) noexcept {
	const uintptr_t run = g_run.load();
	std::thread([context, delayMs, callback, run] {
		std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
		if (run == g_run.load()) {
			g_threads->runOnUiThread(context, callback, reinterpret_cast<void*>(run));
		}
	}).detach();
}

// UI thread. All waves spawned and every spawned monster (and the boss) dead: give the scroll.
static void __cdecl ClearArena(const D2RL::PluginContext* context, void* run) noexcept {
	const int index = g_arena.load();
	if (index < 0 || reinterpret_cast<uintptr_t>(run) != g_run.load() || g_portalDropped.exchange(true)) {
		return;
	}
	if (g_threads->runOnGameThread(context, DropTownPortal, nullptr) != D2RL::Threads::Result::Success) {
		context->LogWarn("Town Portal: could not queue the drop.");
	}
	char message[160] {};
	std::snprintf(message, sizeof(message), "%s: cleared.", Arenas[index].name);
	context->LogInfo(message);
	// The hero's route progress: this stop counts as cleared (route.h).
	try {
		const Route::Stop* stop = Route::StopByArenaLevel(Arenas[index].levelId);
		const auto file = Campfire::RouteProgressFile();
		if (stop != nullptr && !file.empty()) {
			Route::Progress progress = Route::Load(file);
			const bool counted = progress.RecordClear(*stop);
			if (counted && !Route::Save(file, progress)) {
				context->LogWarn("Route: could not save the hero's progress.");
			}
			std::snprintf(message, sizeof(message), "Route: %s %s; tier %d of %d is open now.", stop->id,
				counted ? "counts as cleared" : "was not open for this hero, so it doesn't count", progress.CurrentTier() + 1, Route::TierCount);
			context->LogInfo(message);
		}
	} catch (...) {
		context->LogWarn("Route: recording the clear failed.");
	}
}

// UI thread. Spawns the next wave and, if more are left, schedules the one after it.
static void __cdecl StartNextWave(const D2RL::PluginContext* context, void* run) noexcept {
	const int index = g_arena.load();
	if (index < 0 || reinterpret_cast<uintptr_t>(run) != g_run.load()) {
		return;
	}
	const Arena& arena = Arenas[index];
	const int wave = g_wave.load() + 1;
	if (wave > arena.waveCount) {
		return;
	}
	const Wave& w = arena.waves[wave - 1];
	int total = 0;
	for (int g = 0; g < w.groupCount; ++g) {
		total += w.groups[g].count;
	}
	if (w.bossClass != AnyClass) {
		const int slot = g_bossCount.load();
		if (slot < MaxBosses) {
			g_bosses[slot] = w.bossClass;
			g_bossCount = slot + 1;
		}
	}
	g_remaining += total;
	if (wave == arena.waveCount) {
		g_finalKills = 0;
		g_finalSize = total;
	}
	g_wave = wave;
	char message[128] {};
	for (int g = 0; g < w.groupCount; ++g) {
		const Group& group = w.groups[g];
		const int x = (arena.originX + group.at.tx) * 5 + 2;
		const int y = (arena.originY + group.at.ty) * 5 + 2;
		if (std::strcmp(group.command, "mspawn") == 0) {
			std::snprintf(message, sizeof(message), "cheat mspawn %s,%d @ %d,%d", group.what, group.count, x, y);
		} else {
			std::snprintf(message, sizeof(message), "cheat monster %s @ %d,%d", group.what, x, y);
		}
		RunConsole(context, message);
	}
	std::snprintf(message, sizeof(message), "%s: wave %d of %d (+%d, %d alive).", arena.name, wave, arena.waveCount, total, g_remaining.load());
	context->LogInfo(message);
	if (wave < arena.waveCount) {
		Later(context, WaveIntervalMs, StartNextWave);
	}
}

// Server thread. Count deaths in the running arena's level. The arena is cleared when the last
// wave has spawned, every spawned monster has died and the boss (if any) is dead. Extra monsters
// (raised or revived) only add kills; the boss must still die.
static void __cdecl OnMonsterDeath(const D2RL::PluginContext* context, const D2RL::Lifecycle::MonsterDeathEvent* event, void*) noexcept {
	if (context == nullptr || event == nullptr) {
		return;
	}
	const int index = g_arena.load();
	if (index < 0 || event->levelId != Arenas[index].levelId || g_wave.load() < 1 || g_portalDropped.load()) {
		return;
	}
	const Arena& arena = Arenas[index];
	for (int i = 0; i < g_bossCount.load(); ++i) {
		if (event->monster.classId == g_bosses[i].load()) {
			g_bossesDead |= 1U << i;
		}
	}
	for (const auto& rule : g_dropRules) {  // a guaranteed set or unique (Treehead)
		if (rule.levelId == event->levelId && rule.monsterClass == event->monster.classId) {
			g_pendingDropLevel = rule.itemLevel[std::min<uint32_t>(event->difficulty, 2)];
			if (g_threads->runOnGameThread(context, DropGuaranteed, nullptr) != D2RL::Threads::Result::Success) {
				context->LogWarn("Guaranteed drop: could not queue it.");
			}
		}
	}
	const int left = g_remaining.fetch_sub(1) - 1;
	// Revived or raised monsters add extra kills, which could run the count down early. So the
	// last wave must also have lost as many as it spawned since it appeared.
	const int finalKills = g_wave.load() >= arena.waveCount ? g_finalKills.fetch_add(1) + 1 : 0;
	char message[96] {};
	std::snprintf(message, sizeof(message), "%s: kill (class %u), %d alive.", arena.name, event->monster.classId, left);
	context->LogInfo(message);
	// Crowded maps sometimes refuse a spawn ("Monster not created") and the plugin can't tell, so
	// allow a few missing monsters.
	if (g_wave.load() >= arena.waveCount && left <= MissingSpawnSlack && finalKills >= g_finalSize.load() - MissingSpawnSlack &&
		AllBossesDead()) {
		g_threads->runOnUiThread(context, ClearArena, reinterpret_cast<void*>(g_run.load()));
	}
}

// ---- Route map (the waypoint "map") ------------------------------------------------------
// A plugin panel listing the act's stops as in the design sketch. A node button sends a UI
// message (see OnUiMessage); the plugin closes the panel and sends the player there with the
// console's goto. The same layout also ships as a mod file (src/data/.../RouteMaphd.json), but
// the game uses this built-in copy: change both (tests/test_route_map.py checks they match).
// The Den once went to the real Den (level 8) for weeks because only the file was changed. Clicking a town
// waypoint opens this map instead of Blizzard's list (see OnUiMessage); waypoints outside town
// keep Blizzard's list, so the player can always get home.
static const D2RL::PanelService*     g_panels = nullptr;
static const D2RL::WidgetService*    g_widgets = nullptr;  // sends native panel messages (optional)
static D2RL::Panels::RegistrationHandle g_routeMap = D2RL::Panels::InvalidHandle;

// The game's waypoint panel ("WaypointsPanelExpansion") that the route map replaced. Even with its
// open message consumed, the game counts it as open (an empty left frame and a dimmed screen until
// another panel closes it), so we close it ourselves. Empty when there is nothing to close.
static char g_nativeWaypointPanel[64] {};

// UI thread. Sends "PanelManager:ClosePanel:<the waypoint panel>", as the game's own close does.
static void __cdecl CloseNativeWaypointPanel(const D2RL::PluginContext* context, void*) noexcept {
	if (g_widgets == nullptr || g_nativeWaypointPanel[0] == '\0') {
		return;
	}
	const D2RL::Widgets::UiAction action {
		.structSize = D2RL::Widgets::UiActionSize,
		.target     = "PanelManager",
		.command    = "ClosePanel",
		.text       = g_nativeWaypointPanel,
	};
	const auto result = g_widgets->dispatchUiAction(context, &action);
	char message[128] {};
	std::snprintf(message, sizeof(message), "Route map: closing %s -> %s (code %d).", g_nativeWaypointPanel,
		result == D2RL::Widgets::Result::Success ? "sent" : "failed", static_cast<int>(result));
	context->LogInfo(message);
	if (result == D2RL::Widgets::Result::Success) {
		g_nativeWaypointPanel[0] = '\0';
	}
}

static constexpr int TownLevelId = 1;  // Rogue Encampment: "Return to town" (Act I only so far)

// levels.txt Ids of the five towns: Rogue Encampment, Lut Gholein, Kurast Docks,
// Pandemonium Fortress, Harrogath.
static auto InTown() noexcept -> bool {
	switch (g_levelId) {
	case 1: case 40: case 75: case 103: case 109:
		return true;
	default:
		return false;
	}
}

static constexpr char RouteMapLayout[] = R"json({"type":"Panel","name":"darkestdiablo/RouteMap","fields":{"rect":"$LeftPanelRect","anchor":"$LeftPanelAnchor","priority":8500},"children":[{"type":"ImageWidget","name":"Background","fields":{"rect":{"x":0,"y":0},"filename":"PANEL\\Waypoints\\Waypoints_Base","frame":0}},{"type":"ImageWidget","name":"LeftHinge","fields":{"rect":"$LeftHingeRect","filename":"$LeftHingeSprite"}},{"type":"TextBoxWidget","name":"Title","fields":{"rect":"$InGamePanelTitleRect","text":"Act I - Choose your path","style":"$StyleTitleBlock"}},{"type":"ButtonWidget","name":"NodeTownLocked","fields":{"rect":{"x":30,"y":110},"filename":"FrontEnd\\HD\\Final\\FrontEnd_ButtonSmall","pressedFrame":1,"disabledFrame":2,"hoveredFrame":3,"textString":"Return to town","pointSize":"$SmallFontSize","textColor":"$FontColorDarkGrayBlue","disabled":true}},{"type":"ButtonWidget","name":"NodeTown","fields":{"rect":{"x":30,"y":110},"filename":"FrontEnd\\HD\\Final\\FrontEnd_ButtonSmall","pressedFrame":1,"disabledFrame":2,"hoveredFrame":3,"textString":"Return to town","pointSize":"$SmallFontSize","textColor":"$FontColorWhite","onClickMessage":"PanelManager:OpenPanel:darkestdiablo/travel-1"}},{"type":"ButtonWidget","name":"NodeDenLocked","fields":{"rect":{"x":406,"y":190},"filename":"FrontEnd\\HD\\Final\\FrontEnd_ButtonSmall","pressedFrame":1,"disabledFrame":2,"hoveredFrame":3,"textString":"Den of Evil","pointSize":"$SmallFontSize","textColor":"$FontColorDarkGrayBlue","disabled":true}},{"type":"ButtonWidget","name":"NodeDen","fields":{"rect":{"x":406,"y":190},"filename":"FrontEnd\\HD\\Final\\FrontEnd_ButtonSmall","pressedFrame":1,"disabledFrame":2,"hoveredFrame":3,"textString":"Den of Evil","pointSize":"$SmallFontSize","textColor":"$FontColorWhite","onClickMessage":"PanelManager:OpenPanel:darkestdiablo/travel-13","disabled":true}},{"type":"TextBoxWidget","name":"Line1","fields":{"rect":{"x":561,"y":275,"width":40,"height":40},"text":"|","style":{"fontColor":"$FontColorGoldYellow","pointSize":"$MediumFontSize","alignment":{"h":"center","v":"center"}}}},{"type":"ButtonWidget","name":"NodeBurialLocked","fields":{"rect":{"x":406,"y":330},"filename":"FrontEnd\\HD\\Final\\FrontEnd_ButtonSmall","pressedFrame":1,"disabledFrame":2,"hoveredFrame":3,"textString":"Blood Raven","pointSize":"$SmallFontSize","textColor":"$FontColorDarkGrayBlue","disabled":true}},{"type":"ButtonWidget","name":"NodeBurial","fields":{"rect":{"x":406,"y":330},"filename":"FrontEnd\\HD\\Final\\FrontEnd_ButtonSmall","pressedFrame":1,"disabledFrame":2,"hoveredFrame":3,"textString":"Blood Raven","pointSize":"$SmallFontSize","textColor":"$FontColorWhite","onClickMessage":"PanelManager:OpenPanel:darkestdiablo/travel-10","disabled":true}},{"type":"TextBoxWidget","name":"Line2","fields":{"rect":{"x":561,"y":415,"width":40,"height":40},"text":"|","style":{"fontColor":"$FontColorGoldYellow","pointSize":"$MediumFontSize","alignment":{"h":"center","v":"center"}}}},{"type":"TextBoxWidget","name":"Line3","fields":{"rect":{"x":205,"y":425,"width":752,"height":40},"text":"________________________________________________","style":{"fontColor":"$FontColorGoldYellow","pointSize":"$MediumFontSize","alignment":{"h":"center","v":"center"}}}},{"type":"TextBoxWidget","name":"Line4","fields":{"rect":{"x":185,"y":460,"width":40,"height":40},"text":"|","style":{"fontColor":"$FontColorGoldYellow","pointSize":"$MediumFontSize","alignment":{"h":"center","v":"center"}}}},{"type":"TextBoxWidget","name":"Line5","fields":{"rect":{"x":561,"y":460,"width":40,"height":40},"text":"|","style":{"fontColor":"$FontColorGoldYellow","pointSize":"$MediumFontSize","alignment":{"h":"center","v":"center"}}}},{"type":"TextBoxWidget","name":"Line6","fields":{"rect":{"x":937,"y":460,"width":40,"height":40},"text":"|","style":{"fontColor":"$FontColorGoldYellow","pointSize":"$MediumFontSize","alignment":{"h":"center","v":"center"}}}},{"type":"TextBoxWidget","name":"Fork1Note","fields":{"rect":{"x":0,"y":475,"width":1162,"height":30},"text":"Fork - pick 2","style":{"fontColor":"$FontColorDarkGrayGold","pointSize":"$SmallFontSize","alignment":{"h":"center","v":"center"}}}},{"type":"ButtonWidget","name":"NodeInifussLocked","fields":{"rect":{"x":30,"y":520},"filename":"FrontEnd\\HD\\Final\\FrontEnd_ButtonSmall","pressedFrame":1,"disabledFrame":2,"hoveredFrame":3,"textString":"Rakanishu & Treehead","pointSize":"$SmallFontSize","textColor":"$FontColorDarkGrayBlue","disabled":true}},{"type":"ButtonWidget","name":"NodeInifuss","fields":{"rect":{"x":30,"y":520},"filename":"FrontEnd\\HD\\Final\\FrontEnd_ButtonSmall","pressedFrame":1,"disabledFrame":2,"hoveredFrame":3,"textString":"Rakanishu & Treehead","pointSize":"$SmallFontSize","textColor":"$FontColorWhite","onClickMessage":"PanelManager:OpenPanel:darkestdiablo/travel-12","disabled":true}},{"type":"ButtonWidget","name":"Fork1Mausoleum","fields":{"rect":{"x":406,"y":520},"filename":"FrontEnd\\HD\\Final\\FrontEnd_ButtonSmall","pressedFrame":1,"disabledFrame":2,"hoveredFrame":3,"textString":"Mausoleum","pointSize":"$SmallFontSize","textColor":"$FontColorDarkGrayBlue","disabled":true}},{"type":"ButtonWidget","name":"Fork1Crypt","fields":{"rect":{"x":782,"y":520},"filename":"FrontEnd\\HD\\Final\\FrontEnd_ButtonSmall","pressedFrame":1,"disabledFrame":2,"hoveredFrame":3,"textString":"Crypt","pointSize":"$SmallFontSize","textColor":"$FontColorDarkGrayBlue","disabled":true}},{"type":"TextBoxWidget","name":"Kind0_602","fields":{"rect":{"x":30,"y":602,"width":350,"height":28},"text":"MF run","style":{"fontColor":"$FontColorDarkGrayGold","pointSize":"$SmallFontSize","alignment":{"h":"center","v":"center"}}}},{"type":"TextBoxWidget","name":"Kind1_602","fields":{"rect":{"x":406,"y":602,"width":350,"height":28},"text":"Crafting hunt","style":{"fontColor":"$FontColorDarkGrayGold","pointSize":"$SmallFontSize","alignment":{"h":"center","v":"center"}}}},{"type":"TextBoxWidget","name":"Kind2_602","fields":{"rect":{"x":782,"y":602,"width":350,"height":28},"text":"XP grind","style":{"fontColor":"$FontColorDarkGrayGold","pointSize":"$SmallFontSize","alignment":{"h":"center","v":"center"}}}},{"type":"TextBoxWidget","name":"Line11","fields":{"rect":{"x":185,"y":630,"width":40,"height":40},"text":"|","style":{"fontColor":"$FontColorGoldYellow","pointSize":"$MediumFontSize","alignment":{"h":"center","v":"center"}}}},{"type":"TextBoxWidget","name":"Line12","fields":{"rect":{"x":561,"y":630,"width":40,"height":40},"text":"|","style":{"fontColor":"$FontColorGoldYellow","pointSize":"$MediumFontSize","alignment":{"h":"center","v":"center"}}}},{"type":"TextBoxWidget","name":"Line13","fields":{"rect":{"x":937,"y":630,"width":40,"height":40},"text":"|","style":{"fontColor":"$FontColorGoldYellow","pointSize":"$MediumFontSize","alignment":{"h":"center","v":"center"}}}},{"type":"TextBoxWidget","name":"Line14","fields":{"rect":{"x":205,"y":635,"width":752,"height":40},"text":"________________________________________________","style":{"fontColor":"$FontColorGoldYellow","pointSize":"$MediumFontSize","alignment":{"h":"center","v":"center"}}}},{"type":"TextBoxWidget","name":"Line15","fields":{"rect":{"x":561,"y":680,"width":40,"height":40},"text":"|","style":{"fontColor":"$FontColorGoldYellow","pointSize":"$MediumFontSize","alignment":{"h":"center","v":"center"}}}},{"type":"ButtonWidget","name":"NodeTristram","fields":{"rect":{"x":406,"y":730},"filename":"FrontEnd\\HD\\Final\\FrontEnd_ButtonSmall","pressedFrame":1,"disabledFrame":2,"hoveredFrame":3,"textString":"Tristram","pointSize":"$SmallFontSize","textColor":"$FontColorDarkGrayBlue","disabled":true}},{"type":"TextBoxWidget","name":"Line16","fields":{"rect":{"x":561,"y":815,"width":40,"height":40},"text":"|","style":{"fontColor":"$FontColorGoldYellow","pointSize":"$MediumFontSize","alignment":{"h":"center","v":"center"}}}},{"type":"TextBoxWidget","name":"Line17","fields":{"rect":{"x":205,"y":825,"width":752,"height":40},"text":"________________________________________________","style":{"fontColor":"$FontColorGoldYellow","pointSize":"$MediumFontSize","alignment":{"h":"center","v":"center"}}}},{"type":"TextBoxWidget","name":"Line18","fields":{"rect":{"x":185,"y":860,"width":40,"height":40},"text":"|","style":{"fontColor":"$FontColorGoldYellow","pointSize":"$MediumFontSize","alignment":{"h":"center","v":"center"}}}},{"type":"TextBoxWidget","name":"Line19","fields":{"rect":{"x":561,"y":860,"width":40,"height":40},"text":"|","style":{"fontColor":"$FontColorGoldYellow","pointSize":"$MediumFontSize","alignment":{"h":"center","v":"center"}}}},{"type":"TextBoxWidget","name":"Line20","fields":{"rect":{"x":937,"y":860,"width":40,"height":40},"text":"|","style":{"fontColor":"$FontColorGoldYellow","pointSize":"$MediumFontSize","alignment":{"h":"center","v":"center"}}}},{"type":"TextBoxWidget","name":"Fork2Note","fields":{"rect":{"x":0,"y":895,"width":1162,"height":30},"text":"Fork - pick 2","style":{"fontColor":"$FontColorDarkGrayGold","pointSize":"$SmallFontSize","alignment":{"h":"center","v":"center"}}}},{"type":"ButtonWidget","name":"Fork2Pit","fields":{"rect":{"x":30,"y":940},"filename":"FrontEnd\\HD\\Final\\FrontEnd_ButtonSmall","pressedFrame":1,"disabledFrame":2,"hoveredFrame":3,"textString":"Pit","pointSize":"$SmallFontSize","textColor":"$FontColorDarkGrayBlue","disabled":true}},{"type":"ButtonWidget","name":"Fork2Countess","fields":{"rect":{"x":406,"y":940},"filename":"FrontEnd\\HD\\Final\\FrontEnd_ButtonSmall","pressedFrame":1,"disabledFrame":2,"hoveredFrame":3,"textString":"Countess","pointSize":"$SmallFontSize","textColor":"$FontColorDarkGrayBlue","disabled":true}},{"type":"ButtonWidget","name":"Fork2Malus","fields":{"rect":{"x":782,"y":940},"filename":"FrontEnd\\HD\\Final\\FrontEnd_ButtonSmall","pressedFrame":1,"disabledFrame":2,"hoveredFrame":3,"textString":"Horadric Malus","pointSize":"$SmallFontSize","textColor":"$FontColorDarkGrayBlue","disabled":true}},{"type":"TextBoxWidget","name":"Kind0_1022","fields":{"rect":{"x":30,"y":1022,"width":350,"height":28},"text":"MF run","style":{"fontColor":"$FontColorDarkGrayGold","pointSize":"$SmallFontSize","alignment":{"h":"center","v":"center"}}}},{"type":"TextBoxWidget","name":"Kind1_1022","fields":{"rect":{"x":406,"y":1022,"width":350,"height":28},"text":"Crafting hunt","style":{"fontColor":"$FontColorDarkGrayGold","pointSize":"$SmallFontSize","alignment":{"h":"center","v":"center"}}}},{"type":"TextBoxWidget","name":"Kind2_1022","fields":{"rect":{"x":782,"y":1022,"width":350,"height":28},"text":"XP grind","style":{"fontColor":"$FontColorDarkGrayGold","pointSize":"$SmallFontSize","alignment":{"h":"center","v":"center"}}}},{"type":"TextBoxWidget","name":"Line25","fields":{"rect":{"x":185,"y":1050,"width":40,"height":40},"text":"|","style":{"fontColor":"$FontColorGoldYellow","pointSize":"$MediumFontSize","alignment":{"h":"center","v":"center"}}}},{"type":"TextBoxWidget","name":"Line26","fields":{"rect":{"x":561,"y":1050,"width":40,"height":40},"text":"|","style":{"fontColor":"$FontColorGoldYellow","pointSize":"$MediumFontSize","alignment":{"h":"center","v":"center"}}}},{"type":"TextBoxWidget","name":"Line27","fields":{"rect":{"x":937,"y":1050,"width":40,"height":40},"text":"|","style":{"fontColor":"$FontColorGoldYellow","pointSize":"$MediumFontSize","alignment":{"h":"center","v":"center"}}}},{"type":"TextBoxWidget","name":"Line28","fields":{"rect":{"x":205,"y":1055,"width":752,"height":40},"text":"________________________________________________","style":{"fontColor":"$FontColorGoldYellow","pointSize":"$MediumFontSize","alignment":{"h":"center","v":"center"}}}},{"type":"TextBoxWidget","name":"Line29","fields":{"rect":{"x":561,"y":1100,"width":40,"height":40},"text":"|","style":{"fontColor":"$FontColorGoldYellow","pointSize":"$MediumFontSize","alignment":{"h":"center","v":"center"}}}},{"type":"ButtonWidget","name":"NodeAndariel","fields":{"rect":{"x":406,"y":1150},"filename":"FrontEnd\\HD\\Final\\FrontEnd_ButtonSmall","pressedFrame":1,"disabledFrame":2,"hoveredFrame":3,"textString":"Andariel","pointSize":"$SmallFontSize","textColor":"$FontColorDarkGrayBlue","disabled":true}},{"type":"ButtonWidget","name":"CloseButton","fields":{"rect":{"x":1075,"y":9},"filename":"PANEL\\closebtn_4x","hoveredFrame":3,"tooltipString":"@d2r:strClose","sound":"cursor_close_window_hd","onClickMessage":"PanelManager:ClosePanel:darkestdiablo/RouteMap","pressedFrame":1}}]})json";

// ---- Unlocks along the route (src/plugin/route.h) -----------------------------------------
// Each hero's progress decides which buttons on the route map work: only stops of the current
// tier. "Return to town" works everywhere but town. Travel to a locked stop is refused too.
static auto LoadProgress() noexcept -> Route::Progress {
	try {
		const auto file = Campfire::RouteProgressFile();
		return file.empty() ? Route::Progress {} : Route::Load(file);
	} catch (...) {
		return {};
	}
}

// UI thread: enable the route map's buttons for the hero's progress.
static void __cdecl RefreshRouteMap(const D2RL::PluginContext* context, void*) noexcept {
	if (g_widgets == nullptr) {
		return;
	}
	D2RL::Widgets::WidgetHandle panel = D2RL::Widgets::InvalidHandle;
	if (g_widgets->findPanel(context, "darkestdiablo/RouteMap", &panel) != D2RL::Widgets::Result::Success) {
		context->LogWarn("Route map: panel not found; its buttons keep their layout state.");
		return;
	}
	// Every stop button starts disabled in the layout; only the ones the hero may enter are switched
	// on, so a failure here leaves stops locked rather than open.
	std::string open, failed;
	const auto set = [&](const char* name, bool enabled) {
		D2RL::Widgets::WidgetHandle widget = D2RL::Widgets::InvalidHandle;
		const auto found = g_widgets->findWidget(context, panel, name, &widget);
		const auto result = found == D2RL::Widgets::Result::Success ? g_widgets->setWidgetEnabled(context, widget, enabled) : found;
		if (result != D2RL::Widgets::Result::Success) {
			failed += std::string(" ") + name + "(" + std::to_string(static_cast<int>(result)) + ")";
		} else if (enabled) {
			open += std::string(" ") + name;
		}
	};
	// A button's text colour can't change at runtime, so each built stop (and "Return to town") has
	// a grey twin "<node>Locked" in the same spot: show the white button when it's open, else the twin.
	const auto show = [&](const std::string& name, bool visible) {
		D2RL::Widgets::WidgetHandle widget = D2RL::Widgets::InvalidHandle;
		const auto found = g_widgets->findWidget(context, panel, name.c_str(), &widget);
		const auto result = found == D2RL::Widgets::Result::Success ? g_widgets->setWidgetVisible(context, widget, visible) : found;
		if (result != D2RL::Widgets::Result::Success) {
			failed += " " + name + "(" + std::to_string(static_cast<int>(result)) + ")";
		}
	};
	const auto node = [&](const char* name, bool open) {
		set(name, open);
		show(name, open);
		show(std::string(name) + "Locked", !open);
	};
	const Route::Progress progress = LoadProgress();
	for (const auto& stop : Route::Stops) {
		if (stop.travelLevel >= 0) {
			node(stop.node, progress.CanEnter(stop));
		} else {
			set(stop.node, false);  // not built yet: grey in the layout already
		}
	}
	node("NodeTown", !InTown());
	char message[256] {};
	std::snprintf(message, sizeof(message), "Route map: tier %d of %d; enabled:%s%s%s", progress.CurrentTier() + 1, Route::TierCount,
		open.empty() ? " none" : open.c_str(), failed.empty() ? "" : "; FAILED:", failed.c_str());
	if (failed.empty()) {
		context->LogInfo(message);
	} else {
		context->LogWarn(message);
	}
}

static auto OnUiMessage(const D2RL::PluginContext* context, const D2RL::SharedEvents::UiMessageEvent* event, void*) noexcept -> D2RL::SharedEvents::UiMessageAction {
	using Action = D2RL::SharedEvents::UiMessageAction;
	if (context == nullptr || event == nullptr) {
		return Action::Continue;
	}
	const char* target = event->target != nullptr ? event->target : "";
	const char* command = event->command != nullptr ? event->command : "";
	const char* text = event->text != nullptr ? event->text : "";
	char message[256] {};
	// Log UI messages for finding new ones, but not mouse and keys or the per-frame monster bar.
	if (std::strcmp(target, "MonsterHealth") != 0 && std::strcmp(target, "InputMessage") != 0) {
		std::snprintf(message, sizeof(message), "UI message: %s:%s:%s", target, command, text);
		context->LogInfo(message);
	}
	if (Campfire::OnUiMessage(context, target, command, text)) {
		return Action::Consume;
	}
	// Every waypoint asks for Blizzard's list ("PanelManager:OpenPanel:WaypointsPanelExpansion").
	// Consume that and open the route map instead, so the route's unlocks can't be skipped
	// (the plugin unlocks every waypoint, see LocalPlayerReady). Outside town the map offers
	// "Return to town", so the player is never stranded.
	static constexpr char WaypointPanelPrefix[] = "WaypointsPanel";
	if (g_panels != nullptr && std::strcmp(target, "PanelManager") == 0 && std::strcmp(command, "OpenPanel") == 0 &&
		std::strncmp(text, WaypointPanelPrefix, sizeof(WaypointPanelPrefix) - 1) == 0) {
		const auto result = g_panels->openPanel(context, g_routeMap);
		if (result == D2RL::Panels::Result::Success) {
			// Close the game's half-open waypoint panel, queued so it runs after this message.
			std::snprintf(g_nativeWaypointPanel, sizeof(g_nativeWaypointPanel), "%s", text);
			g_threads->runOnUiThread(context, CloseNativeWaypointPanel, nullptr);
			RefreshRouteMap(context, nullptr);
			g_threads->runOnUiThread(context, RefreshRouteMap, nullptr);  // again once its widgets surely exist
			return Action::Consume;
		}
		std::snprintf(message, sizeof(message), "Route map: open failed (code %d); showing Blizzard's waypoint list.", static_cast<int>(result));
		context->LogWarn(message);
		return Action::Continue;
	}
	// Node buttons send "PanelManager:OpenPanel:darkestdiablo/travel-<level id>". The game drops
	// button messages with targets or commands it doesn't know, so the travel order rides on a
	// real command with a made-up panel name. Caught here and consumed before the game sees it.
	static constexpr char TravelPrefix[] = "darkestdiablo/travel-";
	if (std::strcmp(target, "PanelManager") == 0 && std::strcmp(command, "OpenPanel") == 0 &&
		std::strncmp(text, TravelPrefix, sizeof(TravelPrefix) - 1) == 0) {
		const int level = std::atoi(text + sizeof(TravelPrefix) - 1);
		const Route::Stop* stop = Route::StopByTravelLevel(level);
		if (level > 0 && level != TownLevelId && (stop == nullptr || !LoadProgress().CanEnter(*stop))) {
			std::snprintf(message, sizeof(message), "Route map: %s is locked for this hero.", stop != nullptr ? stop->id : text);
			context->LogInfo(message);
			RefreshRouteMap(context, nullptr);
			return Action::Consume;
		}
		if (level > 0) {
			if (g_panels != nullptr) {
				g_panels->closePanel(context, g_routeMap);
			}
			// If the earlier close didn't take, close the game's waypoint panel before leaving town.
			g_threads->runOnUiThread(context, CloseNativeWaypointPanel, nullptr);
			std::snprintf(message, sizeof(message), "goto %d", level);
			RunConsole(context, message);
		}
		return Action::Consume;
	}
	return Action::Continue;
}

static void __cdecl OnGameplayEvent(const D2RL::PluginContext* context, const D2RL::Lifecycle::GameplayEvent* event, void*) noexcept {
	if (context == nullptr || !D2RL::Lifecycle::HasGameplayEventField(event, D2RL::Lifecycle::GameplayEventRequiredSize)) {
		return;
	}
	Campfire::OnGameplayEvent(context, event);
	using Kind = D2RL::Lifecycle::GameplayEventKind;
	if (event->kind == Kind::LevelChanged) {
		g_levelId = event->currentValue;
		char message[96] {};
		std::snprintf(message, sizeof(message), "Level changed: %d -> %d.", event->previousValue, event->currentValue);
		context->LogInfo(message);
		const int arena = FindArena(g_levelId);
		// Back in an arena the hero may not enter now (cleared, or a later tier), e.g. through the
		// Town Portal left open: no waves, so nothing can be farmed (route.h).
		const Route::Stop* stop = Route::StopByArenaLevel(g_levelId);
		if (arena >= 0 && arena != g_arena.load() && stop != nullptr && !LoadProgress().CanEnter(*stop)) {
			ResetArena();
			std::snprintf(message, sizeof(message), "%s: not open for this hero (%s); no waves.", Arenas[arena].name, stop->id);
			context->LogInfo(message);
			return;
		}
		if (arena >= 0 && arena != g_arena.load()) {
			ResetArena();          // a new arena, or the same one after leaving: start from wave 1
			g_arena = arena;
			Later(context, ArrivalDelayMs, StartNextWave);
		} else if (arena < 0 && g_arena.load() >= 0) {
			ResetArena();          // left the arena (town portal, waypoint): stop its wave timer
			context->LogInfo("Left the arena: waves stopped.");
		}
		return;
	}
	if (event->kind == Kind::LocalPlayerReady) {
		RunConsole(context, "cheat waypoints");
		return;
	}
	if (event->kind == Kind::GameLeft) {
		g_levelId = -1;
		ResetArena();
		return;
	}
	if (event->kind == Kind::QuestCompleted) {
		char message[128] {};
		std::snprintf(message, sizeof(message), "Quest completed: quest row %u, level %d.", event->questRecordId, g_levelId);
		context->LogInfo(message);
		// Backup: a native quest finishing inside a running arena also gives the scroll, once.
		if (g_arena.load() >= 0 && !g_portalDropped.exchange(true) && g_threads->runOnGameThread(context, DropTownPortal, nullptr) != D2RL::Threads::Result::Success) {
			context->LogWarn("Town Portal: could not queue the drop.");
		}
	}
}

static auto InfoCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	command->plugin->WriteConsoleMessage((std::string("Darkest Diablo plugin ") + Campfire::Version + " is running.").c_str());
	return D2RL::ConsoleCommandResult::Handled;
}

static auto TownPortalCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	if (g_threads->runOnGameThread(command->plugin, DropTownPortal, nullptr) != D2RL::Threads::Result::Success) {
		command->plugin->WriteConsoleMessage("Join a local game first.");
		return D2RL::ConsoleCommandResult::Failed;
	}
	return D2RL::ConsoleCommandResult::Handled;
}

static auto WavesCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	const int arena = FindArena(g_levelId);
	if (arena < 0) {
		command->plugin->WriteConsoleMessage("Stand in an arena (Den of Evil, Burial Grounds) first.");
		return D2RL::ConsoleCommandResult::Failed;
	}
	ResetArena();
	g_arena = arena;
	StartNextWave(command->plugin, reinterpret_cast<void*>(g_run.load()));
	return D2RL::ConsoleCommandResult::Handled;
}

static auto MapCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr || g_panels == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	const auto result = g_panels->togglePanel(command->plugin, g_routeMap);
	if (result != D2RL::Panels::Result::Success) {
		char message[96] {};
		std::snprintf(message, sizeof(message), "Route map: toggle failed (code %d).", static_cast<int>(result));
		command->plugin->WriteConsoleMessage(message);
		return D2RL::ConsoleCommandResult::Failed;
	}
	g_threads->runOnUiThread(command->plugin, RefreshRouteMap, nullptr);
	return D2RL::ConsoleCommandResult::Handled;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &DarkestDiabloInfo;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr) {
		return false;
	}
	const D2RL::LifecycleService* lifecycle = nullptr;
	if (context->QueryService(&lifecycle) != D2RL::ServiceQueryResult::Success || !D2RL::HasLifecycleServiceField(lifecycle, D2RL::LifecycleServiceRequiredSize) ||
		context->QueryService(&g_inventory) != D2RL::ServiceQueryResult::Success || !D2RL::HasInventoryServiceField(g_inventory, D2RL::InventoryServiceRequiredSize) ||
		context->QueryService(&g_items) != D2RL::ServiceQueryResult::Success || !D2RL::HasItemServiceField(g_items, D2RL::ItemServiceRequiredSize) ||
		context->QueryService(&g_threads) != D2RL::ServiceQueryResult::Success || !D2RL::HasThreadServiceField(g_threads, D2RL::ThreadServiceRequiredSize)) {
		context->LogError("Darkest Diablo: a required D2RLoader service is missing.");
		return false;
	}

	using Kind = D2RL::Lifecycle::GameplayEventKind;
	for (const auto kind : { Kind::GameJoined, Kind::LocalPlayerReady, Kind::LevelChanged, Kind::PlayerLevelChanged, Kind::PlayerResurrected, Kind::GameLeft, Kind::QuestCompleted }) {
		const D2RL::Lifecycle::GameplayEventListener listener {
			.structSize = D2RL::Lifecycle::GameplayEventListenerSize,
			.kind       = kind,
			.callback   = OnGameplayEvent,
		};
		D2RL::Lifecycle::ListenerHandle handle = D2RL::Lifecycle::InvalidHandle;
		if (lifecycle->registerGameplayEventListener(context, &listener, &handle) != D2RL::Lifecycle::Result::Success) {
			context->LogError("Darkest Diablo: could not listen for game events.");
			return false;
		}
	}

	const D2RL::Lifecycle::MonsterDeathListener deathListener {
		.structSize = D2RL::Lifecycle::MonsterDeathListenerSize,
		.callback   = OnMonsterDeath,
	};
	D2RL::Lifecycle::ListenerHandle deathHandle = D2RL::Lifecycle::InvalidHandle;
	if (lifecycle->registerMonsterDeathListener(context, &deathListener, &deathHandle) != D2RL::Lifecycle::Result::Success) {
		context->LogError("Darkest Diablo: could not listen for monster deaths.");
		return false;
	}

	ReadLootData(context);

	// Route map panel and UI-message listener. Optional: without them the arenas still work.
	const D2RL::ResourceService* resources = nullptr;
	const D2RL::SharedEventService* sharedEvents = nullptr;
	if (context->QueryService(&resources) == D2RL::ServiceQueryResult::Success && D2RL::HasResourceServiceField(resources, D2RL::ResourceServiceRequiredSize) &&
		context->QueryService(&g_panels) == D2RL::ServiceQueryResult::Success && D2RL::HasPanelServiceField(g_panels, D2RL::PanelServiceRequiredSize)) {
		const D2RL::Resources::ResourceRegistration resource {
			.structSize = D2RL::Resources::ResourceRegistrationSize,
			.path       = "data/global/ui/layouts/darkestdiablo/RouteMaphd.json",
			.bytes      = RouteMapLayout,
			.byteCount  = sizeof(RouteMapLayout) - 1,
		};
		D2RL::Resources::RegistrationHandle resourceHandle = D2RL::Resources::InvalidHandle;
		const D2RL::Panels::PanelRegistration registration {
			.structSize = D2RL::Panels::PanelRegistrationSize,
			// GameplayLeftSlot: the map takes the left side like Blizzard's waypoint list, and
			// opening it closes the stock left panels. The town waypoint has already marked
			// Blizzard's list open before we swap in the map; without this, its empty frame and the
			// dimmed screen stay up until another panel closes them.
			.flags      = D2RL::Panels::PanelFlags::CloseOnEscape | D2RL::Panels::PanelFlags::GameplayLeftSlot,
			.localId    = "RouteMap",
		};
		if (resources->registerResource(context, &resource, &resourceHandle) != D2RL::Resources::Result::Success ||
			g_panels->registerPanel(context, &registration, &g_routeMap) != D2RL::Panels::Result::Success) {
			context->LogWarn("Route map: could not register the panel.");
			g_panels = nullptr;
		}
	} else {
		g_panels = nullptr;
		context->LogWarn("Route map: panel services missing.");
	}
	if (context->QueryService(&g_widgets) != D2RL::ServiceQueryResult::Success || !D2RL::HasWidgetServiceField(g_widgets, D2RL::WidgetServiceRequiredSize)) {
		g_widgets = nullptr;
		context->LogWarn("Route map: widget service missing; the game's waypoint panel won't be closed.");
	}
	if (context->QueryService(&sharedEvents) == D2RL::ServiceQueryResult::Success &&D2RL::HasSharedEventServiceField(sharedEvents, D2RL::SharedEventServiceRequiredSize)) {
		const D2RL::SharedEvents::UiMessageListener listener {
			.structSize = D2RL::SharedEvents::UiMessageListenerSize,
			.callback   = OnUiMessage,
		};
		D2RL::SharedEvents::ListenerHandle handle = 0;
		if (sharedEvents->registerUiMessageListener(context, &listener, &handle) != D2RL::SharedEvents::Result::Success) {
			context->LogWarn("Route map: could not listen for UI messages.");
		}
	} else {
		context->LogWarn("Route map: UI-message service missing.");
	}

	context->LogInfo((std::string("Darkest Diablo plugin ") + Campfire::Version + " loaded.").c_str());
	return Campfire::Load(context) &&
		context->RegisterConsoleCommand("dd", InfoCommand, "Show that the Darkest Diablo plugin is running.") &&
		context->RegisterConsoleCommand("dd-tp", TownPortalCommand, "Drop a Town Portal scroll at your feet (test).") &&
		context->RegisterConsoleCommand("dd-waves", WavesCommand, "Restart the waves of the arena you stand in (test).") &&
		context->RegisterConsoleCommand("dd-map", MapCommand, "Open or close the route map.");
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {}
