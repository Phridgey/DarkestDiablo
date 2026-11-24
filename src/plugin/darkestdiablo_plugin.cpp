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
//     dd-wavehud   show a fake 5-wave run of the wave counter (no monsters), to test how it looks
// - Wave counter (waveoverlay.cpp): "Wave 2 / 5" at the top of the screen in an arena, with a
//   "Next wave in 5" countdown during the last 5 s before each wave.
//     gm           open the GM tools window: cheats for testing (devtools.cpp, docs/gm-tools.md)
// - Route map: clicking a waypoint in town opens the route map instead of Blizzard's list.
// - Waypoints: when the player is ready in a game, the plugin runs D2RLoader's "cheat waypoints"
//   command, so a new character can reach the Den by waypoint on first load.
//   For now this unlocks every waypoint. Per-stop unlocks wait for a waypoint service
//   (D2RLoader/PluginSDK issue #9).
// - The Campfire (campfire.cpp): save slots, the hero roster, recruiting, permanent death.

#include <D2RLPlugin/api.h>
#include <D2RLPlugin/core_exports.h>
#include "campfire.h"
#include "gamememory.h"
#include "gameapi.h"
#include "gamewrite.h"
#include "devtools.h"
#include "route.h"
#include "zone.h"
#include "andariel.h"
#include "waveoverlay.h"
#include "mercgear.h"
#include "routepages.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <mutex>
#include <set>
#include <map>
#include <chrono>
#include <cmath>
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
static std::atomic<int32_t>          g_levelId { -1 };   // the level the local player is in (read by the town watcher too)

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

// ---- Trace file (test) --------------------------------------------------------------------
// The loader's log file doesn't keep the plugin's lines, and a crash loses the console. So the
// loot and wave steps are also written to d2rloader/darkestdiablo-trace.txt (not the plugins
// folder, which ships), flushed
// line by line: after a crash its last lines show the last step taken.
static std::mutex g_traceLock;
static void Trace(const D2RL::PluginContext* context, const char* line) noexcept {
	if (context == nullptr || context->pluginDirectory == nullptr) {
		return;
	}
	try {
		const std::lock_guard<std::mutex> guard(g_traceLock);
		static FILE* out = _wfopen((std::filesystem::path(context->pluginDirectory).parent_path() / "darkestdiablo-trace.txt").c_str(), L"w");
		if (out != nullptr) {
			std::fprintf(out, "%llu %s\n", static_cast<unsigned long long>(GetTickCount64()), line);
			std::fflush(out);
		}
	} catch (...) {
	}
}

// ---- The plugin's own drops ----------------------------------------------------------------
// tools/build.py writes d2rloader/loot_data.txt from the patches (035_inifuss_loot.py):
//   "drop <level id> <monster class> <item level normal> <nightmare> <hell>"  when that monster
//       dies in that level, one extra set or unique item at that item level
//   "unique <item code> <level>" / "set <item code> <level>"                  the candidates
//   "craftmf <level id> <magic find %> <suffix> <rune chance %>" (060_mausoleum.py): in that
//       level, monsters "dd_craft_..." spawn as their copy with that suffix when the hero has at
//       least that much magic find (the copies' loot tables drop runes at that chance)
struct DropRule  { int32_t levelId; uint32_t monsterClass; int itemLevel[3]; };
struct Candidate { std::string code; int level; bool set; };
static std::vector<DropRule>  g_dropRules;
static std::vector<Candidate> g_candidates;
struct CraftStep { int32_t levelId; int magicFind; std::string suffix; double runeChance; };
struct TargetClass { int32_t levelId; uint32_t classId; };
static std::vector<TargetClass> g_targetClasses;      // "targetclass <level id> <class>" (090_pit.py)
static std::map<std::string, uint32_t> g_monsterClasses;  // "monclass <Id> <class>" (999_monster_classes.py)
static std::map<int, int> g_superCopies;              // "superalt <row> <copy row>" (998_superunique_copies.py)
static std::set<int>      g_supersSpawned;            // superunique rows spawned in this game
static std::mutex         g_supersLock;
static std::vector<CraftStep> g_craftSteps;             // in the order of loot_data.txt (rising magic find)
static std::atomic<int>       g_pendingDropLevel { 0 };  // item level of a queued drop, 0 = none

static std::atomic<int> g_mercType { -1 };  // "merctype" in loot_data.txt (997_merc_gear_type.py)

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
				} else if (kind == "monclass") {
					std::string id;
					uint32_t cls = 0;
					if (words >> id >> cls) {
						g_monsterClasses[id] = cls;
					}
				} else if (kind == "merctype") {
					int number = -1;
					if (words >> number) {
						g_mercType = number;
					}
				} else if (kind == "superalt") {
					int row = 0, copy = 0;
					if (words >> row >> copy) {
						g_superCopies[row] = copy;
					}
				} else if (kind == "targetclass") {
					TargetClass target {};
					if (words >> target.levelId >> target.classId) {
						g_targetClasses.push_back(target);
					}
				} else if (kind == "craftmf") {
					CraftStep step {};
					if (words >> step.levelId >> step.magicFind >> step.suffix >> step.runeChance) {
						g_craftSteps.push_back(step);
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
		g_craftSteps.clear();
		g_targetClasses.clear();
		g_monsterClasses.clear();
	}
	char message[160] {};
	std::snprintf(message, sizeof(message), "Loot: %d guaranteed drop(s), %d set/unique candidates, %d magic find step(s).",
		static_cast<int>(g_dropRules.size()), static_cast<int>(g_candidates.size()), static_cast<int>(g_craftSteps.size()));
	context->LogInfo(message);
		Trace(context, message);
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
		Trace(context, message);
			return;
		}
	}
	std::snprintf(message, sizeof(message), "Guaranteed drop: nothing could be made at item level %d (%d candidates, %d tries).",
		itemLevel, static_cast<int>(fits.size()), tries - 1);
	context->LogWarn(message);
		Trace(context, message);
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
		Trace(context, message);
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
	// Spawn around the hero instead of at the fixed spots. The game only keeps the map near the
	// hero loaded, and spawning in a part that isn't loaded crashes it (BC_ASSERT hRoom), so a big
	// hall can't use far fixed spots. The spots then say where around the hero, at half their
	// distance from `centre`, kept within `reach` tiles of the centre (the open floor; 0: no limit,
	// e.g. Tristram, where the spots are just offsets from the hero).
	bool        nearHero = false;
	Spot        centre   = { 0, 0 };
	int         reach    = 0;
	// A stop without waves (the game's own map and monsters) is cleared when this monster dies.
	uint32_t    clearOnKill = 0xFFFFFFFFU;
	// Every "mspawn" pack also gets one unique monster of its type (random name and affixes, like
	// the game's own uniques, no minions): "cheat monster unique:<id>".
	bool        uniquePerPack = false;
	// With uniquePerPack: one of those uniques, picked at random, is the target. Killing it clears
	// the stop (if it can't be found, the stop clears the usual way, everything dead).
	bool        clearOnRandomUnique = false;
	// Every "mspawn" pack comes this many times at once, the copies spread evenly around the hero
	// (more density without longer wave tables).
	int         packCopies = 1;
	// A quest stop (Act II): cleared when the hero picks up this item (its code, e.g. "box", the
	// Horadric Cube), whatever else is still alive. A hero who already owns one when arriving (a
	// replay: the game gives a quest item once) clears it instead by opening `questChest`
	// (objects.txt row), the chest that holds the item; -1 for none.
	const char* clearOnItem = nullptr;
	int         questChest  = -1;
	// Where the hero is put on arrival (a tile of the map; -1: wherever "goto" puts them). Maps
	// without a waypoint or stairs give "goto" nothing to aim at.
	Spot        arrival     = { -1, -1 };
	// A map walked one way, south to north (the Arcane Sanctuary's arm, the tunnels): monsters
	// only spawn ahead of the hero (north of them), never behind.
	bool        ahead       = false;
	// Cleared when this many objects of this objects.txt row have been used (opened, smashed,
	// freed): Travincal's Compelling Orb, the barbarians' three cages, frozen Anya. -1: none.
	int         clearOnObject    = -1;
	int         clearObjectCount = 1;
};

static constexpr uint32_t AnyClass       = 0xFFFFFFFFU;
static constexpr uint32_t ZombieClass    = 5;    // monstats row of zombie1 (Corpsefire's minions; he has his own type)
static constexpr uint32_t BloodRavenClass = 267; // monstats row of bloodraven
static constexpr uint32_t GriswoldClass  = 365;  // monstats row of griswold (Tristram's boss)
static constexpr int      GibbetRow      = 26;   // objects.txt: Gibbet, Cain's cage in Tristram
static constexpr uint32_t SmithClass     = 402;  // monstats row of smith (the Malus's guard)
static constexpr uint32_t AndarielClass  = 156;  // monstats row of andariel
static constexpr uint32_t RadamentClass  = 229;  // monstats row of radament
static constexpr int32_t  SewersLevelId  = 49;   // Act II: Sewers 3 (Radament)
static constexpr int      CubeChestRow   = 354;  // objects.txt: HoradricCubeChest
static constexpr int      StaffChestRow  = 356;  // objects.txt: StaffOfKingsChest
static constexpr int      SunAltarRow    = 149;  // objects.txt: TaintedSunShrine (gives the Amulet of the Viper)
static constexpr uint32_t SummonerClass  = 250;  // monstats row of summoner
static constexpr uint32_t DurielClass    = 211;  // monstats row of duriel
// Acts III-V (monstats rows)
static constexpr uint32_t MephistoClass  = 242;
static constexpr uint32_t DiabloClass    = 243;
static constexpr uint32_t IzualClass     = 256;
static constexpr uint32_t HephastoClass  = 409;
static constexpr uint32_t PindleskinClass = 441;  // reanimatedhorde5, Pindleskin's type
static constexpr uint32_t ShenkClass     = 480;   // overseer1, Shenk's type (the waves use no overseers)
static constexpr uint32_t NihlathakClass = 527;
static constexpr uint32_t BaalClass      = 545;   // baalcrab, Baal in the Worldstone Chamber
static constexpr uint32_t CowKingClass   = 736;
static constexpr uint32_t CorpsefireClass = 751;  // monstats row of dd_corpsefire, Corpsefire's own type
                                                  // (CORPSEFIRE_CLASS in src/patches/010_den_of_evil.py)
static constexpr uint32_t TreeheadClass   = 752;  // monstats row of dd_treehead, Treehead WoodFist's own type
                                                  // (TREEHEAD_CLASS in src/patches/035_inifuss_loot.py)
static constexpr uint32_t RakanishuClass  = 759;  // monstats row of dd_rakanishu, Rakanishu's own type
                                                  // (RAKANISHU_CLASS in src/patches/035_inifuss_loot.py)

// Den of Evil (level 13): a round grotto, radius 11 around tile 12,12 (src/maps/den_of_evil.json).
// The waypoint stands at the west edge (tile 3,12) and the fight comes from the east and south,
// so the player never has to turn back. Fallen come from the east
// (17,10) and the south (12,18) in turn, zombies from the south-east (17,15). Shamans appear
// closest (11,11) so they can be killed before they raise many Fallen. Corpsefire comes last from
// the far south-east (19,17). Test the spots in game with "cheat mspawn". 43 monsters before Corpsefire.
static constexpr Wave DenWaves[] = {
	{ { { "mspawn", "dd_den_fallen1", 6, { 12, 18 } }, { "mspawn", "dd_den_zombie1", 3, { 17, 15 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_den_fallen1", 4, { 17, 10 } }, { "mspawn", "dd_den_zombie1", 3, { 17, 15 } }, { "mspawn", "dd_den_fallenshaman1", 1, { 11, 11 } } }, 3, AnyClass },
	{ { { "mspawn", "dd_den_fallen1", 4, { 12, 18 } }, { "mspawn", "dd_den_zombie1", 3, { 17, 15 } }, { "mspawn", "dd_den_fallenshaman1", 2, { 11, 11 } } }, 3, AnyClass },
	{ { { "mspawn", "dd_den_fallen1", 4, { 17, 10 } }, { "mspawn", "dd_den_zombie1", 3, { 17, 15 } }, { "mspawn", "dd_den_fallenshaman1", 2, { 11, 11 } } }, 3, AnyClass },
	{ { { "mspawn", "dd_den_fallen1", 3, { 12, 18 } }, { "mspawn", "dd_den_zombie1", 4, { 17, 15 } }, { "mspawn", "dd_den_fallenshaman1", 1, { 11, 11 } } }, 3, AnyClass },
	// Corpsefire + 5 minions, at the hall's east side. The stop is cleared when everything is dead,
	// Corpsefire included: leftover Fallen used to count the Den clear while he lived.
	{ { { "monster", "super:40", 6, { 19, 17 } } }, 1, AnyClass, CorpsefireClass },
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

// Mausoleum (level 14, the fork 1 crafting hunt): a round crypt hall, 24 tiles across, centre tile
// 16,16 (src/maps/mausoleum.json). The waypoint stands at the west edge (tile 5,16); the undead come
// from the east. Their types are the Mausoleum's undead copied as dd_craft_<id>, which drop only
// runes, gems, potions and gold (src/patches/060_mausoleum.py). Test the spots in game with
// "cheat mspawn". 44 monsters; no boss: cleared when all are dead.
static constexpr Wave MausoleumWaves[] = {
	{ { { "mspawn", "dd_craft_skeleton1", 5, { 14, 12 } }, { "mspawn", "dd_craft_zombie2", 4, { 14, 20 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_craft_zombie2", 5, { 20, 16 } }, { "mspawn", "dd_craft_skeleton1", 4, { 14, 12 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_craft_wraith1", 4, { 22, 11 } }, { "mspawn", "dd_craft_skeleton1", 4, { 14, 20 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_craft_sk_archer1", 3, { 24, 20 } }, { "mspawn", "dd_craft_zombie2", 5, { 20, 16 } }, { "mspawn", "dd_craft_wraith1", 2, { 22, 11 } } }, 3, AnyClass },
	{ { { "mspawn", "dd_craft_skeleton1", 5, { 14, 12 } }, { "mspawn", "dd_craft_sk_archer1", 3, { 24, 20 } }, { "mspawn", "dd_craft_wraith1", 2, { 22, 21 } } }, 3, AnyClass },
	{ { { "mspawn", "dd_craft_zombie2", 6, { 26, 16 } }, { "mspawn", "dd_craft_skeleton1", 4, { 20, 16 } } }, 2, AnyClass },
};

// Crypt (level 15, the fork 1 XP grind): the Mausoleum's round hall again (src/maps/crypt.json),
// with the same waves of the Crypt's undead copied as dd_xp_<id>, worth 40% more experience and
// dropping as normal (src/patches/070_crypt.py). Each pack is led by a unique of its type (random
// name and affixes). 44 monsters and 14 uniques; killing one of the uniques, picked at random, clears it.
static constexpr Wave CryptWaves[] = {
	{ { { "mspawn", "dd_xp_skeleton1", 5, { 14, 12 } }, { "mspawn", "dd_xp_zombie2", 4, { 14, 20 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_zombie2", 5, { 20, 16 } }, { "mspawn", "dd_xp_skeleton1", 4, { 14, 12 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_wraith1", 4, { 22, 11 } }, { "mspawn", "dd_xp_skeleton1", 4, { 14, 20 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_sk_archer1", 3, { 24, 20 } }, { "mspawn", "dd_xp_zombie2", 5, { 20, 16 } }, { "mspawn", "dd_xp_wraith1", 2, { 22, 11 } } }, 3, AnyClass },
	{ { { "mspawn", "dd_xp_skeleton1", 5, { 14, 12 } }, { "mspawn", "dd_xp_sk_archer1", 3, { 24, 20 } }, { "mspawn", "dd_xp_wraith1", 2, { 22, 21 } } }, 3, AnyClass },
	{ { { "mspawn", "dd_xp_zombie2", 6, { 26, 16 } }, { "mspawn", "dd_xp_skeleton1", 4, { 20, 16 } } }, 2, AnyClass },
};

// Tristram (level 38, forced): the game's map without its own monsters (src/maps/tristram.json).
// Waves of Tristram's monsters copied as dd_tris_<id> (the zone's magic find bonus is in their
// loot, src/patches/080_tristram.py), spawned around the hero wherever they stand; then Griswold
// ("super:5") with a pack. Killing Griswold clears it.
static constexpr Wave TristramWaves[] = {
	{ { { "mspawn", "dd_tris_skeleton2", 5, { 12, 0 } }, { "mspawn", "dd_tris_goatman2", 4, { -12, 4 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_tris_goatman2", 5, { 0, 12 } }, { "mspawn", "dd_tris_sk_archer1", 3, { 0, -12 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_tris_skeleton2", 5, { -12, -4 } }, { "mspawn", "dd_tris_fallenshaman2", 2, { 12, 4 } }, { "mspawn", "dd_tris_goatman2", 3, { 12, 4 } } }, 3, AnyClass },
	{ { { "mspawn", "dd_tris_sk_archer1", 4, { 12, -8 } }, { "mspawn", "dd_tris_skeleton2", 5, { -8, 12 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_tris_goatman2", 6, { -12, 0 } }, { "mspawn", "dd_tris_skeleton2", 4, { 12, 0 } } }, 2, AnyClass },
	{ { { "monster", "super:5", 1, { 12, 0 } }, { "mspawn", "dd_tris_skeleton2", 5, { 12, 2 } } }, 2, AnyClass, GriswoldClass },
};

// Pit (level 16, the real Pit level 2; fork 2 MF run): our copy of its room, as the Den's
// (src/maps/pit.json), so the Den's tested spots. Five packs of the Pit's monsters copied as
// dd_pit_<id> (all drop with the run's magic find bonus, src/patches/090_pit.py), each led by a
// unique; one unique, picked at random, spawns as its "_t" type, drops a guaranteed unique or set,
// and killing it wins.
static constexpr Wave PitWaves[] = {
	{ { { "mspawn", "dd_pit_fallen3", 8, { 17, 10 } } }, 1, AnyClass },
	{ { { "mspawn", "dd_pit_corruptrogue3", 6, { 12, 18 } } }, 1, AnyClass },
	{ { { "mspawn", "dd_pit_skeleton3", 7, { 17, 15 } } }, 1, AnyClass },
	{ { { "mspawn", "dd_pit_cr_archer3", 5, { 19, 17 } } }, 1, AnyClass },
	{ { { "mspawn", "dd_pit_fallen3", 8, { 11, 11 } } }, 1, AnyClass },
};

// Countess (level 25, Tower Cellar 5; fork 2 crafting hunt): the cellar's map without its own
// monsters (src/maps/countess.json). Waves of the cellar's monster types around the hero, then the
// Countess ("super:6") with her pack. Her death clears it (her type: "targetclass" in loot_data.txt).
static constexpr Wave CountessWaves[] = {
	{ { { "mspawn", "fallen3", 8, { 12, 0 } }, { "mspawn", "goatman3", 4, { -12, 4 } } }, 2, AnyClass },
	{ { { "mspawn", "wraith1", 5, { 0, 12 } }, { "mspawn", "cr_archer3", 4, { 0, -12 } } }, 2, AnyClass },
	{ { { "mspawn", "goatman3", 6, { 12, 6 } }, { "mspawn", "fallen3", 6, { -12, -6 } } }, 2, AnyClass },
	{ { { "mspawn", "wraith1", 6, { -8, 10 } }, { "mspawn", "cr_archer3", 5, { 10, -8 } } }, 2, AnyClass },
	{ { { "monster", "super:6", 7, { 12, 0 } }, { "mspawn", "goatman3", 5, { -12, 0 } } }, 2, AnyClass },
};

// Horadric Malus (level 28, a cut-down Barracks; fork 2 XP grind): a large hall (tiles 2-19 by
// 2-12, centre 10,7) whose door leads into the Smith's forge (src/maps/malus.json). Waves of the
// Barracks' monsters copied as dd_malus_<id> (+40% experience, src/patches/110_malus.py) around the
// hero, kept inside the hall. The Smith is the forge's own; killing him clears it.
static constexpr Wave MalusWaves[] = {
	{ { { "mspawn", "dd_malus_corruptrogue4", 6, { 4, 4 } }, { "mspawn", "dd_malus_goatman5", 5, { 16, 10 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_malus_skmage_fire2", 4, { 16, 4 } }, { "mspawn", "dd_malus_fallen4", 8, { 4, 10 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_malus_goatman5", 6, { 10, 3 } }, { "mspawn", "dd_malus_sk_archer3", 4, { 10, 11 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_malus_corruptrogue4", 6, { 16, 4 } }, { "mspawn", "dd_malus_fallenshaman3", 2, { 4, 10 } }, { "mspawn", "dd_malus_fallen4", 6, { 4, 10 } } }, 3, AnyClass },
	{ { { "mspawn", "dd_malus_sk_archer3", 5, { 4, 4 } }, { "mspawn", "dd_malus_skmage_fire2", 4, { 16, 10 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_malus_goatman5", 7, { 10, 7 } }, { "mspawn", "dd_malus_corruptrogue4", 5, { 4, 7 } } }, 2, AnyClass },
};

// ---- Act II (docs/act2.md, src/patches/111_act2_arenas.py) ----
// Halls of the Dead 3 (level 60): three tomb rooms in a line (src/maps/halls.json); the hero
// arrives in the south room and walks north to the Horadric Cube's chest. Cleared on picking up
// the Cube. Bloodwitch the Wild (superunique 11) guards the chest in the last wave.
static constexpr Wave HallsWaves[] = {
	{ { { "mspawn", "dd_halls_mummy2", 4, { 8, 32 } }, { "mspawn", "dd_halls_unraveler1", 2, { 8, 24 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_halls_batdemon1", 4, { 11, 27 } }, { "mspawn", "dd_halls_slinger2", 3, { 6, 27 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_halls_mummy2", 5, { 8, 17 } }, { "mspawn", "dd_halls_unraveler1", 2, { 10, 9 } } }, 2, AnyClass },
	{ { { "monster", "super:11", 4, { 8, 7 } }, { "mspawn", "dd_halls_mummy2", 4, { 7, 11 } } }, 2, AnyClass },
};
// Sewers 3 (level 49): three sewer rooms in a line (src/maps/sewers.json), XP grind; the hero
// arrives in the south room, Radament (superunique 10) waits in his lair at the north end.
// Cleared when every wave is dead, Radament included (2026-11-23: no longer on his Book of Skill,
// which can't be read outside the real quest; the 3 skill points come with the clear).
static constexpr Wave SewersWaves[] = {
	{ { { "mspawn", "dd_xp_a2_skeleton4", 5, { 7, 19 } }, { "mspawn", "dd_xp_a2_mummy1", 3, { 4, 25 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_a2_pantherwoman1", 4, { 3, 14 } }, { "mspawn", "dd_xp_a2_skmage_fire3", 3, { 7, 19 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_a2_skeleton4", 6, { 3, 8 } }, { "mspawn", "dd_xp_a2_mummy1", 4, { 7, 3 } } }, 2, AnyClass },
	{ { { "monster", "super:10", 5, { 5, 4 } } }, 1, AnyClass, RadamentClass },
};
// Stony Tomb (level 55): a round tomb hall (src/maps/stony.json), crafting hunt like the
// Mausoleum (runes up to Shael). The hero arrives at the west edge; waves come around the hero.
static constexpr Wave StonyWaves[] = {
	{ { { "mspawn", "dd_craft_a2_skeleton5", 5, { 26, 16 } }, { "mspawn", "dd_craft_a2_scarab1", 4, { 20, 26 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_craft_a2_scarab1", 5, { 22, 8 } }, { "mspawn", "dd_craft_a2_skmage_ltng3", 3, { 26, 20 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_craft_a2_skeleton5", 6, { 16, 26 } }, { "mspawn", "dd_craft_a2_skmage_ltng3", 3, { 16, 6 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_craft_a2_scarab1", 6, { 24, 22 } }, { "mspawn", "dd_craft_a2_skeleton5", 5, { 24, 10 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_craft_a2_skmage_ltng3", 4, { 26, 16 } }, { "mspawn", "dd_craft_a2_scarab1", 6, { 16, 26 } }, { "mspawn", "dd_craft_a2_skeleton5", 5, { 16, 6 } } }, 3, AnyClass },
};

// Maggot Lair 3 (level 64): three lair rooms in a line (src/maps/maggot.json); the hero arrives
// in the south room and goes north to the treasure room and the Staff of Kings' chest, where
// Coldworm the Burrower (superunique 15) waits. Cleared on picking up the staff.
static constexpr Wave MaggotWaves[] = {
	{ { { "mspawn", "dd_maggot_sandmaggot1", 4, { 4, 20 } }, { "mspawn", "dd_maggot_scarab2", 3, { 6, 15 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_maggot_swarm2", 3, { 4, 12 } }, { "mspawn", "dd_maggot_sandmaggot2", 4, { 5, 5 } } }, 2, AnyClass },
	{ { { "monster", "super:15", 3, { 5, 5 } }, { "mspawn", "dd_maggot_scarab3", 4, { 3, 7 } } }, 2, AnyClass },
};
// Claw Viper Temple 2 (level 61): the altar room alone (src/maps/clawviper.json); the hero arrives
// in the corridor at its north end. Fangskin (superunique 12) and the clan; the Tainted Sun altar
// gives the Amulet of the Viper. Cleared on picking it up.
static constexpr Wave ClawViperWaves[] = {
	{ { { "mspawn", "dd_viper_clawviper2", 4, { 5, 10 } }, { "mspawn", "dd_viper_clawviper3", 3, { 12, 8 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_viper_mummy3", 3, { 10, 13 } }, { "mspawn", "dd_viper_clawviper2", 4, { 4, 8 } } }, 2, AnyClass },
	{ { { "monster", "super:12", 4, { 12, 13 } }, { "mspawn", "dd_viper_clawviper3", 3, { 5, 13 } } }, 2, AnyClass },
};
// Arcane Sanctuary (level 74): one arm of the cross (src/maps/arcane.json), walked south to
// north; monsters only come from ahead. Crafting hunt (runes up to Shael). The Summoner
// (superunique 18) waits on his platform at the far end; his death clears it.
static constexpr Wave ArcaneWaves[] = {
	{ { { "mspawn", "dd_craft_a2b_wraith3", 4, { 5, 29 } }, { "mspawn", "dd_craft_a2b_goatman4", 4, { 5, 17 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_craft_a2b_vampire1", 2, { 5, 17 } }, { "mspawn", "dd_craft_a2b_goatman4", 5, { 5, 7 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_craft_a2b_wraith3", 5, { 5, 7 } }, { "mspawn", "dd_craft_a2b_goatman4", 4, { 4, 5 } } }, 2, AnyClass },
	{ { { "monster", "super:18", 1, { 3, 3 } }, { "mspawn", "dd_craft_a2b_vampire1", 2, { 4, 5 } } }, 2, AnyClass, SummonerClass },
};
// Ancient Tunnels (level 65): four sewer rooms in a line (src/maps/tunnels.json), MF run; every
// pack is led by a unique, and a random one of them is the target. Monsters only come from ahead.
static constexpr Wave TunnelsWaves[] = {
	{ { { "mspawn", "dd_mf_a2_zombie5", 4, { 6, 31 } }, { "mspawn", "dd_mf_a2_sandraider3", 3, { 4, 26 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_mf_a2_mummy3", 3, { 7, 19 } }, { "mspawn", "dd_mf_a2_skmage_ltng4", 2, { 3, 14 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_mf_a2_sandraider3", 4, { 5, 3 } }, { "mspawn", "dd_mf_a2_zombie5", 4, { 8, 6 } } }, 2, AnyClass },
};
// Canyon of the Magi (on level 59, a spare tomb level; the real level 46 is outdoors and crashed): a round sandy hollow (src/maps/canyon.json), XP grind; waves
// around the hero. Cleared when everything is dead.
static constexpr Wave CanyonWaves[] = {
	{ { { "mspawn", "dd_xp_a2b_brute4", 3, { 20, 12 } }, { "mspawn", "dd_xp_a2b_sandmaggot3", 4, { 16, 19 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_a2b_pantherwoman4", 5, { 18, 6 } }, { "mspawn", "dd_xp_a2b_scarab4", 4, { 20, 16 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_a2b_slinger4", 4, { 12, 20 } }, { "mspawn", "dd_xp_a2b_brute4", 3, { 12, 4 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_a2b_scarab4", 6, { 19, 18 } }, { "mspawn", "dd_xp_a2b_pantherwoman4", 5, { 19, 7 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_a2b_sandmaggot3", 5, { 20, 12 } }, { "mspawn", "dd_xp_a2b_slinger4", 4, { 14, 20 } }, { "mspawn", "dd_xp_a2b_brute4", 3, { 14, 4 } } }, 3, AnyClass },
};

// ---- Acts III-V (docs/act3.md, act4.md, act5.md; src/patches/113_act3.py, 114_act4.py, 114a_act5.py) ----
// Spots and arrivals sit on tiles where the game's own maps place things (so they are floor), not
// tested in game yet.
// Spider Cavern (level 85, src/maps/spider.json): one round grotto (centre 15,15, radius 13); the
// hero arrives at the west edge, Khalim's Eye chest is on the east side.
static constexpr Wave SpiderWaves[] = {
	{ { { "mspawn", "dd_xp_a3a_arach3", 3, { 12, 7 } }, { "mspawn", "dd_xp_a3a_arach4", 4, { 12, 23 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_a3a_batdemon2", 4, { 16, 15 } }, { "mspawn", "dd_xp_a3a_arach3", 5, { 20, 8 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_a3a_arach4", 5, { 20, 22 } }, { "mspawn", "dd_xp_a3a_batdemon2", 3, { 9, 15 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_a3a_arach3", 3, { 23, 11 } }, { "mspawn", "dd_xp_a3a_arach4", 4, { 23, 19 } } }, 2, AnyClass },
};
// Flayer Dungeon (level 91, src/maps/flayer.json).
static constexpr Wave FlayerWaves[] = {
	{ { { "mspawn", "dd_xp_a3b_fetish3", 3, { 4, 20 } }, { "mspawn", "dd_xp_a3b_fetishshaman3", 4, { 4, 20 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_a3b_fetishblow3", 4, { 8, 16 } }, { "mspawn", "dd_xp_a3b_fetish3", 5, { 6, 12 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_a3b_fetishshaman3", 5, { 6, 12 } }, { "mspawn", "dd_xp_a3b_fetishblow3", 3, { 6, 12 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_a3b_fetish3", 3, { 6, 12 } }, { "mspawn", "dd_xp_a3b_fetishshaman3", 4, { 6, 4 } } }, 2, AnyClass },
};
// Kurast Sewers (level 93, src/maps/ksewers.json).
static constexpr Wave KSewersWaves[] = {
	{ { { "mspawn", "dd_xp_a3c_mummy4", 3, { 8, 17 } }, { "mspawn", "dd_xp_a3c_unraveler4", 4, { 10, 15 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_a3c_frogdemon3", 4, { 9, 13 } }, { "mspawn", "dd_xp_a3c_mummy4", 5, { 10, 11 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_a3c_unraveler4", 5, { 9, 8 } }, { "mspawn", "dd_xp_a3c_frogdemon3", 3, { 9, 8 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_a3c_mummy4", 3, { 9, 4 } }, { "mspawn", "dd_xp_a3c_unraveler4", 4, { 8, 3 } } }, 2, AnyClass },
};
// Ruined Temple (level 94, src/maps/lamesen.json).
static constexpr Wave LamEsenWaves[] = {
	{ { { "mspawn", "dd_xp_a3d_brute5", 3, { 13, 5 } }, { "mspawn", "dd_xp_a3d_corruptrogue5", 4, { 13, 7 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_a3d_arach5", 4, { 13, 7 } }, { "mspawn", "dd_xp_a3d_vampire2", 5, { 13, 7 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_a3d_brute5", 5, { 13, 7 } }, { "mspawn", "dd_xp_a3d_corruptrogue5", 3, { 13, 12 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_a3d_arach5", 3, { 13, 12 } }, { "mspawn", "dd_xp_a3d_vampire2", 4, { 13, 12 } }, { "monster", "super:24", 1, { 13, 12 } } }, 3, AnyClass },
};
// Swampy Pit (level 90, src/maps/swampy.json).
static constexpr Wave SwampyWaves[] = {
	{ { { "mspawn", "dd_mf_a3_bonefetish4", 3, { 3, 4 } }, { "mspawn", "dd_mf_a3_bonefetish5", 4, { 3, 4 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_mf_a3_batdemon2", 4, { 8, 10 } }, { "mspawn", "dd_mf_a3_willowisp1", 5, { 9, 10 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_mf_a3_bonefetish4", 5, { 16, 14 } }, { "mspawn", "dd_mf_a3_bonefetish5", 3, { 16, 14 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_mf_a3_batdemon2", 3, { 18, 17 } }, { "mspawn", "dd_mf_a3_willowisp1", 4, { 18, 20 } } }, 2, AnyClass },
};
// Lower Kurast (level 95, src/maps/lowerkurast.json).
static constexpr Wave LowerKurastWaves[] = {
	{ { { "mspawn", "dd_craft_a3_baboon4", 3, { 14, 43 } }, { "mspawn", "dd_craft_a3_sandleaper4", 4, { 13, 38 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_craft_a3_zealot1", 4, { 10, 33 } }, { "mspawn", "dd_craft_a3_baboon4", 5, { 11, 27 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_craft_a3_sandleaper4", 5, { 11, 27 } }, { "mspawn", "dd_craft_a3_zealot1", 3, { 9, 20 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_craft_a3_baboon4", 3, { 10, 16 } }, { "mspawn", "dd_craft_a3_sandleaper4", 4, { 10, 14 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_craft_a3_zealot1", 4, { 10, 14 } }, { "mspawn", "dd_craft_a3_baboon4", 5, { 10, 16 } } }, 2, AnyClass },  // (8,5) and (10,9) sit on the temple platform's wall and a brazier
};
// Travincal (level 96, src/maps/travincal.json).
static constexpr Wave TravincalWaves[] = {
	{ { { "mspawn", "dd_mf_a3b_cantor3", 3, { 28, 24 } }, { "mspawn", "dd_mf_a3b_vampire2", 4, { 28, 24 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_mf_a3b_cantor3", 4, { 17, 22 } }, { "mspawn", "dd_mf_a3b_vampire2", 5, { 17, 22 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_mf_a3b_cantor3", 5, { 17, 22 } }, { "mspawn", "dd_mf_a3b_vampire2", 3, { 16, 20 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_mf_a3b_cantor3", 3, { 14, 20 } }, { "mspawn", "dd_mf_a3b_vampire2", 4, { 12, 18 } } }, 2, AnyClass },
};
// Plains of Despair (level 105, src/maps/plains.json).
static constexpr Wave PlainsWaves[] = {
	{ { { "mspawn", "dd_xp_a4_willowisp3", 3, { 12, 39 } }, { "mspawn", "dd_xp_a4_vilemother1", 4, { 12, 39 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_a4_fingermage1", 4, { 12, 33 } }, { "mspawn", "dd_xp_a4_regurgitator1", 5, { 12, 33 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_a4_doomknight1", 5, { 12, 26 } }, { "mspawn", "dd_xp_a4_willowisp3", 3, { 12, 20 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_a4_vilemother1", 3, { 12, 15 } }, { "mspawn", "dd_xp_a4_fingermage1", 4, { 12, 15 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_a4_regurgitator1", 4, { 12, 9 } }, { "mspawn", "dd_xp_a4_doomknight1", 5, { 12, 3 } }, { "mspawn", "izual", 1, { 12, 3 } } }, 3, AnyClass, IzualClass },
};
// Abaddon (level 125, src/maps/abaddon.json).
static constexpr Wave AbaddonWaves[] = {
	{ { { "mspawn", "dd_xp_a4b_bloodlord3", 3, { 9, 86 } }, { "mspawn", "dd_xp_a4b_imp2", 4, { 9, 76 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_a4b_bloodlord3", 4, { 9, 74 } }, { "mspawn", "dd_xp_a4b_imp2", 5, { 10, 63 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_a4b_bloodlord3", 5, { 11, 57 } }, { "mspawn", "dd_xp_a4b_imp2", 3, { 17, 49 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_a4b_bloodlord3", 3, { 12, 40 } }, { "mspawn", "dd_xp_a4b_imp2", 4, { 11, 33 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_a4b_bloodlord3", 4, { 9, 26 } }, { "mspawn", "dd_xp_a4b_imp2", 5, { 15, 20 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_a4b_bloodlord3", 5, { 10, 17 } }, { "mspawn", "dd_xp_a4b_imp2", 3, { 12, 6 } } }, 2, AnyClass },
};
// Pit of Acheron (level 126, src/maps/acheron.json).
static constexpr Wave AcheronWaves[] = {
	{ { { "mspawn", "dd_mf_a4_bloodlord2", 3, { 18, 83 } }, { "mspawn", "dd_mf_a4_overseer3", 4, { 14, 78 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_mf_a4_bloodlord2", 4, { 18, 70 } }, { "mspawn", "dd_mf_a4_overseer3", 5, { 16, 63 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_mf_a4_bloodlord2", 5, { 16, 57 } }, { "mspawn", "dd_mf_a4_overseer3", 3, { 16, 49 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_mf_a4_bloodlord2", 3, { 14, 41 } }, { "mspawn", "dd_mf_a4_overseer3", 4, { 16, 38 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_mf_a4_bloodlord2", 4, { 16, 31 } }, { "mspawn", "dd_mf_a4_overseer3", 5, { 15, 18 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_mf_a4_bloodlord2", 5, { 15, 11 } }, { "mspawn", "dd_mf_a4_overseer3", 3, { 13, 5 } } }, 2, AnyClass },
};
// Infernal Pit (level 127, src/maps/infernal.json).
static constexpr Wave InfernalWaves[] = {
	{ { { "mspawn", "dd_craft_a4_imp4", 3, { 18, 83 } }, { "mspawn", "dd_craft_a4_overseer4", 4, { 14, 78 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_craft_a4_imp4", 4, { 18, 70 } }, { "mspawn", "dd_craft_a4_overseer4", 5, { 16, 63 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_craft_a4_imp4", 5, { 16, 57 } }, { "mspawn", "dd_craft_a4_overseer4", 3, { 16, 49 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_craft_a4_imp4", 3, { 14, 41 } }, { "mspawn", "dd_craft_a4_overseer4", 4, { 16, 38 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_craft_a4_imp4", 4, { 16, 31 } }, { "mspawn", "dd_craft_a4_overseer4", 5, { 15, 20 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_craft_a4_imp4", 5, { 14, 10 } }, { "mspawn", "dd_craft_a4_overseer4", 3, { 12, 6 } } }, 2, AnyClass },
};
// Hellforge (level 107, src/maps/hellforge.json).
static constexpr Wave HellforgeWaves[] = {
	{ { { "mspawn", "dd_craft_a4b_blunderbore4", 3, { 11, 12 } }, { "mspawn", "dd_craft_a4b_vilemother3", 4, { 11, 12 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_craft_a4b_fingermage2", 4, { 15, 11 } }, { "mspawn", "dd_craft_a4b_doomknight2", 5, { 19, 14 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_craft_a4b_blunderbore4", 5, { 21, 9 } }, { "mspawn", "dd_craft_a4b_vilemother3", 3, { 22, 8 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_craft_a4b_fingermage2", 3, { 29, 15 } }, { "mspawn", "dd_craft_a4b_doomknight2", 4, { 33, 11 } }, { "monster", "super:41", 1, { 33, 11 } } }, 3, AnyClass, HephastoClass },
};
// Bloody Foothills (level 116, src/maps/shenk.json).
static constexpr Wave ShenkWaves[] = {
	{ { { "mspawn", "dd_shenk_minion1", 3, { 12, 36 } }, { "mspawn", "dd_shenk_deathmauler1", 4, { 11, 33 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_shenk_imp1", 4, { 11, 33 } }, { "mspawn", "dd_shenk_minion1", 5, { 10, 24 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_shenk_deathmauler1", 5, { 10, 24 } }, { "mspawn", "dd_shenk_imp1", 3, { 9, 18 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_shenk_minion1", 3, { 8, 16 } }, { "mspawn", "dd_shenk_deathmauler1", 4, { 11, 12 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_shenk_imp1", 4, { 6, 10 } }, { "monster", "super:42", 1, { 4, 6 } }, { "monster", "super:49", 1, { 4, 6 } } }, 3, AnyClass, ShenkClass },
};
// Frigid Highlands (level 113, src/maps/highlands.json).
static constexpr Wave HighlandsWaves[] = {
	{ { { "mspawn", "dd_xp_a5_imp1", 3, { 10, 43 } }, { "mspawn", "dd_xp_a5_siegebeast2", 4, { 11, 38 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_a5_minion1", 4, { 10, 32 } }, { "mspawn", "dd_xp_a5_imp1", 5, { 10, 32 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_a5_siegebeast2", 5, { 11, 27 } }, { "mspawn", "dd_xp_a5_minion1", 3, { 9, 23 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_a5_imp1", 3, { 8, 18 } }, { "mspawn", "dd_xp_a5_siegebeast2", 4, { 8, 18 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_xp_a5_minion1", 4, { 10, 11 } }, { "mspawn", "dd_xp_a5_imp1", 5, { 9, 9 } } }, 2, AnyClass },
};
// Pindleskin (level 121, src/maps/pindle.json).
static constexpr Wave PindleWaves[] = {
	{ { { "mspawn", "dd_mf_a5_reanimatedhorde1", 3, { 15, 20 } }, { "mspawn", "dd_mf_a5_succubus1", 4, { 14, 19 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_mf_a5_reanimatedhorde1", 4, { 14, 16 } }, { "mspawn", "dd_mf_a5_succubus1", 5, { 13, 16 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_mf_a5_reanimatedhorde1", 5, { 12, 15 } }, { "mspawn", "dd_mf_a5_succubus1", 3, { 12, 11 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_mf_a5_reanimatedhorde1", 3, { 12, 9 } }, { "mspawn", "dd_mf_a5_succubus1", 4, { 12, 8 } }, { "monster", "super:52", 1, { 11, 7 } } }, 3, AnyClass, PindleskinClass },
};
// Icy Cellar (level 119, src/maps/cellar.json).
static constexpr Wave CellarWaves[] = {
	{ { { "mspawn", "dd_craft_a5_snowyeti4", 3, { 5, 55 } }, { "mspawn", "dd_craft_a5_succubus3", 4, { 4, 46 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_craft_a5_reanimatedhorde3", 4, { 7, 43 } }, { "mspawn", "dd_craft_a5_snowyeti4", 5, { 7, 35 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_craft_a5_succubus3", 5, { 7, 35 } }, { "mspawn", "dd_craft_a5_reanimatedhorde3", 3, { 6, 30 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_craft_a5_snowyeti4", 3, { 9, 19 } }, { "mspawn", "dd_craft_a5_succubus3", 4, { 9, 19 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_craft_a5_reanimatedhorde3", 4, { 5, 13 } }, { "mspawn", "dd_craft_a5_snowyeti4", 5, { 6, 8 } } }, 2, AnyClass },
};
// Frozen River (level 114, src/maps/anya.json).
static constexpr Wave AnyaWaves[] = {
	{ { { "mspawn", "dd_mf_a5b_snowyeti2", 3, { 39, 9 } }, { "mspawn", "dd_mf_a5b_succubus5", 4, { 36, 19 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_mf_a5b_reanimatedhorde1", 4, { 31, 21 } }, { "mspawn", "dd_mf_a5b_snowyeti2", 5, { 31, 29 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_mf_a5b_succubus5", 5, { 25, 36 } }, { "mspawn", "dd_mf_a5b_reanimatedhorde1", 3, { 21, 37 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_mf_a5b_snowyeti2", 3, { 16, 46 } }, { "mspawn", "dd_mf_a5b_succubus5", 4, { 12, 50 } }, { "monster", "super:59", 1, { 12, 52 } } }, 3, AnyClass },
};
// Halls of Vaught (level 124, src/maps/vaught.json).
static constexpr Wave VaughtWaves[] = {
	{ { { "mspawn", "dd_mf_a5c_minion6", 3, { 42, 74 } }, { "mspawn", "dd_mf_a5c_succubus4", 4, { 41, 73 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_mf_a5c_putriddefiler2", 4, { 41, 65 } }, { "mspawn", "dd_mf_a5c_minion6", 5, { 41, 65 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_mf_a5c_succubus4", 5, { 40, 60 } }, { "mspawn", "dd_mf_a5c_putriddefiler2", 3, { 41, 54 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_mf_a5c_minion6", 3, { 39, 48 } }, { "mspawn", "dd_mf_a5c_succubus4", 4, { 40, 45 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_mf_a5c_putriddefiler2", 4, { 40, 45 } }, { "mspawn", "dd_mf_a5c_minion6", 5, { 42, 38 } }, { "monster", "super:60", 1, { 41, 36 } } }, 3, AnyClass, NihlathakClass },
};
// Cow Level (level 39, src/maps/cows.json).
static constexpr Wave CowWaves[] = {
	{ { { "mspawn", "dd_craft_a5b_hellbovine", 3, { 6, 24 } }, { "mspawn", "dd_craft_a5b_hellbovine", 4, { 6, 24 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_craft_a5b_hellbovine", 4, { 6, 24 } }, { "mspawn", "dd_craft_a5b_hellbovine", 5, { 6, 24 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_craft_a5b_hellbovine", 5, { 6, 24 } }, { "mspawn", "dd_craft_a5b_hellbovine", 3, { 24, 8 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_craft_a5b_hellbovine", 3, { 24, 8 } }, { "mspawn", "dd_craft_a5b_hellbovine", 4, { 24, 8 } } }, 2, AnyClass },
	{ { { "mspawn", "dd_craft_a5b_hellbovine", 4, { 24, 8 } }, { "mspawn", "dd_craft_a5b_hellbovine", 5, { 24, 8 } }, { "monster", "super:39", 1, { 24, 8 } } }, 3, AnyClass, CowKingClass },
};
// Arreat Summit (level 120, src/maps/ancients.json): the three Ancients at once (their altar is gone).
static constexpr Wave AncientsWaves[] = {
	{ { { "monster", "super:43", 1, { 9, 13 } }, { "monster", "super:44", 1, { 7, 15 } }, { "monster", "super:45", 1, { 11, 15 } } }, 3, AnyClass },
};

static constexpr Arena Arenas[] = {
	{ "Den of Evil",    DenOfEvilLevelId, 1500, 2500, DenWaves,    static_cast<int>(sizeof(DenWaves) / sizeof(DenWaves[0])) },
	{ "Burial Grounds", 9,                1500, 1300, BurialWaves, static_cast<int>(sizeof(BurialWaves) / sizeof(BurialWaves[0])) },
	{ "Tree of Inifuss", 11,              1500, 1900, InifussWaves, static_cast<int>(sizeof(InifussWaves) / sizeof(InifussWaves[0])) },
	{ "Mausoleum",      14,               1500, 2624, MausoleumWaves, static_cast<int>(sizeof(MausoleumWaves) / sizeof(MausoleumWaves[0])), true, { 16, 16 }, 10 },
	{ "Crypt",          15,               1500, 2748, CryptWaves,     static_cast<int>(sizeof(CryptWaves) / sizeof(CryptWaves[0])),         true, { 16, 16 }, 10, AnyClass, true, true },
	{ "Pit",            16,               1500, 2872, PitWaves,       static_cast<int>(sizeof(PitWaves) / sizeof(PitWaves[0])),             false, { 0, 0 }, 0, AnyClass, true, true },
	{ "Countess",       25,               2500, 2200, CountessWaves,  static_cast<int>(sizeof(CountessWaves) / sizeof(CountessWaves[0])),   true, { 0, 0 }, 0 },
	{ "Malus",          28,               5500, 1000, MalusWaves,     static_cast<int>(sizeof(MalusWaves) / sizeof(MalusWaves[0])),         true, { 10, 7 }, 7, SmithClass },
	// Andariel: no waves; her lair's packs, uniques and Andariel herself come from andariel.cpp. Her death clears it.
	{ "Andariel",       37,               4500, 1900, nullptr,        0,                                                                     false, { 0, 0 }, 0, AndarielClass },
	{ "Halls of the Dead", 60,            3000, 2500, HallsWaves,  static_cast<int>(sizeof(HallsWaves) / sizeof(HallsWaves[0])),  false, { 0, 0 }, 0, AnyClass, false, false, 1, "box", CubeChestRow, { 10, 42 }, true },
	{ "Sewers",         49,               1500, 1600, SewersWaves, static_cast<int>(sizeof(SewersWaves) / sizeof(SewersWaves[0])), false, { 0, 0 }, 0, AnyClass, false, false, 1, nullptr, -1, { 5, 30 }, true },
	{ "Stony Tomb",     55,               3000, 1000, StonyWaves,  static_cast<int>(sizeof(StonyWaves) / sizeof(StonyWaves[0])),   true, { 16, 16 }, 11, AnyClass, false, false, 1, nullptr, -1, { 4, 16 } },
	{ "Maggot Lair",    64,               3500, 1600, MaggotWaves,    static_cast<int>(sizeof(MaggotWaves) / sizeof(MaggotWaves[0])),       false, { 0, 0 }, 0, AnyClass, false, false, 1, "msf", StaffChestRow, { 4, 27 }, true },
	{ "Claw Viper Temple", 61,            3000, 2800, ClawViperWaves, static_cast<int>(sizeof(ClawViperWaves) / sizeof(ClawViperWaves[0])), false, { 0, 0 }, 0, AnyClass, false, false, 1, "vip", SunAltarRow, { 8, 4 } },
	{ "Arcane Sanctuary", 74,             5000, 1000, ArcaneWaves,    static_cast<int>(sizeof(ArcaneWaves) / sizeof(ArcaneWaves[0])),       false, { 0, 0 }, 0, SummonerClass, false, false, 1, nullptr, -1, { 4, 41 }, true },
	{ "Ancient Tunnels", 65,              4000, 1000, TunnelsWaves,   static_cast<int>(sizeof(TunnelsWaves) / sizeof(TunnelsWaves[0])),     false, { 0, 0 }, 0, AnyClass, true, true, 1, nullptr, -1, { 5, 42 }, true },
	{ "Canyon of the Magi", 59,           3000, 2200, CanyonWaves,    static_cast<int>(sizeof(CanyonWaves) / sizeof(CanyonWaves[0])),       true, { 12, 12 }, 9, AnyClass, false, false, 1, nullptr, -1, { 3, 12 } },
	// Duriel: the hero arrives in the orifice room (level 66, no waves); inserting the staff opens
	// the way to his lair (level 73, Blizzard's), where his death clears the stop.
	{ "Tal Rasha's Tomb", 66,             4500, 1000, nullptr,        0,                                                                     false, { 0, 0 }, 0, AnyClass, false, false, 1, nullptr, -1, { 9, 4 } },
	{ "Duriel",         73,               4500, 3100, nullptr,        0,                                                                     false, { 0, 0 }, 0, DurielClass },
	{ "Spider Cavern", 85, 1500, 1300, SpiderWaves, static_cast<int>(sizeof(SpiderWaves) / sizeof(SpiderWaves[0])), false, { 0, 0 }, 0, AnyClass, true, false, 1, "qey", 407, { 4, 15 }, false, -1, 1 },
	{ "Flayer Dungeon", 91, 2000, 2340, FlayerWaves, static_cast<int>(sizeof(FlayerWaves) / sizeof(FlayerWaves[0])), false, { 0, 0 }, 0, AnyClass, true, false, 1, "qbr", 406, { 4, 20 }, true, -1, 1 },
	{ "Kurast Sewers", 93, 2500, 1300, KSewersWaves, static_cast<int>(sizeof(KSewersWaves) / sizeof(KSewersWaves[0])), false, { 0, 0 }, 0, AnyClass, true, false, 1, "qhr", 405, { 9, 19 }, true, -1, 1 },
	{ "Ruined Temple", 94, 3000, 1000, LamEsenWaves, static_cast<int>(sizeof(LamEsenWaves) / sizeof(LamEsenWaves[0])), false, { 0, 0 }, 0, AnyClass, false, false, 1, "bbb", 193, { 13, 5 }, false, -1, 1 },
	{ "Swampy Pit", 90, 2000, 2200, SwampyWaves, static_cast<int>(sizeof(SwampyWaves) / sizeof(SwampyWaves[0])), false, { 0, 0 }, 0, AnyClass, true, true, 1, nullptr, -1, { 2, 2 }, false, -1, 1 },
	{ "Lower Kurast", 95, 3000, 1124, LowerKurastWaves, static_cast<int>(sizeof(LowerKurastWaves) / sizeof(LowerKurastWaves[0])), false, { 0, 0 }, 0, AnyClass, false, false, 1, nullptr, -1, { 13, 46 }, true, -1, 1 },
	{ "Travincal", 96, 3000, 1248, TravincalWaves, static_cast<int>(sizeof(TravincalWaves) / sizeof(TravincalWaves[0])), false, { 0, 0 }, 0, AnyClass, false, false, 1, nullptr, -1, { 28, 24 }, false, 404, 1 },
	{ "Plains of Despair", 105, 2000, 1000, PlainsWaves, static_cast<int>(sizeof(PlainsWaves) / sizeof(PlainsWaves[0])), false, { 0, 0 }, 0, AnyClass, false, false, 1, nullptr, -1, { 12, 46 }, true, -1, 1 },
	{ "Abaddon", 125, 2500, 1184, AbaddonWaves, static_cast<int>(sizeof(AbaddonWaves) / sizeof(AbaddonWaves[0])), false, { 0, 0 }, 0, AnyClass, false, false, 1, nullptr, -1, { 10, 91 }, true, -1, 1 },
	{ "Pit of Acheron", 126, 2500, 1484, AcheronWaves, static_cast<int>(sizeof(AcheronWaves) / sizeof(AcheronWaves[0])), false, { 0, 0 }, 0, AnyClass, true, true, 1, nullptr, -1, { 20, 92 }, true, -1, 1 },
	{ "Infernal Pit", 127, 2500, 1784, InfernalWaves, static_cast<int>(sizeof(InfernalWaves) / sizeof(InfernalWaves[0])), false, { 0, 0 }, 0, AnyClass, false, false, 1, nullptr, -1, { 20, 92 }, true, -1, 1 },
	{ "Hellforge", 107, 2000, 1000, HellforgeWaves, static_cast<int>(sizeof(HellforgeWaves) / sizeof(HellforgeWaves[0])), false, { 0, 0 }, 0, AnyClass, false, false, 1, nullptr, -1, { 4, 12 }, false, -1, 1 },
	{ "Bloody Foothills", 116, 2000, 1764, ShenkWaves, static_cast<int>(sizeof(ShenkWaves) / sizeof(ShenkWaves[0])), false, { 0, 0 }, 0, AnyClass, false, false, 1, nullptr, -1, { 11, 40 }, true, -1, 1 },
	{ "Frigid Highlands", 113, 2000, 1000, HighlandsWaves, static_cast<int>(sizeof(HighlandsWaves) / sizeof(HighlandsWaves[0])), false, { 0, 0 }, 0, AnyClass, false, false, 1, nullptr, -1, { 12, 46 }, true, 474, 3 },
	{ "Pindleskin", 121, 2000, 2636, PindleWaves, static_cast<int>(sizeof(PindleWaves) / sizeof(PindleWaves[0])), false, { 0, 0 }, 0, AnyClass, false, false, 1, nullptr, -1, { 15, 22 }, true, -1, 1 },
	{ "Icy Cellar", 119, 2000, 2376, CellarWaves, static_cast<int>(sizeof(CellarWaves) / sizeof(CellarWaves[0])), false, { 0, 0 }, 0, AnyClass, false, false, 1, nullptr, -1, { 5, 57 }, true, -1, 1 },
	{ "Frozen River", 114, 2000, 1300, AnyaWaves, static_cast<int>(sizeof(AnyaWaves) / sizeof(AnyaWaves[0])), false, { 0, 0 }, 0, AnyClass, false, false, 1, nullptr, -1, { 43, 7 }, false, 461, 1 },
	{ "Halls of Vaught", 124, 2500, 1000, VaughtWaves, static_cast<int>(sizeof(VaughtWaves) / sizeof(VaughtWaves[0])), false, { 0, 0 }, 0, AnyClass, false, false, 1, nullptr, -1, { 42, 79 }, false, -1, 1 },
	{ "Cow Level", 39, 5000, 1148, CowWaves, static_cast<int>(sizeof(CowWaves) / sizeof(CowWaves[0])), false, { 0, 0 }, 0, AnyClass, false, false, 1, nullptr, -1, { 6, 24 }, false, -1, 1 },
	{ "Arreat Summit", 120, 2000, 2508, AncientsWaves, 1, false, { 0, 0 }, 0, AnyClass, false, false, 1, nullptr, -1, { 11, 25 } },
	{ "Durance of Hate", 102, 3500, 1600, nullptr, 0, false, { 0, 0 }, 0, MephistoClass, false, false, 1, nullptr, -1, { 29, 19 } },
	{ "Chaos Sanctuary", 108, 1500, 1000, nullptr, 0, false, { 0, 0 }, 0, DiabloClass },
	{ "Throne of Destruction", 131, 3000, 1000, nullptr, 0, false, { 0, 0 }, 0, AnyClass, false, false, 1, nullptr, -1, { 3, 48 } },
	{ "Worldstone Chamber", 132, 3000, 1152, nullptr, 0, false, { 0, 0 }, 0, BaalClass },
	// Tristram: freeing Cain from his gibbet (objects.txt row 26) clears it; a hero who freed him in
	// an earlier game (the gibbet is already open) clears it by killing Griswold.
	{ "Tristram",       38,               0,    0,    TristramWaves,  static_cast<int>(sizeof(TristramWaves) / sizeof(TristramWaves[0])),   true, { 0, 0 }, 0, GriswoldClass, false, false, 3, nullptr, -1, { -1, -1 }, false, GibbetRow, 1 },
};

// ---- Spawning off screen ----------------------------------------------------------------
// Waves come in from beyond the edge of the screen, so nothing appears out of thin air and the
// hero isn't buried under a pack. The game draws the world at a slant: a sub-tile step is 16
// pixels across and 8 down per axis, at the classic scale (D2R's screen holds about 1068x600 of
// those pixels high, scaled to fit the window, so the resolution doesn't matter but the window's
// shape does: a wider window shows more ground left and right). A spot counts as off screen when
// it lies past the screen's edge plus a margin.
static constexpr double ScreenHalfHeight = 300.0;
static constexpr double ScreenMarginX    = 64.0;
static constexpr double ScreenMarginY    = 48.0;
static constexpr double MaxSpawnSubtiles = 60.0;  // no further from the hero (the game only keeps nearby rooms loaded)
static std::atomic<double> g_screenAspect { 16.0 / 9.0 };

// Width / height of the game's window (its largest visible window in this process), read before
// each wave, so a window resized or moved to another screen counts too. Kept between 4:3 and
// 21:9, the shapes the game draws the world at.
static void ReadScreenAspect() {
	struct Found { DWORD process; long width, height; } found { GetCurrentProcessId(), 0, 0 };
	EnumWindows([](HWND window, LPARAM param) -> BOOL {
		Found& f = *reinterpret_cast<Found*>(param);
		DWORD process = 0;
		GetWindowThreadProcessId(window, &process);
		RECT r {};
		if (process == f.process && IsWindowVisible(window) && GetClientRect(window, &r) &&
			static_cast<long long>(r.right) * r.bottom > static_cast<long long>(f.width) * f.height) {
			f.width = r.right;
			f.height = r.bottom;
		}
		return TRUE;
	}, reinterpret_cast<LPARAM>(&found));
	if (found.width > 0 && found.height > 0) {
		g_screenAspect = (std::min)(21.0 / 9.0, (std::max)(4.0 / 3.0, static_cast<double>(found.width) / found.height));
	}
}

// How far past the screen's edge a spot is: below 1 on screen, above 1 off it. dx, dy in sub-tiles.
static double ScreenReach(double dx, double dy) {
	const double halfWidth = ScreenHalfHeight * g_screenAspect.load() + ScreenMarginX;
	return (std::max)(std::fabs((dx - dy) * 16.0) / halfWidth, std::fabs((dx + dy) * 8.0) / (ScreenHalfHeight + ScreenMarginY));
}

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

// The random unique that clears the stop (clearOnRandomUnique): which pack it leads, and its unit
// number once it has spawned and been found.
static std::atomic<int>      g_targetPack { -1 };  // packs counted from 0 over all waves
static std::atomic<int>      g_packsSpawned { 0 };
static std::atomic<uint32_t> g_targetUnit { 0 };   // 0: not found (yet)
static std::mutex            g_uniquesLock;
static std::vector<uint32_t> g_uniquesBefore;      // unique monsters loaded before the target spawned

// The stop whose run is going on in this game ("" none). A run found in progress with no game of
// its own was left by quitting.
static std::string g_runStop;

// The hero's route progress (defined with the route map below).
static auto LoadProgress() noexcept -> Route::Progress;
static auto SaveProgress(const D2RL::PluginContext* context, const Route::Progress& progress) noexcept -> bool;

// A quest stop clears when both are true (2026-11-22: a hero ran to the Cube with one wave out,
// and the stop ended almost empty): the quest item taken, and every wave out and dead.
// The stop the hero last picked on the route map (its travel level; -1 none). A new run starts only
// from a pick: walking back in through a Town Portal left open used to start the stop again and
// spend the fork's second pick without the hero knowing (2026-11-22, Ancient Tunnels).
static std::atomic<int32_t> g_pickedTravel { -1 };

static std::atomic<bool> g_questDone { false };
static std::atomic<bool> g_wavesDone { false };

static void ResetArena() noexcept {
	g_targetPack = -1;
	g_packsSpawned = 0;
	g_targetUnit = 0;
	++g_run;
	g_arena = -1;
	g_wave = 0;
	g_remaining = 0;
	g_bossCount = 0;
	g_bossesDead = 0;
	g_portalDropped = false;
	g_questDone = false;
	g_wavesDone = false;
	g_finalKills = 0;
	g_finalSize = 0;
	WaveOverlay::Hide();
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

// True if the level has target types: its target unique spawns as "<type>_t" and is known by its
// class when it dies (no memory search needed).
static auto HasTargetTypes(int32_t levelId) noexcept -> bool {
	for (const auto& t : g_targetClasses) {
		if (t.levelId == levelId) {
			return true;
		}
	}
	return false;
}

static auto IsTargetClass(int32_t levelId, uint32_t classId) noexcept -> bool {
	for (const auto& t : g_targetClasses) {
		if (t.levelId == levelId && t.classId == classId) {
			return true;
		}
	}
	return false;
}

// UI thread, a moment after the target's pack spawned: the target is the unique monster that
// wasn't there before.
static void __cdecl FindTargetUnique(const D2RL::PluginContext* context, void* run) noexcept {
	if (reinterpret_cast<uintptr_t>(run) != g_run.load()) {
		return;
	}
	std::vector<uint32_t> before;
	{
		const std::lock_guard<std::mutex> guard(g_uniquesLock);
		before = g_uniquesBefore;
	}
	for (const Game::MonsterDetails& m : Game::Monsters(-1, true)) {
		if (m.unique && !m.superUnique && !m.minion && std::find(before.begin(), before.end(), m.unit.id) == before.end()) {
			g_targetUnit = m.unit.id;
			char message[96] {};
			std::snprintf(message, sizeof(message), "Target unique: unit %u (monstats row %u).", m.unit.id, m.unit.row);
			context->LogInfo(message);
			Trace(context, message);
			return;
		}
	}
	context->LogWarn("Target unique: not found; the stop clears when everything is dead.");
	Trace(context, "Target unique: not found; the stop clears when everything is dead.");
}

// ---- The camp's potion supply -------------------------------------------------------------
// Shops sell no health potions and nothing drops them (src/patches/006_no_health_potions.py).
// Instead every cleared stop earns PotionsPerClear, handed out in town into the hero's stash (the
// bag if the stash is full). What doesn't fit stays owed (in the route progress file) and comes
// at the next visit, so unclaimed potions pile up.
static constexpr int PotionsPerClear = 3;
static std::atomic<uint32_t> g_difficulty { 0 };  // as last seen from a monster's death
static constexpr const char* PotionByDifficulty[] = { "hp2", "hp4", "hp5" };  // light, greater, super

// Game thread, in town: hand out what is owed.
static void __cdecl DeliverPotions(const D2RL::PluginContext* context, void*) noexcept {
	Route::Progress progress = LoadProgress();
	if (progress.potionsOwed <= 0) {
		return;
	}
	D2RL::PlayerHandle player = D2RL::InvalidPlayerHandle;
	if (g_inventory->getLocalPlayer(context, &player) != D2RL::Inventory::Result::Success) {
		return;
	}
	const char* code = PotionByDifficulty[(std::min)(g_difficulty.load(), 2U)];
	int given = 0;
	while (progress.potionsOwed > 0) {
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
				.container  = D2RL::Items::ItemContainer::PersonalStash,
				.placement  = D2RL::Items::Placement::Automatic,
			},
		};
		D2RL::ItemHandle item = D2RL::InvalidItemHandle;
		auto result = g_items->createItem(context, player, &spec, &item);
		if (result != D2RL::Items::Result::Success) {  // never the ground: plugin ground drops crash the game
			spec.destination.container = D2RL::Items::ItemContainer::Inventory;
			result = g_items->createItem(context, player, &spec, &item);
		}
		if (result != D2RL::Items::Result::Success) {
			break;  // no room: the rest stays owed
		}
		--progress.potionsOwed;
		++given;
	}
	SaveProgress(context, progress);
	char message[128] {};
	std::snprintf(message, sizeof(message), "Potion supply: %d %s handed out, %d still owed.", given, code, progress.potionsOwed);
	context->LogInfo(message);
	Trace(context, message);
}

// ---- No healing from Akara -------------------------------------------------------------------
// Healing is the potion supply's job (above). The game heals the hero whenever they talk to Akara,
// decided by who she is, with no setting for it, so in town a watcher undoes it: when the hero's
// life or mana jumps to full while standing at Akara, it is written back to what it was a moment
// before (gamewrite.h, the one place that writes game memory). Drinking a full rejuvenation right
// next to her gets undone too.
static constexpr uint32_t AkaraClass   = 148;  // monstats row of akara
static constexpr int      AkaraReach   = 12;   // sub-tiles: talking range, with some slack
static std::atomic<bool>  g_healWatch { false };

static void WatchTownHealing(const D2RL::PluginContext* context, int townLevel) noexcept {
	if (g_healWatch.exchange(true)) {
		return;  // already watching
	}
	std::thread([context, townLevel] {
		try {
			const Game::Unit hero = Game::Hero();
			const auto copies = hero ? GameWrite::Copies(hero) : std::vector<uintptr_t> {};
			Game::HeroDetails last = Game::HeroInfo();
			while (g_levelId == townLevel && !copies.empty()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				const Game::HeroDetails now = Game::HeroInfo();
				if (now.mana != last.mana) {  // temporary: mana potions in town (2026-11-20)
					char seen[96] {};
					std::snprintf(seen, sizeof(seen), "Town: mana %d -> %d of %d.", last.mana, now.mana, now.maxMana);
					Trace(context, seen);
				}
				bool atAkara = false;
				for (const Game::MonsterDetails& m : Game::Monsters(AkaraReach, true)) {
					atAkara = atAkara || m.unit.row == AkaraClass;
				}
				const bool lifeJump = now.life >= now.maxLife && last.life >= 0 && last.life < now.maxLife && now.life - last.life >= (std::max)(5, now.maxLife / 10);
				const bool manaJump = now.mana >= now.maxMana && last.mana >= 0 && last.mana < now.maxMana && now.mana - last.mana >= (std::max)(5, now.maxMana / 10);
				if (atAkara && (lifeJump || manaJump)) {
					if (lifeJump) {
						GameWrite::SetStat(copies, Game::StatId::Life, last.life);
					}
					if (manaJump) {
						GameWrite::SetStat(copies, Game::StatId::Mana, last.mana);
					}
					char message[128] {};
					std::snprintf(message, sizeof(message), "Akara's healing undone: life %d -> %d, mana %d -> %d.", now.life,
						lifeJump ? last.life : now.life, now.mana, manaJump ? last.mana : now.mana);
					context->LogInfo(message);
					Trace(context, message);
					continue;  // keep the old values as "last"
				}
				last = now;
			}
		} catch (...) {
		}
		g_healWatch = false;
	}).detach();
}

// ---- Rewards of Acts I-V given by the plugin (docs/act3.md, act4.md, act5.md) -----------------
// Game thread: a Socket Chisel (114a_act5.py; cube it with an item without sockets).
static void __cdecl GiveChisel(const D2RL::PluginContext* context, void*) noexcept {
	GiveItem(context, "zsk", "Socket Chisel");
}

static auto RandomRune(int lowest, int highest, int topWeight) noexcept -> int {
	// Runes lowest..highest (r<nn>): the five highest (Sur..Zod, r29-r33) at `topWeight` each out of
	// 100 when in range, the rest share what is left evenly.
	static std::mt19937 random { std::random_device {}() };
	int top = 0, rest = 0;
	for (int r = lowest; r <= highest; ++r) {
		(r >= 29 ? top : rest) += 1;
	}
	const int restWeight = rest > 0 ? (100 * 8 - top * topWeight * 8) / rest : 0;
	int total = 0;
	for (int r = lowest; r <= highest; ++r) {
		total += r >= 29 ? topWeight * 8 : restWeight;
	}
	int roll = std::uniform_int_distribution<int>(0, total - 1)(random);
	for (int r = lowest; r <= highest; ++r) {
		roll -= r >= 29 ? topWeight * 8 : restWeight;
		if (roll < 0) {
			return r;
		}
	}
	return highest;
}

// Game thread: the Hellforge's three runes, Pul (r21) or higher; Sur, Ber, Jah, Cham, Zod 5% each.
static void __cdecl GiveHellforgeRunes(const D2RL::PluginContext* context, void*) noexcept {
	for (int k = 0; k < 3; ++k) {
		char code[4] {};
		std::snprintf(code, sizeof(code), "r%02d", RandomRune(21, 33, 5));
		GiveItem(context, code, "Hellforge rune");
	}
}

// Game thread: the barbarians' three runes, Pul to Ohm (r21-r27), evenly.
static void __cdecl GiveBarbarianRunes(const D2RL::PluginContext* context, void*) noexcept {
	for (int k = 0; k < 3; ++k) {
		char code[4] {};
		std::snprintf(code, sizeof(code), "r%02d", RandomRune(21, 27, 0));
		GiveItem(context, code, "Barbarians' rune");
	}
}

static void OpenAnyaScroll(const D2RL::PluginContext* context) noexcept;

// ---- The level curve (Act I, decided 2026-11-22) ----------------------------------------
// A hero arrives at Andariel at level 15 having taken no XP grind at the forks, and one level
// higher for each of the four fork picks that was an XP grind (19 with all four). The monsters'
// own experience falls far short of that (about 50,000 for the whole act), so every counted clear
// adds the rest: the stop's share of the curve minus what its monsters give (estimated from the
// wave tables, monstats Exp and monlvl.txt, 2026-11-22). Shares, as total experience at the end of
// each step: Den 3,750 (level 4), Burial Grounds 14,175 (6), fork 1 57,715 (9), Tristram 112,725
// (13), fork 2 176,132 (15); an XP grind gives 63,470 more than the other picks of its fork
// ((430,010 - 176,132) / 4, level 19 with four).
// Acts II-V (decided 2026-11-22) follow the same rule, 18 levels an act: the hero arrives at Duriel
// at 33, Mephisto 51, Diablo 69 and Baal 87 with no XP grind, one level higher per XP grind pick
// (worth one level at the act's end). Each act's clears share the climb evenly (a fork pick is one
// share; the boss's own kill is taken off the next act's first share), less the stop's estimated
// monster experience, which is small next to it. Numbers from experience.txt; tune after play.
struct StopExperience { const char* stop; int experience; };
static constexpr StopExperience ClearExperience[] = {
	{ "den",        1257 }, { "burial",     8038 },
	{ "inifuss",   16668 }, { "mausoleum", 16919 }, { "crypt",  67303 },
	{ "tristram",  36755 },
	{ "pit",       24080 }, { "countess",  27341 }, { "malus",  83881 },
	// Act II: Duriel at 33 (37 with four XP grinds); an XP grind is worth 1,074,627 more.
	{ "halls", 1020000 }, { "sewers", 2090000 }, { "stony", 1030000 },
	{ "maggot", 1015000 }, { "clawviper", 1020000 }, { "arcane", 1026000 },
	{ "tunnels", 1015000 }, { "canyon", 2059000 },
	// Act III: Mephisto at 51 (53 with two); XP grind +5,069,147.
	{ "spider", 7331000 }, { "flayer", 7355000 }, { "ksewers", 7334000 },
	{ "lamesen", 12374000 }, { "swampy", 7351000 }, { "lowerkurast", 7397000 },
	{ "travincal", 7320000 },
	// Act IV: Diablo at 69 (71 with two); XP grind +23,911,777.
	{ "plains", 51972000 }, { "abaddon", 75798000 }, { "acheron", 52012000 },
	{ "infernal", 52341000 }, { "hellforge", 52296000 },
	// Act V: Baal at 87 (89 with two), plus the Ancients' level; XP grind +112,794,729.
	{ "shenk", 140643000 }, { "highlands", 253373000 }, { "pindle", 140873000 },
	{ "cellar", 141084000 }, { "anya", 140760000 }, { "vaught", 140621000 },
	{ "cows", 141059000 }, { "ancients", 141084000 },
};

// UI thread. A stop's once-per-hero reward (quest rewards given once per difficulty are tripled,
// docs/flattening.md; the game's own part of a reward comes from the quest as usual).
static void FirstClearReward(const D2RL::PluginContext* context, const Route::Stop& stop, Route::Progress& progress,
	const std::filesystem::path& file) noexcept {
	const std::string id = stop.id;
	char command[96] {};
	if (id == "lamesen") {         // the tome gives 5 stat points when read; 10 more here
		const int points = Game::Stat(Game::Hero(), Game::StatId::StatPoints, 0, false).value_or(0);
		std::snprintf(command, sizeof(command), "cheat stat statpts,%d", points + 10);
		RunConsole(context, command);
	} else if (id == "plains") {   // Tyrael gives Izual's 2 skill points; 4 more here
		RunConsole(context, "cheat skillpoints 4");
	} else if (id == "hellforge") {
		g_threads->runOnGameThread(context, GiveHellforgeRunes, nullptr);
	} else if (id == "highlands") {
		g_threads->runOnGameThread(context, GiveBarbarianRunes, nullptr);
	} else if (id == "malus" || id == "shenk") {  // Charsi's socket; Larzuk's second socket
		g_threads->runOnGameThread(context, GiveChisel, nullptr);
	} else if (id == "ancients") { // a full level instead of the usual experience
		const int level = Game::HeroInfo().level;
		if (level > 0 && level < 99) {
			std::snprintf(command, sizeof(command), "cheat level %d", level + 1);
			RunConsole(context, command);
		}
	} else if (id == "anya") {     // her scroll: +5 to the maximum of one resistance, the hero's choice
		progress.flags["anyaowed"] = 1;
		Route::Save(file, progress);
		OpenAnyaScroll(context);
	}
}

// UI thread. All waves spawned and every spawned monster (and the boss) dead: give the scroll.
// UI thread. The run's boss is dead: noted in the hero's progress, so a run left before its last
// monsters die (town, save and exit) clears on return instead of bringing the boss back for a
// second kill and a second drop (Blood Raven, 2026-11-22).
static void __cdecl MarkBossDown(const D2RL::PluginContext* context, void*) noexcept {
	Route::Progress progress = LoadProgress();
	if (progress.running.empty() || progress.Flag("bossdown:" + progress.running) != 0) {
		return;
	}
	progress.flags["bossdown:" + progress.running] = 1;
	SaveProgress(context, progress);
}

static void __cdecl ClearArena(const D2RL::PluginContext* context, void* run) noexcept {
	const int index = g_arena.load();
	if (index < 0 || reinterpret_cast<uintptr_t>(run) != g_run.load() || g_portalDropped.exchange(true)) {
		return;
	}
	if (g_threads->runOnGameThread(context, DropTownPortal, nullptr) != D2RL::Threads::Result::Success) {
		context->LogWarn("Town Portal: could not queue the drop.");
	}
	WaveOverlay::Cleared();
	char message[160] {};
	std::snprintf(message, sizeof(message), "%s: cleared.", Arenas[index].name);
	context->LogInfo(message);
		Trace(context, message);
	// The hero's route progress: this stop counts as cleared (route.h).
	try {
		const Route::Stop* stop = Route::StopByArenaLevel(Arenas[index].levelId);
		const auto file = Campfire::RouteProgressFile();
		if (stop != nullptr && !file.empty()) {
			Route::Progress progress = Route::Load(file);
			const bool counted = progress.RecordClear(*stop);
			progress.flags.erase(std::string("bossdown:") + stop->id);  // saved with the clear
			g_runStop.clear();
			if (counted) {
				progress.potionsOwed += PotionsPerClear;  // handed out in town (DeliverPotions)
			}
			if (counted && !Route::Save(file, progress)) {
				context->LogWarn("Route: could not save the hero's progress.");
			}
			// The Den quest's skill point, once per hero: the Den arena is not the quest level (that
			// would offer Akara's free respec), and a dropped Book of Skill can't be used in Act I.
			// Quest rewards given once per difficulty are tripled (docs/flattening.md): 3 points.
			if (counted && Arenas[index].levelId == DenOfEvilLevelId) {
				RunConsole(context, "cheat skillpoints 3");
			}
			// Radament's quest reward, 3 points, once per hero. His Book of Skill only clears the stop:
			// outside the real quest the game won't let the hero read it (2026-11-22).
			if (counted && Arenas[index].levelId == SewersLevelId && progress.Clears(*stop) == 1) {
				RunConsole(context, "cheat skillpoints 3");
			}
			if (counted && progress.Clears(*stop) == 1) {
				FirstClearReward(context, *stop, progress, file);
			}
			for (const auto& e : ClearExperience) {
				if (counted && std::strcmp(e.stop, stop->id) == 0) {
					char command[64] {};
					std::snprintf(command, sizeof(command), "cheat experience %d", e.experience);
					RunConsole(context, command);
				}
			}
			std::snprintf(message, sizeof(message), "Route: %s %s; tier %d of %d is open now.", stop->id,
				counted ? "counts as cleared" : "was not open for this hero, so it doesn't count", progress.CurrentTier() + 1, Route::TierCount);
			context->LogInfo(message);
		Trace(context, message);
		}
	} catch (...) {
		context->LogWarn("Route: recording the clear failed.");
	}
}

// ---- Quest stops: cleared on picking up the quest item ----------------------------------
// Every half second while the hero is in a quest stop, the plugin counts the quest item in
// everything the hero owns. More than on arrival: picked up, cleared.
static std::atomic<int> g_questBaseline { -1 };  // the count on arrival; -1 not counted yet
static std::atomic<int64_t> g_questChestOpenedMs { 0 };  // when the quest chest was seen open; 0 not yet
static constexpr int64_t QuestChestGraceMs = 10000;  // then the stop clears even without a pickup

struct ItemCount { uint32_t code; int count; };

static auto __cdecl CountItem(const D2RL::PluginContext*, const D2RL::Items::ItemInfo* item, void* out) noexcept -> D2RL::Inventory::IterationAction {
	auto* c = static_cast<ItemCount*>(out);
	if (item != nullptr && item->code == c->code) {
		++c->count;
	}
	return D2RL::Inventory::IterationAction::Continue;
}

static void __cdecl QuestItemTick(const D2RL::PluginContext* context, void* run) noexcept;

// Game thread. The quest item is the hero's: the stop clears now if the waves are done, else when
// the last of them dies (OnMonsterDeath).
static void QuestDone(const D2RL::PluginContext* context, void* run) noexcept {
	g_questDone = true;
	if (g_wavesDone.load()) {
		g_threads->runOnUiThread(context, ClearArena, run);
	} else {
		Trace(context, "Quest item taken; the stop clears once every wave is out and dead.");
		context->WriteConsoleMessage("The way back opens once the dead are put to rest.");
		Later(context, 500, QuestItemTick);  // keeps checking for an empty arena (QuestItemCheck)
	}
}

// Game thread.
static void __cdecl QuestItemCheck(const D2RL::PluginContext* context, void* run) noexcept {
	const int index = g_arena.load();
	if (index < 0 || reinterpret_cast<uintptr_t>(run) != g_run.load() || g_portalDropped.load() ||
		(Arenas[index].clearOnItem == nullptr && Arenas[index].clearOnObject < 0)) {
		return;
	}
	const Arena& arena = Arenas[index];
	if (arena.clearOnItem == nullptr) {
		// Cleared by using objects (Cain's gibbet, the Compelling Orb, the barbarians' cages, frozen
		// Anya). Only objects first seen unused count: one already open when it comes into view (Cain
		// freed in an earlier game) does not, and the stop then clears the usual way.
		static std::set<uint32_t> s_seenUnused;
		static uintptr_t s_run = 0;
		if (s_run != reinterpret_cast<uintptr_t>(run)) {
			s_run = reinterpret_cast<uintptr_t>(run);
			s_seenUnused.clear();
		}
		int used = 0;
		try {
			for (const Game::Unit& object : Game::Objects(-1, static_cast<uint32_t>(arena.clearOnObject))) {
				if (object.mode == 0) {
					s_seenUnused.insert(object.id);
				} else if (s_seenUnused.count(object.id) != 0) {
					++used;
				}
			}
		} catch (...) {
		}
		if (used >= arena.clearObjectCount) {
			char message[128] {};
			std::snprintf(message, sizeof(message), "%s: %d of the objects used; cleared.", arena.name, used);
			Trace(context, message);
			g_threads->runOnUiThread(context, ClearArena, run);
			return;
		}
		Later(context, 500, QuestItemTick);
		return;
	}
	if (g_questDone.load()) {
		// The quest item is the hero's and the waves are all out, but the kill count never reached
		// the end (2026-11-22: 10 of the Halls' monsters never died where the hero could find them,
		// likely failed spawns). Clear once no living enemy is left anywhere loaded for 3 s.
		static int s_quietTicks = 0;
		bool anyAlive = true;
		try {
			anyAlive = !Game::Enemies(-1).empty();
		} catch (...) {
		}
		s_quietTicks = (g_wave.load() >= arena.waveCount && !anyAlive) ? s_quietTicks + 1 : 0;
		if (s_quietTicks >= 6) {
			s_quietTicks = 0;
			g_wavesDone = true;
			char message[128] {};
			std::snprintf(message, sizeof(message), "%s: quest item taken and no enemy left nearby; cleared (%d unaccounted for).",
				arena.name, g_remaining.load());
			Trace(context, message);
			g_threads->runOnUiThread(context, ClearArena, run);
			return;
		}
		Later(context, 500, QuestItemTick);
		return;
	}
	D2RL::PlayerHandle player = D2RL::InvalidPlayerHandle;
	ItemCount count { ItemCode(arena.clearOnItem), 0 };
	if (g_inventory != nullptr && g_inventory->getLocalPlayer(context, &player) == D2RL::Inventory::Result::Success) {
		const D2RL::Inventory::ItemFilter filter {
			.structSize    = D2RL::Inventory::ItemFilterSize,
			.containerMask = D2RL::Items::AllItemContainers,
		};
		g_inventory->forEachInventoryItem(context, player, &filter, CountItem, &count);
		const int baseline = g_questBaseline.load();
		char message[128] {};
		if (baseline < 0) {
			g_questBaseline = count.count;
			std::snprintf(message, sizeof(message), "%s: the hero owns %d of the quest item (%s) on arrival.", arena.name, count.count, arena.clearOnItem);
			Trace(context, message);
		} else if (count.count > baseline) {
			std::snprintf(message, sizeof(message), "%s: quest item picked up.", arena.name);
			Trace(context, message);
			QuestDone(context, run);
			return;
		} else if (arena.questChest >= 0) {
			// The game gives a quest item once per hero (and a staff may be in the Horadric
			// Staff by now): after the chest (or altar) is opened, a replay clears without a pickup,
			// at once for a hero who already owns the item, else after a short wait.
			const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
			bool open = false;
			for (const Game::Unit& chest : Game::Objects(-1, static_cast<uint32_t>(arena.questChest))) {
				open = open || chest.mode != 0;  // opening or open
			}
			if (open && g_questChestOpenedMs.load() == 0) {
				g_questChestOpenedMs = now;
			}
			if (open && (baseline > 0 || now - g_questChestOpenedMs.load() >= QuestChestGraceMs)) {
				std::snprintf(message, sizeof(message), "%s: quest chest opened; no pickup needed (the hero had the item or the quest).", arena.name);
				Trace(context, message);
				QuestDone(context, run);
				return;
			}
		}
	}
	Later(context, 500, QuestItemTick);
}

// UI thread (from Later).
static void __cdecl QuestItemTick(const D2RL::PluginContext* context, void* run) noexcept {
	g_threads->runOnGameThread(context, QuestItemCheck, run);
}

// Game thread. Every wave is out, but the kill count may never reach the end: a spawn the game
// refused ("Monster not created") still counts as alive (2026-11-23: Lower Kurast stuck at wave 5
// with 11 of 40 never there). Clear once every boss is dead and no living enemy is left anywhere
// loaded for 5 s. Quest and object stops have their own check (QuestItemCheck).
static void __cdecl EmptyArenaTick(const D2RL::PluginContext* context, void* run) noexcept;
static void __cdecl EmptyArenaCheck(const D2RL::PluginContext* context, void* run) noexcept {
	static int s_quietTicks = 0;
	static uintptr_t s_run = 0;
	const int index = g_arena.load();
	if (index < 0 || reinterpret_cast<uintptr_t>(run) != g_run.load() || g_portalDropped.load()) {
		return;
	}
	if (s_run != reinterpret_cast<uintptr_t>(run)) {
		s_run = reinterpret_cast<uintptr_t>(run);
		s_quietTicks = 0;
	}
	const Arena& arena = Arenas[index];
	bool anyAlive = true;
	try {
		anyAlive = !Game::Enemies(-1).empty();
	} catch (...) {
	}
	s_quietTicks = (g_wave.load() >= arena.waveCount && AllBossesDead() && !anyAlive) ? s_quietTicks + 1 : 0;
	if (s_quietTicks >= 10) {
		s_quietTicks = 0;
		g_wavesDone = true;
		char message[128] {};
		std::snprintf(message, sizeof(message), "%s: every wave out and no enemy left nearby; cleared (%d unaccounted for).",
			arena.name, g_remaining.load());
		Trace(context, message);
		g_threads->runOnUiThread(context, ClearArena, run);
		return;
	}
	Later(context, 500, EmptyArenaTick);
}
static void __cdecl EmptyArenaTick(const D2RL::PluginContext* context, void* run) noexcept {
	g_threads->runOnGameThread(context, EmptyArenaCheck, run);
}
// Starts the check for arenas that clear on kills alone.
static void WatchEmptyArena(const D2RL::PluginContext* context, int index, int delayMs) noexcept {
	const Arena& arena = Arenas[index];
	if (arena.waveCount > 0 && arena.clearOnItem == nullptr && arena.clearOnObject < 0) {
		Later(context, delayMs, EmptyArenaTick);
	}
}

// UI thread. Puts the hero at the arena's arrival tile (maps without a waypoint or stairs).
static void __cdecl PlaceAtArrival(const D2RL::PluginContext* context, void* run) noexcept {
	const int index = g_arena.load();
	if (index < 0 || reinterpret_cast<uintptr_t>(run) != g_run.load() || Arenas[index].arrival.tx < 0) {
		return;
	}
	const Arena& arena = Arenas[index];
	char command[64];
	std::snprintf(command, sizeof(command), "location %d,%d", (arena.originX + arena.arrival.tx) * 5 + 2, (arena.originY + arena.arrival.ty) * 5 + 2);
	RunConsole(context, command);
}

// UI thread. Spawns the next wave and, if more are left, schedules the one after it.
// A boss that didn't spawn ("Monster not created", e.g. a crowded spot) would leave the stop
// uncleared for good. A few seconds after the boss's spawn the plugin looks for it, and sends the
// same command again if it isn't there (up to BossSpawnTries times).
static constexpr int BossCheckMs = 3000;
static constexpr int BossSpawnTries = 3;
struct BossSpawn { std::string command; uint32_t cls = 0; int triesLeft = 0; };
static BossSpawn  g_bossSpawn;
static std::mutex g_bossSpawnLock;

static void __cdecl CheckBossSpawned(const D2RL::PluginContext* context, void* run) noexcept {
	if (reinterpret_cast<uintptr_t>(run) != g_run.load() || g_arena.load() < 0) {
		return;
	}
	BossSpawn boss;
	{
		const std::lock_guard<std::mutex> guard(g_bossSpawnLock);
		boss = g_bossSpawn;
	}
	try {
		for (const Game::MonsterDetails& m : Game::Monsters(-1, false)) {  // dead counts too: already killed
			if (m.unit.row == boss.cls) {
				return;
			}
		}
	} catch (...) {
		return;
	}
	if (boss.triesLeft <= 0) {
		Trace(context, "Boss: not found after every try; the stop clears when everything else is dead.");
		return;
	}
	// Its spot may be full (a crowded wave: "Monster not created"), so each retry spawns it close
	// to the hero instead, a little further round each time; the hero stands on open floor.
	std::string command = boss.command;
	try {
		const Game::Unit hero = Game::Hero();
		if (hero.x > 0 && hero.y > 0) {
			static constexpr int Offsets[][2] = { { 0, 8 }, { 8, 0 }, { -8, 0 }, { 0, -8 } };
			const auto& o = Offsets[static_cast<size_t>(boss.triesLeft) % 4];
			command = command.substr(0, command.find(" @ ")) + " @ " + std::to_string(static_cast<int>(hero.x) + o[0]) + "," +
				std::to_string(static_cast<int>(hero.y) + o[1]);
		}
	} catch (...) {
	}
	Trace(context, "Boss: not found; spawning again near the hero.");
	RunConsole(context, command.c_str());
	{
		const std::lock_guard<std::mutex> guard(g_bossSpawnLock);
		g_bossSpawn.triesLeft = boss.triesLeft - 1;
	}
	Later(context, BossCheckMs, CheckBossSpawned);
}

// Where a group spawns, moved off screen. `wanted` is the spot the wave tables give (tiles inside
// the arena's map), `hero` the hero's sub-tile.
// - Arenas with fixed spots (tested in the game, see "Lessons learned" in CLAUDE.md): the wanted
//   spot if it's off screen, else the off-screen spot (of all the arena's spots) nearest to it,
//   else the one furthest from the screen's middle. Only tested spots, so spawns never land in rock.
// - Arenas that spawn around the hero: the spot is pushed out from the hero, in its own direction
//   or the nearest one that works, to just past the screen's edge, staying within `reach` of the
//   centre. Bosses ("cheat monster") in arenas without a `reach` stay where they were: there the
//   open floor is unknown, and a boss that fails to spawn would leave the stop uncleared.
static Spot OffscreenSpot(const Arena& arena, Spot wanted, const GameMemory::Pos& hero, bool boss, const char* what) {
	const auto reach = [&](double tx, double ty) {
		return ScreenReach((arena.originX + tx) * 5 + 2 - hero.x, (arena.originY + ty) * 5 + 2 - hero.y);
	};
	const double heroTy = (hero.y - 2) / 5.0 - arena.originY;
	const auto isAhead = [&](const Spot& s) { return !arena.ahead || s.ty < heroTy - 1; };
	if (reach(wanted.tx, wanted.ty) >= 1.0 && isAhead(wanted)) {
		return wanted;
	}
	if (!arena.nearHero) {
		Spot best = wanted, furthest = wanted, foremost = wanted;
		double bestDistance = 1e9, furthestReach = isAhead(wanted) ? reach(wanted.tx, wanted.ty) : -1.0;
		for (int i = 0; i < arena.waveCount; ++i) {
			for (int g = 0; g < arena.waves[i].groupCount; ++g) {
				if (boss && std::strcmp(arena.waves[i].groups[g].what, what) != 0) {
					continue;  // a boss only goes where it was tested (Blood Raven failed on a zombie spot)
				}
				const Spot s = arena.waves[i].groups[g].at;
				if (s.ty < foremost.ty) {
					foremost = s;
				}
				if (!isAhead(s)) {
					continue;  // behind the hero
				}
				const double r = reach(s.tx, s.ty);
				const double d = std::hypot(s.tx - wanted.tx, s.ty - wanted.ty);
				if (r >= 1.0 && d < bestDistance) {
					best = s;
					bestDistance = d;
				}
				if (r > furthestReach) {
					furthest = s;
					furthestReach = r;
				}
			}
		}
		if (bestDistance < 1e9) {
			return best;
		}
		return furthestReach >= 0.0 ? furthest : foremost;  // nothing ahead: the spot furthest along
	}
	if (boss && arena.reach == 0) {
		return wanted;
	}
	const double hx = (hero.x - 2) / 5.0 - arena.originX, hy = (hero.y - 2) / 5.0 - arena.originY;
	double angle = std::atan2(wanted.ty - hy, wanted.tx - hx);
	if (wanted.tx == static_cast<int>(std::lround(hx)) && wanted.ty == static_cast<int>(std::lround(hy))) {
		angle = std::atan2(arena.centre.ty - hy, arena.centre.tx - hx) + 3.141592653589793;  // away from the middle
	}
	Spot furthest = wanted;
	double furthestReach = reach(wanted.tx, wanted.ty);
	for (int step = 0; step < 12; ++step) {  // its own direction, then 30 degrees either side, ...
		const double turn = (step + 1) / 2 * 0.5235987755982988 * (step % 2 == 0 ? 1 : -1);
		const double c = std::cos(angle + turn), sn = std::sin(angle + turn);
		for (double distance = 1.0; distance * 5.0 <= MaxSpawnSubtiles; distance += 0.5) {
			const double tx = hx + c * distance, ty = hy + sn * distance;
			if (arena.reach > 0 && std::hypot(tx - arena.centre.tx, ty - arena.centre.ty) > arena.reach) {
				break;  // off the open floor
			}
			const double r = reach(tx, ty);
			const Spot s { static_cast<int>(std::lround(tx)), static_cast<int>(std::lround(ty)) };
			if (r > furthestReach) {
				furthest = s;
				furthestReach = r;
			}
			if (r >= 1.0) {
				return s;
			}
		}
	}
	return furthest;
}

static void __cdecl StartNextWave(const D2RL::PluginContext* context, void* run) noexcept {
	const int index = g_arena.load();
	// No more waves once the stop is cleared (a target kill can clear it before the last wave).
	if (index < 0 || reinterpret_cast<uintptr_t>(run) != g_run.load() || g_portalDropped.load()) {
		return;
	}
	const Arena& arena = Arenas[index];
	const int wave = g_wave.load() + 1;
	if (wave > arena.waveCount) {
		return;
	}
	const Wave& w = arena.waves[wave - 1];
	if (wave == 1 && arena.clearOnRandomUnique) {  // pick the target pack among all the waves'
		int packs = 0;
		for (int i = 0; i < arena.waveCount; ++i) {
			for (int g = 0; g < arena.waves[i].groupCount; ++g) {
				packs += std::strcmp(arena.waves[i].groups[g].command, "mspawn") == 0 ? arena.packCopies : 0;
			}
		}
		static std::mt19937 pick(std::random_device {}());
		g_targetPack = packs > 0 ? std::uniform_int_distribution<int>(0, packs - 1)(pick) : -1;
	}
	int total = 0;
	for (int g = 0; g < w.groupCount; ++g) {
		const bool pack = std::strcmp(w.groups[g].command, "mspawn") == 0;
		total += w.groups[g].count * (pack ? arena.packCopies : 1);
		if (arena.uniquePerPack && pack) {
			total += arena.packCopies;  // the packs' uniques
		}
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
	if (const Route::Stop* stop = Route::StopByArenaLevel(arena.levelId)) {  // a quit resumes after this wave
		Route::Progress progress = LoadProgress();
		if (progress.running == stop->id) {
			progress.NoteWaves(*stop, wave);
			SaveProgress(context, progress);
		}
	}
	WaveOverlay::SetWaves(wave, arena.waveCount, wave < arena.waveCount ? WaveIntervalMs : -1);
	char message[128] {};
	// The crafting hunt: its monsters spawn as the copies for the hero's magic find (craftmf
	// lines). Between two steps each monster is the higher step's copy with a chance that grows
	// with the magic find past the lower step, so the rune chance climbs smoothly on average.
	const CraftStep* step = nullptr;   // the highest step at or under the hero's magic find
	const CraftStep* next = nullptr;   // the step after it
	const GameMemory::Stat mf = GameMemory::HeroMagicFind();
	const int find = mf.ok ? (std::max)(0, mf.value) : 0;
	for (const auto& candidate : g_craftSteps) {
		if (candidate.levelId != arena.levelId) {
			continue;
		}
		if (step == nullptr || find >= candidate.magicFind) {
			step = &candidate;
			next = nullptr;
		} else if (next == nullptr) {
			next = &candidate;
		}
	}
	double upShare = 0.0;  // share of monsters that are the next step's copy
	if (step != nullptr && next != nullptr && next->magicFind > step->magicFind) {
		upShare = static_cast<double>(find - step->magicFind) / (next->magicFind - step->magicFind);
	}
	if (step != nullptr) {
		const double chance = step->runeChance + (next != nullptr ? (next->runeChance - step->runeChance) * upShare : 0.0);
		std::snprintf(message, sizeof(message), "%s: magic find %d%% (%s): monsters %s/%s (%.0f%% the second), rune chance %.1f%%.", arena.name,
			mf.value, mf.ok ? "read" : mf.where.c_str(), step->suffix.c_str(), next != nullptr ? next->suffix.c_str() : "-", upShare * 100.0, chance);
		context->LogInfo(message);
		Trace(context, message);
	}
	static std::mt19937 rng(std::random_device {}());
	const GameMemory::Pos heroNow = GameMemory::Hero();
	ReadScreenAspect();
	for (int g = 0; g < w.groupCount; ++g) {
		const Group& group = w.groups[g];
		const bool bossGroup = (g == 0 && w.bossClass != AnyClass) || std::strcmp(group.command, "mspawn") != 0;
		const bool craft = step != nullptr && std::strncmp(group.what, "dd_craft_", 9) == 0;
		int upper = 0;  // how many of the group are the next step's copy
		if (craft && next != nullptr) {
			upper = std::binomial_distribution<int>(group.count, upShare)(rng);
		}
		const int copies = std::strcmp(group.command, "mspawn") == 0 ? (std::max)(1, arena.packCopies) : 1;
		for (int copy = 0; copy < copies; ++copy) {
		Spot at = group.at;  // copies: the spot turned around the centre by a share of a full circle
		if (copy > 0) {
			const double turn = 6.283185307179586 * copy / copies;
			const double ox = group.at.tx - arena.centre.tx, oy = group.at.ty - arena.centre.ty;
			at.tx = arena.centre.tx + static_cast<int>(std::lround(ox * std::cos(turn) - oy * std::sin(turn)));
			at.ty = arena.centre.ty + static_cast<int>(std::lround(ox * std::sin(turn) + oy * std::cos(turn)));
		}
		int tx = at.tx, ty = at.ty;
		if (arena.nearHero) {
			const GameMemory::Pos hero = GameMemory::Hero();
			if (hero.ok) {
				double hx = (hero.x - 2) / 5.0 - arena.originX + (at.tx - arena.centre.tx);
				double hy = (hero.y - 2) / 5.0 - arena.originY + (at.ty - arena.centre.ty);
				const double dx = hx - arena.centre.tx, dy = hy - arena.centre.ty;
				const double distance = std::sqrt(dx * dx + dy * dy);
				if (arena.reach > 0 && distance > arena.reach) {  // pull it back onto the open floor
					hx = arena.centre.tx + dx * arena.reach / distance;
					hy = arena.centre.ty + dy * arena.reach / distance;
				}
				tx = static_cast<int>(std::lround(hx));
				ty = static_cast<int>(std::lround(hy));
			} else if (arena.reach == 0) {
				context->LogWarn("Spawn skipped: the hero's position can't be read, and this map has no fixed spots.");
				continue;
			} else {
				tx = (at.tx + arena.centre.tx * 2) / 3;  // no hero position: closer in than the spot
				ty = (at.ty + arena.centre.ty * 2) / 3;
			}
		}
		if (heroNow.ok) {
			const Spot off = OffscreenSpot(arena, { tx, ty }, heroNow, bossGroup, group.what);
			tx = off.tx;
			ty = off.ty;
		}
		const int x = (arena.originX + tx) * 5 + 2;
		const int y = (arena.originY + ty) * 5 + 2;
		if (std::strcmp(group.command, "mspawn") == 0 && craft) {
			if (group.count - upper > 0) {
				std::snprintf(message, sizeof(message), "cheat mspawn %s%s,%d @ %d,%d", group.what, step->suffix.c_str(), group.count - upper, x, y);
				RunConsole(context, message);
			}
			if (upper > 0) {
				std::snprintf(message, sizeof(message), "cheat mspawn %s%s,%d @ %d,%d", group.what, next->suffix.c_str(), upper, x, y);
				RunConsole(context, message);
			}
			continue;
		}
		if (std::strcmp(group.command, "mspawn") == 0 && arena.uniquePerPack) {
			std::snprintf(message, sizeof(message), "cheat mspawn %s,%d @ %d,%d", group.what, group.count, x, y);
			RunConsole(context, message);
			// The unique comes alone: its own minions would add kills the wave count doesn't know.
			const bool target = arena.clearOnRandomUnique && g_packsSpawned.fetch_add(1) == g_targetPack.load();
			const bool typed = target && HasTargetTypes(arena.levelId);  // spawned as its "_t" type
			if (target && !typed) {
				std::vector<uint32_t> before;
				for (const Game::MonsterDetails& m : Game::Monsters(-1, false)) {
					if (m.unique) {
						before.push_back(m.unit.id);
					}
				}
				const std::lock_guard<std::mutex> guard(g_uniquesLock);
				g_uniquesBefore = std::move(before);
			}
			RunConsole(context, "cheat nominions 1");
			// The console takes a unique's class number (a type's Id spawns nothing).
			const std::string type = std::string(group.what) + (typed ? "_t" : "");
			const auto cls = g_monsterClasses.find(type);
			if (cls != g_monsterClasses.end()) {
				std::snprintf(message, sizeof(message), "cheat monster unique:%u @ %d,%d", cls->second, x, y);
			} else {
				std::snprintf(message, sizeof(message), "cheat monster unique:%s @ %d,%d", type.c_str(), x, y);
			}
			RunConsole(context, message);
			RunConsole(context, "cheat resetnominions");
			if (typed) {
				Trace(context, "Target unique: spawned as its target type.");
			} else if (target) {
				Later(context, 700, FindTargetUnique);
			}
			continue;
		}
		if (std::strcmp(group.command, "mspawn") == 0) {
			std::snprintf(message, sizeof(message), "cheat mspawn %s,%d @ %d,%d", group.what, group.count, x, y);
		} else if (std::strncmp(group.what, "super:", 6) == 0) {
			// A superunique appears once per game: the second time, spawn its copy's row.
			int row = std::atoi(group.what + 6);
			{
				const std::lock_guard<std::mutex> guard(g_supersLock);
				const auto copy = g_superCopies.find(row);
				if (g_supersSpawned.count(row) != 0 && copy != g_superCopies.end()) {
					row = copy->second;
				}
				g_supersSpawned.insert(row);
			}
			std::snprintf(message, sizeof(message), "cheat monster super:%d @ %d,%d", row, x, y);
		} else {
			std::snprintf(message, sizeof(message), "cheat monster %s @ %d,%d", group.what, x, y);
		}
		RunConsole(context, message);
		if (g == 0 && copy == 0 && w.bossClass != AnyClass) {  // make sure the boss is really there
			const std::lock_guard<std::mutex> guard(g_bossSpawnLock);
			g_bossSpawn = { message, w.bossClass, BossSpawnTries };
			Later(context, BossCheckMs, CheckBossSpawned);
		}
		}
	}
	std::snprintf(message, sizeof(message), "%s: wave %d of %d (+%d, %d alive).", arena.name, wave, arena.waveCount, total, g_remaining.load());
	context->LogInfo(message);
		Trace(context, message);
	if (wave < arena.waveCount) {
		Later(context, WaveIntervalMs, StartNextWave);
	}
	if (wave == arena.waveCount) {
		WatchEmptyArena(context, index, 3000);
	}
}

// Server thread. Count deaths in the running arena's level. The arena is cleared when the last
// wave has spawned, every spawned monster has died and the boss (if any) is dead. Extra monsters
// (raised or revived) only add kills; the boss must still die.
static void __cdecl OnMonsterDeath(const D2RL::PluginContext* context, const D2RL::Lifecycle::MonsterDeathEvent* event, void*) noexcept {
	if (context == nullptr || event == nullptr) {
		return;
	}
	Campfire::NoteDifficulty(context, event->difficulty);
	g_difficulty = event->difficulty;
	{
		char line[96];
		std::snprintf(line, sizeof(line), "death: level %d class %u", event->levelId, event->monster.classId);
		Trace(context, line);
	}
	const int index = g_arena.load();
	if (index >= 0 && event->levelId == Arenas[index].levelId && !g_portalDropped.load() &&
	    ((Arenas[index].clearOnKill != AnyClass && event->monster.classId == Arenas[index].clearOnKill) ||  // e.g. Griswold
	     IsTargetClass(event->levelId, event->monster.classId))) {  // e.g. the Countess, the Pit's target
		g_threads->runOnUiThread(context, ClearArena, reinterpret_cast<void*>(g_run.load()));  // e.g. Griswold
		return;
	}
	if (index < 0 || event->levelId != Arenas[index].levelId || g_wave.load() < 1 || g_portalDropped.load()) {
		return;
	}
	if ((g_targetUnit.load() != 0 && event->monster.id == g_targetUnit.load()) ||  // the random unique
	    IsTargetClass(event->levelId, event->monster.classId)) {
		context->LogInfo("Target unique killed.");
		Trace(context, "Target unique killed.");
		g_threads->runOnUiThread(context, ClearArena, reinterpret_cast<void*>(g_run.load()));
		return;
	}
	const Arena& arena = Arenas[index];
	for (int i = 0; i < g_bossCount.load(); ++i) {
		if (event->monster.classId == g_bosses[i].load()) {
			g_bossesDead |= 1U << i;
		}
	}
	if (g_bossCount.load() > 0 && g_wave.load() >= arena.waveCount && AllBossesDead()) {
		g_threads->runOnUiThread(context, MarkBossDown, nullptr);
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
		Trace(context, message);
	// Crowded maps sometimes refuse a spawn ("Monster not created") and the plugin can't tell, so
	// allow a few missing monsters.
	if (g_wave.load() >= arena.waveCount && left <= MissingSpawnSlack && finalKills >= g_finalSize.load() - MissingSpawnSlack &&
		AllBossesDead()) {
		g_wavesDone = true;
		if (arena.clearOnItem != nullptr && !g_questDone.load()) {
			Trace(context, "Waves done; the stop clears once the quest item is taken.");
			return;
		}
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
static D2RL::Panels::RegistrationHandle g_routeMap = D2RL::Panels::InvalidHandle;   // Act I's page
static D2RL::Panels::RegistrationHandle g_routeMap2 = D2RL::Panels::InvalidHandle;  // Act II's page
static D2RL::Panels::RegistrationHandle g_routeLater[3] = { D2RL::Panels::InvalidHandle, D2RL::Panels::InvalidHandle,
	D2RL::Panels::InvalidHandle };  // Acts III-V (routepages.h)
static std::atomic<int> g_routeAct { 1 };  // the page last opened
static D2RL::Panels::RegistrationHandle g_anyaPanel = D2RL::Panels::InvalidHandle;  // Anya's scroll (routepages.h)

static auto LoadProgress() noexcept -> Route::Progress;

// The page the route map opens on: the act of the hero's first open tier, wherever the hero
// stands (a Town Portal from a borrowed level, e.g. Act IV's red portals or the Cow Level, lands
// in another act's town, and the map must still lead on).
static auto CurrentAct() noexcept -> int {
	const int tier = LoadProgress().CurrentTier();
	return Route::ActOfTier(tier < Route::TierCount ? tier : Route::TierCount - 1);
}

static auto RouteMapPanel(int act) noexcept -> D2RL::Panels::RegistrationHandle {
	if (act >= 3 && act <= 5 && g_routeLater[act - 3] != D2RL::Panels::InvalidHandle) {
		return g_routeLater[act - 3];
	}
	return act == 2 ? g_routeMap2 : g_routeMap;
}

static auto RouteMapName(int act) noexcept -> const char* {
	static constexpr const char* Names[] = { "darkestdiablo/RouteMap", "darkestdiablo/RouteMap2", "darkestdiablo/RouteMap3",
		"darkestdiablo/RouteMap4", "darkestdiablo/RouteMap5" };
	return Names[act >= 1 && act <= 5 ? act - 1 : 0];
}

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
		Trace(context, message);
	if (result == D2RL::Widgets::Result::Success) {
		g_nativeWaypointPanel[0] = '\0';
	}
}

static constexpr int TownLevelId = 1;  // Rogue Encampment
static constexpr int LutGholeinLevelId  = 40;
static constexpr int KurastDocksLevelId = 75;
static constexpr uint32_t MeshifClass = 210;  // monstats row of meshif1 (Lut Gholein's Meshif)
static constexpr int MeshifReach = 12;        // sub-tiles: Meshif next to the hero

// levels.txt Ids of the five towns: Rogue Encampment, Lut Gholein, Kurast Docks,
// Pandemonium Fortress, Harrogath.
static auto InTown() noexcept -> bool {
	switch (g_levelId.load()) {
	case 1: case 40: case 75: case 103: case 109:
		return true;
	default:
		return false;
	}
}

// Act II's page of the route map (RouteMap2hd.json; the same rule: change both).

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

// Saves the hero's route progress; false (and a warning) if it couldn't be written.
static auto SaveProgress(const D2RL::PluginContext* context, const Route::Progress& progress) noexcept -> bool {
	try {
		const auto file = Campfire::RouteProgressFile();
		if (!file.empty() && Route::Save(file, progress)) {
			return true;
		}
	} catch (...) {
	}
	context->LogWarn("Route: could not save the hero's progress.");
	return false;
}

// What a run of the arena has to spawn: its waves, or 1 for a stop without waves (entering it).
static auto ToSpawn(const Arena& arena) noexcept -> int {
	return arena.waveCount > 0 ? arena.waveCount : 1;
}

// A run left by quitting with nothing more to spawn (route.h) ends as given up: the stop counts as
// cleared, with no portal and no reward. Returns true if it did.
static auto GiveUpLeftRun(const D2RL::PluginContext* context) noexcept -> bool {
	Route::Progress progress = LoadProgress();
	const Route::Stop* stop = progress.running.empty() ? nullptr : Route::StopById(progress.running);
	const int arena = stop != nullptr ? FindArena(stop->arenaLevel) : -1;
	// Only fork picks can be given up: a main-line stop (Den, Blood Raven, Tristram) must be done,
	// so it is never locked out; it resumes at its last wave instead.
	if (arena < 0 || !Route::IsFork(stop->tier) || g_runStop == stop->id || progress.runWaves < ToSpawn(Arenas[arena]) ||
	    !progress.RecordClear(*stop)) {
		return false;
	}
	SaveProgress(context, progress);
	char message[160] {};
	std::snprintf(message, sizeof(message), "Route: %s was left by quitting with nothing more to spawn: given up (counts as cleared, no reward).", stop->id);
	context->LogInfo(message);
	Trace(context, message);
	return true;
}

// UI thread: enable the route map's buttons for the hero's progress.
static void __cdecl RefreshRouteMap(const D2RL::PluginContext* context, void*) noexcept {
	if (g_widgets == nullptr) {
		Trace(context, "Route map: no widget service; its buttons keep their layout state.");
		return;
	}
	const int act = g_routeAct.load();
	D2RL::Widgets::WidgetHandle panel = D2RL::Widgets::InvalidHandle;
	const auto foundPanel = g_widgets->findPanel(context, RouteMapName(act), &panel);
	static std::atomic<int> s_retries { 0 };
	if (foundPanel == D2RL::Widgets::Result::Success) {
		s_retries = 0;
	} else {
		// "Unavailable" (3): the panel is still being built after opening. Try again shortly,
		// a few times, instead of leaving every button in its layout state.
		char why[112] {};
		if (foundPanel == D2RL::Widgets::Result::Unavailable && s_retries.fetch_add(1) < 10) {
			std::thread([context] {
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				g_threads->runOnUiThread(context, RefreshRouteMap, nullptr);
			}).detach();
			return;
		}
		s_retries = 0;
		std::snprintf(why, sizeof(why), "Route map: panel not found (code %d); its buttons keep their layout state.", static_cast<int>(foundPanel));
		context->LogWarn(why);
		Trace(context, why);
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
		if (Route::ActOfTier(stop.tier) != act) {
			continue;  // on the other act's page
		}
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
		Trace(context, message);
	} else {
		context->LogWarn(message);
		Trace(context, message);
	}
}

// UI thread. Anya's scroll: the choice of resistance (AnyaScrollhd.json); her reward stays owed
// ("flag anyaowed") until a choice is made, and the panel opens again in town.
static void OpenAnyaScroll(const D2RL::PluginContext* context) noexcept {
	if (g_panels != nullptr && g_anyaPanel != D2RL::Panels::InvalidHandle) {
		g_panels->openPanel(context, g_anyaPanel);
	} else {
		context->LogWarn("Anya's scroll: no panel; the reward stays owed.");
	}
}

static void __cdecl OpenAnyaScrollUi(const D2RL::PluginContext* context, void*) noexcept {
	OpenAnyaScroll(context);
}

// UI thread: the Golden Bird's tripled reward, +40 life on top of the potion's 20.
static void __cdecl GoldenBirdLife(const D2RL::PluginContext* context, void*) noexcept {
	const int life = Game::Stat(Game::Hero(), Game::StatId::MaxLife, 0, false).value_or(0);
	if (life <= 0) {
		return;
	}
	char command[64] {};
	std::snprintf(command, sizeof(command), "cheat stat maxhp,%d", life + 40);
	RunConsole(context, command);
}

static std::atomic<int> g_lifePotions { -1 };  // Potions of Life (xyz) the hero held at the last look

// Game thread, on every level change: rewards that depend on what the hero holds.
// - The Golden Bird: a Potion of Life the hero had and has no more was drunk (once per hero).
// - Hratli's socket (Act III): once the hero holds Khalim's Eye, Brain and Heart, in Kurast Docks.
// - Anya's scroll still owed: its panel opens again in town.
// ---- Akara's ring ---------------------------------------------------------------------------
// After Cain is freed, Akara rewards the hero with a magic ring (the game's quest code). Made rare
// (decided 2026-11-22): while the hero is in the Rogue Encampment with Tristram cleared and the
// swap not yet done, the plugin watches for a magic ring that wasn't there on arrival, and puts a
// rare ring of the same item level in its place. Once per hero ("akararing" in the progress).
struct RingScan { std::vector<std::pair<D2RL::ItemHandle, uint32_t>> magic; };  // handle, item level

static auto __cdecl ScanRing(const D2RL::PluginContext*, const D2RL::Items::ItemInfo* item, void* out) noexcept -> D2RL::Inventory::IterationAction {
	auto* scan = static_cast<RingScan*>(out);
	if (item != nullptr && item->code == ItemCode("rin") && item->quality == D2RL::Items::Quality::Magic) {
		scan->magic.emplace_back(item->handle, item->itemLevel);
	}
	return D2RL::Inventory::IterationAction::Continue;
}

static std::atomic<bool> g_ringWatch { false };
static std::mutex        g_ringLock;
static std::set<D2RL::ItemHandle> g_ringsOnArrival;
static std::atomic<bool> g_ringsPrimed { false };

// Game thread.
static void __cdecl AkaraRingCheck(const D2RL::PluginContext* context, void*) noexcept {
	D2RL::PlayerHandle player = D2RL::InvalidPlayerHandle;
	if (g_inventory == nullptr || g_items == nullptr || g_inventory->getLocalPlayer(context, &player) != D2RL::Inventory::Result::Success) {
		return;
	}
	const D2RL::Inventory::ItemFilter filter {
		.structSize    = D2RL::Inventory::ItemFilterSize,
		.containerMask = D2RL::Items::AllItemContainers,
	};
	RingScan scan;
	g_inventory->forEachInventoryItem(context, player, &filter, ScanRing, &scan);
	const std::lock_guard<std::mutex> guard(g_ringLock);
	if (!g_ringsPrimed.exchange(true)) {
		g_ringsOnArrival.clear();
		for (const auto& [handle, level] : scan.magic) {
			g_ringsOnArrival.insert(handle);
		}
		return;
	}
	for (const auto& [handle, level] : scan.magic) {
		if (g_ringsOnArrival.count(handle) != 0) {
			continue;
		}
		try {
			const auto file = Campfire::RouteProgressFile();
			Route::Progress progress = Route::Load(file);
			if (progress.Flag("akararing") != 0) {
				return;
			}
			progress.flags["akararing"] = 1;
			Route::Save(file, progress);
		} catch (...) {
			return;
		}
		const auto destroyed = g_items->destroyItem(context, player, handle, D2RL::Items::SocketedItemPolicy::RejectIfNotEmpty);
		D2RL::Items::ItemCreateSpec spec {
			.structSize      = D2RL::Items::ItemCreateSpecSize,
			.code            = ItemCode("rin"),
			.quality         = D2RL::Items::Quality::Rare,
			.qualityRecordId = D2RL::Items::RandomQualityRecord,
			.itemLevel       = (std::max)(1U, level),
			.seedMode        = D2RL::Items::SeedMode::Random,
			.quantity        = D2RL::Items::DefaultValue,
			.durability      = D2RL::Items::DefaultValue,
			.destination     = {
				.structSize = D2RL::Items::ItemDestinationSize,
				.container  = D2RL::Items::ItemContainer::Inventory,
				.placement  = D2RL::Items::Placement::Automatic,
			},
		};
		D2RL::ItemHandle ring = D2RL::InvalidItemHandle;
		const auto created = g_items->createItem(context, player, &spec, &ring);
		char message[128] {};
		std::snprintf(message, sizeof(message), "Akara's ring: magic ring removed (code %d), rare ring item level %u made (code %d).",
			static_cast<int>(destroyed), level, static_cast<int>(created));
		context->LogInfo(message);
		Trace(context, message);
		return;
	}
}

// Ticks once a second while the hero stays in the Rogue Encampment.
static void WatchAkaraRing(const D2RL::PluginContext* context, int townLevel) noexcept {
	try {
		const auto file = Campfire::RouteProgressFile();
		if (file.empty()) {
			return;
		}
		const Route::Progress progress = Route::Load(file);
		if (progress.Flag("akararing") != 0 || progress.clears.count("tristram") == 0) {
			return;
		}
	} catch (...) {
		return;
	}
	if (g_ringWatch.exchange(true)) {
		return;
	}
	g_ringsPrimed = false;
	std::thread([context, townLevel] {
		while (g_levelId == townLevel) {
			g_threads->runOnGameThread(context, AkaraRingCheck, nullptr);
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}
		g_ringWatch = false;
	}).detach();
}

static void __cdecl CheckHeldRewards(const D2RL::PluginContext* context, void* periodic) noexcept {
	D2RL::PlayerHandle player = D2RL::InvalidPlayerHandle;
	if (g_inventory == nullptr || g_inventory->getLocalPlayer(context, &player) != D2RL::Inventory::Result::Success) {
		return;
	}
	const D2RL::Inventory::ItemFilter filter {
		.structSize    = D2RL::Inventory::ItemFilterSize,
		.containerMask = D2RL::Items::AllItemContainers,
	};
	const auto count = [&](const char* code) {
		ItemCount c { ItemCode(code), 0 };
		g_inventory->forEachInventoryItem(context, player, &filter, CountItem, &c);
		return c.count;
	};
	try {
		const auto file = Campfire::RouteProgressFile();
		if (file.empty()) {
			return;
		}
		Route::Progress progress = Route::Load(file);
		bool changed = false;
		const int potions = count("xyz");
		if (progress.Flag("goldenbird") == 0 && g_lifePotions.load() > 0 && potions == 0) {
			progress.flags["goldenbird"] = 1;
			changed = true;
			g_threads->runOnUiThread(context, GoldenBirdLife, nullptr);
		}
		g_lifePotions = potions;
		if (g_levelId.load() == 75 && progress.Flag("hratli") == 0 && count("qey") > 0 && count("qbr") > 0 && count("qhr") > 0) {
			progress.flags["hratli"] = 1;
			changed = true;
			GiveItem(context, "zsk", "Hratli's Socket Chisel");
		}
		if (changed) {
			Route::Save(file, progress);
		}
		if (periodic == nullptr && InTown() && progress.Flag("anyaowed") > 0) {  // on arrival only, not every second
			g_threads->runOnUiThread(context, OpenAnyaScrollUi, nullptr);
		}
	} catch (...) {
	}
}

// Held rewards are checked once a second while the hero is in town, not only on level changes:
// a Potion of Life from Alkor drunk on the spot was never seen (the count went from 0 to 1 and
// back to 0 between two level changes), so the Golden Bird's extra life never came (2026-11-23).
static std::atomic<bool> g_heldWatch { false };
static void WatchHeldRewards(const D2RL::PluginContext* context, int townLevel) noexcept {
	if (g_heldWatch.exchange(true)) {
		return;
	}
	std::thread([context, townLevel] {
		while (g_levelId == townLevel) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
			g_threads->runOnGameThread(context, CheckHeldRewards, reinterpret_cast<void*>(1));
		}
		g_heldWatch = false;
	}).detach();
}

// "dd-goldenbird": the Golden Bird's +40 life for a hero who drank the potion before 0.17.22 and
// never got it. Once per hero (the same "goldenbird" mark as the automatic reward).
static auto GoldenBirdCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	try {
		const auto file = Campfire::RouteProgressFile();
		Route::Progress progress = Route::Load(file);
		if (progress.Flag("goldenbird") != 0) {
			command->plugin->WriteConsoleMessage("Golden Bird: this hero already has its extra life.");
			return D2RL::ConsoleCommandResult::Handled;
		}
		progress.flags["goldenbird"] = 1;
		Route::Save(file, progress);
	} catch (...) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	g_threads->runOnUiThread(command->plugin, GoldenBirdLife, nullptr);
	command->plugin->WriteConsoleMessage("Golden Bird: +40 maximum life.");
	return D2RL::ConsoleCommandResult::Handled;
}

// The route map can't show the hero's progress (its layout is fixed once the plugin loads), so
// opening it writes the current tier to the console: what is open, done or in progress, and the
// picks left at a fork (decided 2026-11-23, with the map's redesign).
static auto NodeLabel(const char* node) noexcept -> const char* {
	for (const auto& l : RouteNodeLabels) {
		if (std::strcmp(l.node, node) == 0) {
			return l.label;
		}
	}
	return node;
}

static void RouteNote(const D2RL::PluginContext* context) noexcept {
	try {
		const Route::Progress progress = LoadProgress();
		const int tier = progress.CurrentTier();
		std::string open, done;
		for (const auto& stop : Route::Stops) {
			if (stop.tier != tier) {
				continue;
			}
			const char* label = NodeLabel(stop.node);
			if (progress.running == stop.id) {
				open += std::string(open.empty() ? "" : ", ") + label + " (in progress)";
			} else if (progress.CanEnter(stop)) {
				open += std::string(open.empty() ? "" : ", ") + label;
			}
			if (progress.Clears(stop) > 0) {
				done += std::string(done.empty() ? "" : ", ") + label;
			}
		}
		std::string line = "Route: open now: " + (open.empty() ? std::string("nothing") : open) + ".";
		if (Route::IsFork(tier)) {
			const int left = (std::max)(0, Route::PicksPerFork - progress.TierClears(tier));
			line += " Fork: " + std::to_string(left) + (left == 1 ? " pick left." : " picks left.");
		}
		if (!done.empty()) {
			line += " Done here: " + done + ".";
		}
		context->WriteConsoleMessage(line.c_str());
	} catch (...) {
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
		Trace(context, message);
	}
	if (DevTools::OnUiMessage(context, target, command, text) || Campfire::OnUiMessage(context, target, command, text)) {
		return Action::Consume;
	}
	// Meshif sails east as soon as Duriel is dead (decided 2026-11-23): talking to him in Lut Gholein
	// once Act III is open takes the hero to the Kurast Docks, without the game's own steps (Tyrael
	// in the tomb, Jerhyn in the palace). The talk opens "NpcDialogPanel"; the plugin checks that
	// Meshif is the one at hand and sends the hero on; the level change closes the dialog.
	if (std::strcmp(target, "PanelManager") == 0 && std::strcmp(command, "OpenPanel") == 0 &&
		std::strcmp(text, "NpcDialogPanel") == 0 && g_levelId.load() == LutGholeinLevelId && CurrentAct() >= 3) {
		bool meshif = false;
		try {
			for (const Game::MonsterDetails& m : Game::Monsters(MeshifReach, true)) {
				meshif = meshif || m.unit.row == MeshifClass;
			}
		} catch (...) {
		}
		if (meshif) {
			Trace(context, "Meshif: sailing east to the Kurast Docks.");
			context->WriteConsoleMessage("Meshif: \"To Kurast, then. Hold on to something.\"");
			char line[32] {};
			std::snprintf(line, sizeof(line), "goto %d", KurastDocksLevelId);
			RunConsole(context, line);
		}
	}
	// Every waypoint asks for Blizzard's list ("PanelManager:OpenPanel:WaypointsPanelExpansion").
	// Consume that and open the route map instead, so the route's unlocks can't be skipped
	// (the plugin unlocks every waypoint, see LocalPlayerReady). Outside town the map offers
	// "Return to town", so the player is never stranded.
	static constexpr char WaypointPanelPrefix[] = "WaypointsPanel";
	if (g_panels != nullptr && std::strcmp(target, "PanelManager") == 0 && std::strcmp(command, "OpenPanel") == 0 &&
		std::strncmp(text, WaypointPanelPrefix, sizeof(WaypointPanelPrefix) - 1) == 0) {
		g_routeAct = CurrentAct();
		const auto result = g_panels->openPanel(context, RouteMapPanel(g_routeAct.load()));
		if (result == D2RL::Panels::Result::Success) {
			// Close the game's half-open waypoint panel, queued so it runs after this message.
			std::snprintf(g_nativeWaypointPanel, sizeof(g_nativeWaypointPanel), "%s", text);
			g_threads->runOnUiThread(context, CloseNativeWaypointPanel, nullptr);
			RefreshRouteMap(context, nullptr);
			g_threads->runOnUiThread(context, RefreshRouteMap, nullptr);  // again once its widgets surely exist
			RouteNote(context);
			return Action::Consume;
		}
		std::snprintf(message, sizeof(message), "Route map: open failed (code %d); showing Blizzard's waypoint list.", static_cast<int>(result));
		context->LogWarn(message);
		Trace(context, message);
		return Action::Continue;
	}
	// Node buttons send "PanelManager:OpenPanel:darkestdiablo/travel-<level id>". The game drops
	// button messages with targets or commands it doesn't know, so the travel order rides on a
	// real command with a made-up panel name. Caught here and consumed before the game sees it.
	// Anya's scroll: "PanelManager:OpenPanel:darkestdiablo/anya-<fire|cold|lightning|poison>".
	static constexpr char AnyaPrefix[] = "darkestdiablo/anya-";
	if (std::strcmp(target, "PanelManager") == 0 && std::strcmp(command, "OpenPanel") == 0 &&
		std::strncmp(text, AnyaPrefix, sizeof(AnyaPrefix) - 1) == 0) {
		const std::string choice = text + sizeof(AnyaPrefix) - 1;
		static constexpr struct { const char* key; const char* stat; uint16_t id; } Choices[] = {
			{ "fire", "maxfireresist", Game::StatId::MaxFireResist }, { "cold", "maxcoldresist", Game::StatId::MaxColdResist },
			{ "lightning", "maxlightresist", Game::StatId::MaxLightningResist }, { "poison", "maxpoisonresist", Game::StatId::MaxPoisonResist },
		};
		try {
			const auto file = Campfire::RouteProgressFile();
			Route::Progress progress = file.empty() ? Route::Progress {} : Route::Load(file);
			for (const auto& c : Choices) {
				if (choice == c.key && progress.Flag("anyaowed") > 0) {
					const int now = Game::Stat(Game::Hero(), c.id, 0, false).value_or(0);
					std::snprintf(message, sizeof(message), "cheat stat %s,%d", c.stat, now + 5);
					RunConsole(context, message);
					progress.flags.erase("anyaowed");
					progress.flags["anyascroll"] = 1;
					Route::Save(file, progress);
				}
			}
		} catch (...) {
		}
		if (g_panels != nullptr && g_anyaPanel != D2RL::Panels::InvalidHandle) {
			g_panels->closePanel(context, g_anyaPanel);
		}
		return Action::Consume;
	}
	static constexpr char TravelPrefix[] = "darkestdiablo/travel-";
	if (std::strcmp(target, "PanelManager") == 0 && std::strcmp(command, "OpenPanel") == 0 &&
		std::strncmp(text, TravelPrefix, sizeof(TravelPrefix) - 1) == 0) {
		const int level = std::atoi(text + sizeof(TravelPrefix) - 1);
		const Route::Stop* stop = Route::StopByTravelLevel(level);
		const bool town = level == 1 || level == 40 || level == 75 || level == 103 || level == 109;
		if (level > 0 && !town && (stop == nullptr || !LoadProgress().CanEnter(*stop))) {
			std::snprintf(message, sizeof(message), "Route map: %s is locked for this hero.", stop != nullptr ? stop->id : text);
			context->LogInfo(message);
		Trace(context, message);
			RefreshRouteMap(context, nullptr);
			return Action::Consume;
		}
		if (level > 0) {
			if (g_panels != nullptr) {
				g_panels->closePanel(context, g_routeMap);
				if (g_routeMap2 != D2RL::Panels::InvalidHandle) {
					g_panels->closePanel(context, g_routeMap2);
				}
				for (const auto handle : g_routeLater) {
					if (handle != D2RL::Panels::InvalidHandle) {
						g_panels->closePanel(context, handle);
					}
				}
			}
			// If the earlier close didn't take, close the game's waypoint panel before leaving town.
			g_threads->runOnUiThread(context, CloseNativeWaypointPanel, nullptr);
			g_pickedTravel = level;
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
	DevTools::OnGameplayEvent(context, event);
	using Kind = D2RL::Lifecycle::GameplayEventKind;
	if (event->kind == Kind::LevelChanged) {
		g_levelId = event->currentValue;
		Zone::OnLevelChanged(context, g_levelId);
		{
			const Route::Stop* here = Route::StopByArenaLevel(g_levelId);
			Andariel::OnLevelChanged(context, g_levelId, here != nullptr && LoadProgress().CanEnter(*here));
		}
		char message[96] {};
		std::snprintf(message, sizeof(message), "Level changed: %d -> %d.", event->previousValue, event->currentValue);
		context->LogInfo(message);
		Trace(context, message);
		if (InTown()) {  // the camp's potion supply (DeliverPotions), in every town
			g_threads->runOnGameThread(context, DeliverPotions, nullptr);
		}
		g_threads->runOnGameThread(context, CheckHeldRewards, nullptr);
		if (InTown()) {
			WatchHeldRewards(context, g_levelId.load());
		}
		if (g_levelId == TownLevelId) {  // no healer (Akara; Act II's Fara not yet)
			// WatchTownHealing(context, TownLevelId);  // off: healers heal as normal again (2026-11-23)
			WatchAkaraRing(context, TownLevelId);
		}
		const int arena = FindArena(g_levelId);
		// Back in an arena the hero may not enter now (cleared, or a later tier), e.g. through the
		// Town Portal left open: no waves, so nothing can be farmed (route.h).
		const Route::Stop* stop = Route::StopByArenaLevel(g_levelId);
		if (arena >= 0 && arena != g_arena.load() && stop != nullptr && !LoadProgress().CanEnter(*stop)) {
			ResetArena();
			std::snprintf(message, sizeof(message), "%s: not open for this hero (%s); no waves.", Arenas[arena].name, stop->id);
			context->LogInfo(message);
		Trace(context, message);
			return;
		}
		if (arena >= 0 && arena != g_arena.load() && stop != nullptr && LoadProgress().running != stop->id &&
		    g_pickedTravel.load() != stop->travelLevel) {
			// No run of this stop in progress and the hero didn't pick it on the route map (back in
			// through a portal after clearing it): nothing starts.
			ResetArena();
			std::snprintf(message, sizeof(message), "%s: not picked on the route map; no waves.", Arenas[arena].name);
			context->LogInfo(message);
			Trace(context, message);
			context->WriteConsoleMessage("This stop is done. Pick your next stop on the route map, at a town waypoint.");
			return;
		}
		if (arena >= 0 && arena != g_arena.load()) {
			// The hero's run of this stop (route.h): a new one, or the one in progress.
			Route::Progress progress = LoadProgress();
			const bool sameGame = stop != nullptr && g_runStop == stop->id;
			const int spawned = stop != nullptr ? progress.StartRun(*stop) : 0;
			const bool bossDown = stop != nullptr && progress.Flag(std::string("bossdown:") + stop->id) != 0;
			if (stop != nullptr && (spawned == 0 || !bossDown)) {
				progress.flags.erase(std::string("bossdown:") + stop->id);  // a new run, or a stale mark
			}
			if (stop != nullptr && !sameGame && bossDown && spawned > 0) {
				// A new game, back to a run whose boss already died (left before the rest of its wave
				// did): it clears now, nothing spawns again.
				g_pickedTravel = -1;
				g_runStop = stop->id;
				ResetArena();
				g_arena = arena;
				std::snprintf(message, sizeof(message), "%s: the boss already died in this run; cleared.", Arenas[arena].name);
				context->LogInfo(message);
				Trace(context, message);
				Later(context, ArrivalDelayMs, ClearArena);
				return;
			}
			if (stop != nullptr && !sameGame && spawned >= ToSpawn(Arenas[arena]) && Route::IsFork(stop->tier)) {
				GiveUpLeftRun(context);  // left by quitting with nothing more to come: no farming it
				ResetArena();
				std::snprintf(message, sizeof(message), "%s: given up earlier; no waves.", Arenas[arena].name);
				context->LogInfo(message);
				Trace(context, message);
				return;
			}
			if (stop != nullptr) {
				g_pickedTravel = -1;  // used: coming back in later needs a new pick (or this run)
				if (Arenas[arena].waveCount == 0) {
					progress.NoteWaves(*stop, 1);  // entered: quitting now gives it up
				}
				SaveProgress(context, progress);
				g_runStop = stop->id;
			}
			if (!sameGame) {
				ResetArena();      // a new game: resume after the waves already spawned; a main-line
				g_wave = (std::max)(0, (std::min)(spawned, Arenas[arena].waveCount - 1));  // stop gets its last wave again
			}
			g_arena = arena;       // back in the same game: the counts carry on
			if (Arenas[arena].arrival.tx >= 0) {
				Later(context, ArrivalDelayMs / 2, PlaceAtArrival);
			}
			if (Arenas[arena].clearOnItem != nullptr || Arenas[arena].clearOnObject >= 0) {
				g_questBaseline = -1;
				g_questChestOpenedMs = 0;
				Later(context, ArrivalDelayMs + 500, QuestItemTick);
			}
			if (g_wave.load() < Arenas[arena].waveCount) {
				WaveOverlay::SetWaves(g_wave.load(), Arenas[arena].waveCount, ArrivalDelayMs);
				Later(context, ArrivalDelayMs, StartNextWave);
			} else {
				WatchEmptyArena(context, arena, ArrivalDelayMs + 3000);  // back in after the last wave
			}
			std::snprintf(message, sizeof(message), "%s: run %s at wave %d of %d.", Arenas[arena].name, spawned > 0 ? "resumed" : "started",
				g_wave.load(), Arenas[arena].waveCount);
			context->LogInfo(message);
			Trace(context, message);
		} else if (arena < 0 && g_arena.load() >= 0) {
			++g_run;               // left the arena (town portal, waypoint): stop its wave timer,
			g_arena = -1;          // keep the counts for coming back in this game
			context->LogInfo("Left the arena: waves paused.");
		}
		return;
	}
	if (event->kind == Kind::LocalPlayerReady) {
		WaveOverlay::OnPlayerReady();
		RunConsole(context, "cheat waypoints");
		GiveUpLeftRun(context);  // a run the hero quit with nothing more to spawn
		g_threads->runOnGameThread(context, DeliverPotions, nullptr);  // games start in town
		// WatchTownHealing(context, TownLevelId);  // off: healers heal as normal again (2026-11-23)
		return;
	}
	if (event->kind == Kind::GameLeft) {
		g_levelId = -1;
		g_runStop.clear();
		{
			const std::lock_guard<std::mutex> guard(g_supersLock);
			g_supersSpawned.clear();  // a new game has every superunique again
		}
		Zone::OnGameLeft();
		Andariel::OnGameLeft();
		ResetArena();
		return;
	}
	if (event->kind == Kind::QuestCompleted) {
		char message[128] {};
		std::snprintf(message, sizeof(message), "Quest completed: quest row %u, level %d.", event->questRecordId, g_levelId.load());
		context->LogInfo(message);
		Trace(context, message);
		// Backup: a native quest finishing inside a running arena also gives the scroll, once.
		if (g_arena.load() >= 0 && !g_portalDropped.exchange(true) && g_threads->runOnGameThread(context, DropTownPortal, nullptr) != D2RL::Threads::Result::Success) {
			context->LogWarn("Town Portal: could not queue the drop.");
		}
	}
}

// The linear zone's boss died: the same Town Portal scroll as the arenas (zone.h).
static void GiveZoneTownPortal(const D2RL::PluginContext* context) noexcept {
	if (g_threads->runOnGameThread(context, DropTownPortal, nullptr) != D2RL::Threads::Result::Success) {
		context->LogWarn("Town Portal: could not queue the drop.");
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

static auto WaveHudCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	WaveOverlay::Demo();
	command->plugin->WriteConsoleMessage("Wave counter: a fake 5-wave run starts now (no monsters).");
	return D2RL::ConsoleCommandResult::Handled;
}

static auto MapCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr || g_panels == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	g_routeAct = CurrentAct();
	const auto result = g_panels->togglePanel(command->plugin, RouteMapPanel(g_routeAct.load()));
	if (result != D2RL::Panels::Result::Success) {
		char message[96] {};
		std::snprintf(message, sizeof(message), "Route map: toggle failed (code %d).", static_cast<int>(result));
		command->plugin->WriteConsoleMessage(message);
		return D2RL::ConsoleCommandResult::Failed;
	}
	g_threads->runOnUiThread(command->plugin, RefreshRouteMap, nullptr);
	return D2RL::ConsoleCommandResult::Handled;
}

// The wave counter needs a "/ n" text for each arena wave total (waveoverlay.h).
static auto LoadWaveCounter(const D2RL::PluginContext* context) noexcept -> bool {
	constexpr int count = static_cast<int>(sizeof(Arenas) / sizeof(Arenas[0]));
	int totals[count] {};
	for (int i = 0; i < count; ++i) {
		totals[i] = Arenas[i].waveCount;
	}
	return WaveOverlay::Load(context, totals, count);
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
	// MercGear::Install(context, g_mercType.load());  // off since 0.17.28: with it on, nothing could be
	// picked up (2026-11-23); its byte patterns came from an older game build and may match the wrong spot.

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
		} else {
			// Act II's page: the same kind of panel, its own layout.
			const D2RL::Resources::ResourceRegistration resource2 {
				.structSize = D2RL::Resources::ResourceRegistrationSize,
				.path       = "data/global/ui/layouts/darkestdiablo/RouteMap2hd.json",
				.bytes      = RouteMap2Layout,
				.byteCount  = sizeof(RouteMap2Layout) - 1,
			};
			D2RL::Panels::PanelRegistration registration2 = registration;
			registration2.localId = "RouteMap2";
			D2RL::Resources::RegistrationHandle resourceHandle2 = D2RL::Resources::InvalidHandle;
			if (resources->registerResource(context, &resource2, &resourceHandle2) != D2RL::Resources::Result::Success ||
				g_panels->registerPanel(context, &registration2, &g_routeMap2) != D2RL::Panels::Result::Success) {
				context->LogWarn("Route map: could not register Act II's page.");
				g_routeMap2 = g_routeMap;
			}
			// Acts III-V: the same again (routepages.h, tools/route_pages.py).
			static constexpr struct { const char* path; const char* local; const char* bytes; size_t size; } Later[] = {
				{ "data/global/ui/layouts/darkestdiablo/RouteMap3hd.json", "RouteMap3", RouteMap3Layout, sizeof(RouteMap3Layout) - 1 },
				{ "data/global/ui/layouts/darkestdiablo/RouteMap4hd.json", "RouteMap4", RouteMap4Layout, sizeof(RouteMap4Layout) - 1 },
				{ "data/global/ui/layouts/darkestdiablo/RouteMap5hd.json", "RouteMap5", RouteMap5Layout, sizeof(RouteMap5Layout) - 1 },
			};
			for (int k = 0; k < 3; ++k) {
				const D2RL::Resources::ResourceRegistration resourceK {
					.structSize = D2RL::Resources::ResourceRegistrationSize,
					.path       = Later[k].path,
					.bytes      = Later[k].bytes,
					.byteCount  = Later[k].size,
				};
				D2RL::Panels::PanelRegistration registrationK = registration;
				registrationK.localId = Later[k].local;
				D2RL::Resources::RegistrationHandle resourceHandleK = D2RL::Resources::InvalidHandle;
				if (resources->registerResource(context, &resourceK, &resourceHandleK) != D2RL::Resources::Result::Success ||
					g_panels->registerPanel(context, &registrationK, &g_routeLater[k]) != D2RL::Panels::Result::Success) {
					context->LogWarn((std::string("Route map: could not register ") + Later[k].local + ".").c_str());
					g_routeLater[k] = D2RL::Panels::InvalidHandle;
				}
			}
			const D2RL::Resources::ResourceRegistration anyaResource {
				.structSize = D2RL::Resources::ResourceRegistrationSize,
				.path       = "data/global/ui/layouts/darkestdiablo/AnyaScrollhd.json",
				.bytes      = AnyaScrollLayout,
				.byteCount  = sizeof(AnyaScrollLayout) - 1,
			};
			D2RL::Panels::PanelRegistration anyaRegistration = registration;
			anyaRegistration.localId = "AnyaScroll";
			D2RL::Resources::RegistrationHandle anyaHandle = D2RL::Resources::InvalidHandle;
			if (resources->registerResource(context, &anyaResource, &anyaHandle) != D2RL::Resources::Result::Success ||
				g_panels->registerPanel(context, &anyaRegistration, &g_anyaPanel) != D2RL::Panels::Result::Success) {
				context->LogWarn("Anya's scroll: could not register its panel.");
				g_anyaPanel = D2RL::Panels::InvalidHandle;
			}
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
	// The GM window's event list. A new event needs one line here (or a RegisterEvent call of its
	// own before DevTools::Load); the window and "gm event <id>" pick it up by themselves.
	DevTools::RegisterEvent({ .id = "traveler", .label = "A traveler joins the camp", .console = "dd-traveler" });
	return Campfire::Load(context) && DevTools::Load(context) && Zone::Load(context, GiveZoneTownPortal) && Andariel::Load(context) && LoadWaveCounter(context) &&
		context->RegisterConsoleCommand("dd", InfoCommand, "Show that the Darkest Diablo plugin is running.") &&
		context->RegisterConsoleCommand("dd-tp", TownPortalCommand, "Drop a Town Portal scroll at your feet (test).") &&
		context->RegisterConsoleCommand("dd-waves", WavesCommand, "Restart the waves of the arena you stand in (test).") &&
		context->RegisterConsoleCommand("dd-map", MapCommand, "Open or close the route map.") &&
		context->RegisterConsoleCommand("dd-wavehud", WaveHudCommand, "Show a fake 5-wave run of the wave counter (test).") &&
		context->RegisterConsoleCommand("dd-goldenbird", GoldenBirdCommand, "Give the Golden Bird's +40 life to a hero who missed it (once).");
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {}
