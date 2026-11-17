// Darkest Diablo — the D2RLoader plugin (the mod's code layer).
//
// Current features:
// - Arenas (Den of Evil, Burial Grounds): entering one starts its waves; each wave starts when
//   the last is dead; the last wave is the boss (Corpsefire, Blood Raven). Then a Town Portal
//   scroll goes into the inventory. Spawning uses the "cheat mspawn" / "cheat monster" commands.
// - Console commands (Ctrl + ` with enable_console on in d2rloader/config/d2rloader.toml):
//     dd           show that the plugin is running
//     dd-tp        drop a Town Portal scroll now (for testing)
//     dd-waves     restart the waves of the arena you stand in
// - Waypoints: when the player is ready in a game, the plugin runs D2RLoader's "cheat waypoints"
//   command, so a new character can reach the Den by waypoint on first load.
//   For now this unlocks every waypoint. Per-stop unlocks wait for a waypoint service
//   (D2RLoader/PluginSDK issue #9).

#include <D2RLPlugin/api.h>
#include <D2RLPlugin/core_exports.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>

static constexpr D2RL::PluginInfo DarkestDiabloInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "darkestdiablo",
	.name        = "Darkest Diablo",
	.version     = "0.5.0",
	.author      = "Darkest Diablo team",
	.description = "Roguelite rules for the Darkest Diablo mod.",
	.flags       = D2RL::PluginFlags::Shared,
};

static constexpr int32_t DenOfEvilLevelId = 8;  // levels.txt Id of "Den of Evil"

static const D2RL::InventoryService* g_inventory = nullptr;
static const D2RL::ItemService*      g_items     = nullptr;
static const D2RL::ThreadService*    g_threads   = nullptr;
static int32_t                       g_levelId   = -1;  // the level the local player is in

// Runs on the game thread: create one Town Portal scroll ("tsc") on the ground by the player.
// If the ground placement fails, put it in the inventory instead.
static void __cdecl DropTownPortal(const D2RL::PluginContext* context, void*) noexcept {
	D2RL::PlayerHandle player = D2RL::InvalidPlayerHandle;
	if (g_inventory->getLocalPlayer(context, &player) != D2RL::Inventory::Result::Success) {
		context->LogWarn("Town Portal: could not find the player.");
		return;
	}
	D2RL::Items::ItemCreateSpec spec {
		.structSize      = D2RL::Items::ItemCreateSpecSize,
		.code            = D2RL::Items::MakeItemCode("tsc"),
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
		context->LogInfo("Town Portal: put a scroll in the inventory.");
		return;
	}
	spec.destination.container = D2RL::Items::ItemContainer::Ground;
	const auto fallback = g_items->createItem(context, player, &spec, &item);
	char message[160] {};
	std::snprintf(message, sizeof(message), "Town Portal: inventory failed (code %d); ground drop %s (code %d).",
		static_cast<int>(result), fallback == D2RL::Items::Result::Success ? "worked" : "failed too", static_cast<int>(fallback));
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
// Each arena stop is one room. Entering it starts its waves; each wave starts when the last is
// dead; the last wave is the boss. When the boss wave is dead, a Town Portal scroll is given.
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
	uint32_t onlyClass;               // count only deaths of this monstats row (0xFFFFFFFF: any)
};
struct Arena {
	const char* name;
	int32_t     levelId;
	int         originX, originY;     // levels.txt OffsetX/Y: the map's world tile position
	const Wave* waves;
	int         waveCount;
};

static constexpr uint32_t AnyClass       = 0xFFFFFFFFU;
static constexpr uint32_t ZombieClass    = 5;    // monstats row of zombie1 (Corpsefire is one)
static constexpr uint32_t BloodRavenClass = 267; // monstats row of bloodraven

// Den of Evil (level 8): waypoint at tile 9,5. Fallen east, zombies south-west, shamans south-east.
static constexpr Wave DenWaves[] = {
	{ { { "mspawn", "fallen1", 7, { 19, 13 } }, { "mspawn", "zombie1", 3, { 5, 17 } } }, 2, AnyClass },
	{ { { "mspawn", "fallen1", 5, { 19, 13 } }, { "mspawn", "zombie1", 4, { 5, 17 } }, { "mspawn", "fallenshaman1", 1, { 16, 18 } } }, 3, AnyClass },
	{ { { "mspawn", "fallen1", 4, { 19, 13 } }, { "mspawn", "zombie1", 4, { 5, 17 } }, { "mspawn", "fallenshaman1", 2, { 16, 18 } } }, 3, AnyClass },
	{ { { "mspawn", "fallen1", 4, { 19, 13 } }, { "mspawn", "zombie1", 3, { 5, 17 } }, { "mspawn", "fallenshaman1", 3, { 16, 18 } } }, 3, AnyClass },
	{ { { "mspawn", "fallen1", 3, { 19, 13 } }, { "mspawn", "zombie1", 4, { 5, 17 } }, { "mspawn", "fallenshaman1", 3, { 16, 18 } } }, 3, AnyClass },
	// Corpsefire + 5 minions, all zombie1. Shamans revive fallen, so count only zombies here.
	{ { { "monster", "super:40", 6, { 12, 20 } } }, 1, ZombieClass },
};

// Burial Grounds (level 17): waypoint at tile 5,1 (north). Waves come from the open south strip.
static constexpr Wave BurialWaves[] = {
	{ { { "mspawn", "zombie1", 6, { 12, 30 } } }, 1, AnyClass },
	{ { { "mspawn", "zombie1", 4, { 4, 30 } }, { "mspawn", "skeleton1", 3, { 20, 30 } } }, 2, AnyClass },
	{ { { "mspawn", "zombie1", 3, { 20, 30 } }, { "mspawn", "skeleton1", 5, { 4, 30 } } }, 2, AnyClass },
	// Blood Raven and 4 zombies; the wave ends when she dies.
	{ { { "mspawn", "bloodraven", 1, { 12, 30 } }, { "mspawn", "zombie1", 4, { 12, 29 } } }, 2, BloodRavenClass },
};

static constexpr Arena Arenas[] = {
	{ "Den of Evil",    DenOfEvilLevelId, 1500, 1000, DenWaves,    static_cast<int>(sizeof(DenWaves) / sizeof(DenWaves[0])) },
	{ "Burial Grounds", 17,               1550, 1000, BurialWaves, static_cast<int>(sizeof(BurialWaves) / sizeof(BurialWaves[0])) },
};

static std::atomic<int>  g_arena { -1 };      // index into Arenas, -1 = none running
static std::atomic<int>  g_wave { 0 };        // 1-based wave in progress; > waveCount = done
static std::atomic<int>  g_remaining { 0 };   // monsters left in the current wave
static std::atomic<bool> g_portalDropped { false };

static auto FindArena(int32_t levelId) noexcept -> int {
	for (int i = 0; i < static_cast<int>(sizeof(Arenas) / sizeof(Arenas[0])); ++i) {
		if (Arenas[i].levelId == levelId) {
			return i;
		}
	}
	return -1;
}

static void ResetArena() noexcept {
	g_arena = -1;
	g_wave = 0;
	g_remaining = 0;
	g_portalDropped = false;
}

// UI thread.
static void __cdecl StartNextWave(const D2RL::PluginContext* context, void*) noexcept {
	const int index = g_arena.load();
	if (index < 0) {
		return;
	}
	const Arena& arena = Arenas[index];
	const int wave = g_wave.load() + 1;
	char message[128] {};
	if (wave > arena.waveCount) {
		g_wave = wave;
		if (!g_portalDropped.exchange(true) && g_threads->runOnGameThread(context, DropTownPortal, nullptr) != D2RL::Threads::Result::Success) {
			context->LogWarn("Town Portal: could not queue the drop.");
		}
		std::snprintf(message, sizeof(message), "%s: cleared.", arena.name);
		context->LogInfo(message);
		return;
	}
	const Wave& w = arena.waves[wave - 1];
	int total = w.groups[0].count;  // with onlyClass set, only the first group (the boss) counts
	if (w.onlyClass == AnyClass) {
		for (int g = 1; g < w.groupCount; ++g) {
			total += w.groups[g].count;
		}
	}
	g_remaining = total;
	g_wave = wave;
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
	std::snprintf(message, sizeof(message), "%s: wave %d of %d (%d to kill).", arena.name, wave, arena.waveCount, total);
	context->LogInfo(message);
}

// Server thread. Count deaths in the running arena's level; start the next wave on the UI thread.
static void __cdecl OnMonsterDeath(const D2RL::PluginContext* context, const D2RL::Lifecycle::MonsterDeathEvent* event, void*) noexcept {
	if (context == nullptr || event == nullptr) {
		return;
	}
	const int index = g_arena.load();
	if (index < 0 || event->levelId != Arenas[index].levelId) {
		return;
	}
	const Arena& arena = Arenas[index];
	const int wave = g_wave.load();
	if (wave < 1 || wave > arena.waveCount) {
		return;
	}
	const uint32_t only = arena.waves[wave - 1].onlyClass;
	if (only != AnyClass && event->monster.classId != only) {
		return;
	}
	char message[96] {};
	std::snprintf(message, sizeof(message), "%s: kill (class %u), %d left in wave %d.", arena.name, event->monster.classId, g_remaining.load() - 1, wave);
	context->LogInfo(message);
	if (g_remaining.fetch_sub(1) == 1) {
		g_threads->runOnUiThread(context, StartNextWave, nullptr);
	}
}

static void __cdecl OnGameplayEvent(const D2RL::PluginContext* context, const D2RL::Lifecycle::GameplayEvent* event, void*) noexcept {
	if (context == nullptr || !D2RL::Lifecycle::HasGameplayEventField(event, D2RL::Lifecycle::GameplayEventRequiredSize)) {
		return;
	}
	using Kind = D2RL::Lifecycle::GameplayEventKind;
	if (event->kind == Kind::LevelChanged) {
		g_levelId = event->currentValue;
		char message[96] {};
		std::snprintf(message, sizeof(message), "Level changed: %d -> %d.", event->previousValue, event->currentValue);
		context->LogInfo(message);
		const int arena = FindArena(g_levelId);
		if (arena >= 0 && arena != g_arena.load()) {
			ResetArena();          // a new arena, or the same one after leaving: start from wave 1
			g_arena = arena;
			StartNextWave(context, nullptr);  // gameplay events already run on the UI thread
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
	command->plugin->WriteConsoleMessage("Darkest Diablo plugin 0.5.0 is running.");
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
	StartNextWave(command->plugin, nullptr);
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
	for (const auto kind : { Kind::LocalPlayerReady, Kind::LevelChanged, Kind::GameLeft, Kind::QuestCompleted }) {
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

	context->LogInfo("Darkest Diablo plugin 0.5.0 loaded.");
	return context->RegisterConsoleCommand("dd", InfoCommand, "Show that the Darkest Diablo plugin is running.") &&
		context->RegisterConsoleCommand("dd-tp", TownPortalCommand, "Drop a Town Portal scroll at your feet (test).") &&
		context->RegisterConsoleCommand("dd-waves", WavesCommand, "Restart the waves of the arena you stand in (test).");
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {}
