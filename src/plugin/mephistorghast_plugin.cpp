// Mephistorghast — the D2RLoader plugin (the mod's code layer).
//
// Current features:
// - Den of Evil: when the Den quest completes (the last monster dies) while the player is in
//   the Den, a Town Portal scroll drops at the player's feet so they can go back to town.
// - Den of Evil arena: entering the Den starts 5 waves of 10 (fallen, zombies,
//   fallen shamans), then Corpsefire and his
//   pack. Each wave starts when the last one is dead. When Corpsefire's wave is dead, a Town
//   Portal scroll drops. Spawning uses the "cheat mspawn" / "cheat monster" console commands.
// - Console commands (Ctrl + ` with enable_console on in d2rloader/config/d2rloader.toml):
//     mephisto     show that the plugin is running
//     mephisto-tp  drop a Town Portal scroll now (for testing)
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

static constexpr D2RL::PluginInfo MephistorghastInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "mephistorghast",
	.name        = "Mephistorghast",
	.version     = "0.4.0",
	.author      = "Mephistorghast team",
	.description = "Roguelite rules for the Mephistorghast mod.",
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
			.container  = D2RL::Items::ItemContainer::Ground,
			.placement  = D2RL::Items::Placement::Automatic,
		},
	};
	D2RL::ItemHandle item = D2RL::InvalidItemHandle;
	auto result = g_items->createItem(context, player, &spec, &item);
	if (result == D2RL::Items::Result::Success) {
		context->LogInfo("Town Portal: dropped a scroll at the player's feet.");
		return;
	}
	spec.destination.container = D2RL::Items::ItemContainer::Inventory;
	const auto fallback = g_items->createItem(context, player, &spec, &item);
	char message[160] {};
	std::snprintf(message, sizeof(message), "Town Portal: ground drop failed (code %d); inventory %s (code %d).",
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

// ---- Den of Evil arena -------------------------------------------------------------------
static constexpr int      ZombieWaves      = 5;
// Each wave's mix: fallen, zombies, fallen shamans (10 per wave). Corpsefire's wave is all zombies.
struct WaveMix { int fallen, zombies, shamans; };
static constexpr WaveMix Waves[ZombieWaves] = {
	{ 7, 3, 0 },
	{ 5, 4, 1 },
	{ 4, 4, 2 },
	{ 4, 3, 3 },
	{ 3, 4, 3 },
};
static constexpr int      CorpsefireWave   = ZombieWaves + 1;
static constexpr int      CorpsefireCount  = 6;   // Corpsefire + 5 minions (superuniques.txt)

static std::atomic<int>  g_wave { 0 };        // 0 = not started, 1..5 zombies, 6 = Corpsefire, 7 = done
static std::atomic<int>  g_remaining { 0 };   // monsters left in the current wave
static std::atomic<bool> g_portalDropped { false };

static void __cdecl StartNextWave(const D2RL::PluginContext* context, void*) noexcept {
	const int wave = g_wave.load() + 1;
	char message[96] {};
	if (wave <= ZombieWaves) {
		const WaveMix& mix = Waves[wave - 1];
		g_remaining = mix.fallen + mix.zombies + mix.shamans;
		g_wave = wave;
		const struct { const char* id; int count; } groups[] = {
			{ "fallen1", mix.fallen }, { "zombie1", mix.zombies }, { "fallenshaman1", mix.shamans },
		};
		for (const auto& group : groups) {
			if (group.count > 0) {
				std::snprintf(message, sizeof(message), "cheat mspawn %s,%d", group.id, group.count);
				RunConsole(context, message);
			}
		}
	} else if (wave == CorpsefireWave) {
		g_remaining = CorpsefireCount;
		g_wave = wave;
		RunConsole(context, "cheat monster super:40");
	} else {
		g_wave = CorpsefireWave + 1;
		if (!g_portalDropped.exchange(true) && g_threads->runOnGameThread(context, DropTownPortal, nullptr) != D2RL::Threads::Result::Success) {
			context->LogWarn("Town Portal: could not queue the drop.");
		}
		return;
	}
	std::snprintf(message, sizeof(message), "Den arena: wave %d of %d.", wave, CorpsefireWave);
	context->LogInfo(message);
}

// Server thread. Count deaths of wave monsters in the Den; start the next wave on the UI thread.
static void __cdecl OnMonsterDeath(const D2RL::PluginContext* context, const D2RL::Lifecycle::MonsterDeathEvent* event, void*) noexcept {
	if (context == nullptr || event == nullptr || event->levelId != DenOfEvilLevelId) {
		return;
	}
	const int wave = g_wave.load();
	if (wave < 1 || wave > CorpsefireWave) {
		return;
	}
	char message[96] {};
	std::snprintf(message, sizeof(message), "Den arena: kill (class %u), %d left in wave %d.", event->monster.classId, g_remaining.load() - 1, wave);
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
		return;
	}
	if (event->kind == Kind::LocalPlayerReady) {
		RunConsole(context, "cheat waypoints");
		return;
	}
	if (event->kind == Kind::GameLeft) {
		g_levelId = -1;
		return;
	}
	if (event->kind == Kind::QuestCompleted) {
		char message[128] {};
		std::snprintf(message, sizeof(message), "Quest completed: quest row %u, level %d.", event->questRecordId, g_levelId);
		context->LogInfo(message);
		// Backup: the native Den quest finishing also drops the scroll, once.
		if (g_levelId == DenOfEvilLevelId && !g_portalDropped.exchange(true) && g_threads->runOnGameThread(context, DropTownPortal, nullptr) != D2RL::Threads::Result::Success) {
			context->LogWarn("Town Portal: could not queue the drop.");
		}
	}
}

static auto MephistoCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	command->plugin->WriteConsoleMessage("Mephistorghast plugin 0.4.0 is running.");
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

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &MephistorghastInfo;
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
		context->LogError("Mephistorghast: a required D2RLoader service is missing.");
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
			context->LogError("Mephistorghast: could not listen for game events.");
			return false;
		}
	}

	const D2RL::Lifecycle::MonsterDeathListener deathListener {
		.structSize = D2RL::Lifecycle::MonsterDeathListenerSize,
		.callback   = OnMonsterDeath,
	};
	D2RL::Lifecycle::ListenerHandle deathHandle = D2RL::Lifecycle::InvalidHandle;
	if (lifecycle->registerMonsterDeathListener(context, &deathListener, &deathHandle) != D2RL::Lifecycle::Result::Success) {
		context->LogError("Mephistorghast: could not listen for monster deaths.");
		return false;
	}

	context->LogInfo("Mephistorghast plugin 0.4.0 loaded.");
	return context->RegisterConsoleCommand("mephisto", MephistoCommand, "Show that the Mephistorghast plugin is running.") &&
		context->RegisterConsoleCommand("mephisto-tp", TownPortalCommand, "Drop a Town Portal scroll at your feet (test).");
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {}
