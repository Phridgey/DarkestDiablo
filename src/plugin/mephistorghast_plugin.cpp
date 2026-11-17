// Mephistorghast — the D2RLoader plugin (the mod's code layer).
//
// Current features:
// - Den of Evil: when the Den quest completes (the last monster dies) while the player is in
//   the Den, a Town Portal scroll drops at the player's feet so they can go back to town.
// - Console commands (Ctrl + ` with enable_console on in d2rloader/config/d2rloader.toml):
//     mephisto     show that the plugin is running
//     mephisto-tp  drop a Town Portal scroll now (for testing)
// - Waypoints: when the player is ready in a game, the plugin runs D2RLoader's "waypoints"
//   command, so a new character can reach the Den by waypoint on first load.
//   For now this unlocks every waypoint. Per-stop unlocks wait for a waypoint service
//   (D2RLoader/PluginSDK issue #9).

#include <D2RLPlugin/api.h>
#include <D2RLPlugin/core_exports.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

static constexpr D2RL::PluginInfo MephistorghastInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "mephistorghast",
	.name        = "Mephistorghast",
	.version     = "0.3.0",
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

// Runs D2RLoader's "waypoints" console command through D2RCore's ExecuteConsoleCommand export.
// Gameplay listeners already run on the UI thread, which is where this call must happen.
static void UnlockWaypoints(const D2RL::PluginContext* context) noexcept {
	namespace CE = D2RL::CoreExports;
	const HMODULE core = GetModuleHandleA(CE::CoreDllName);
	const auto execute = core == nullptr ? nullptr :
		reinterpret_cast<CE::ExecuteConsoleCommandFn>(GetProcAddress(core, CE::ExecuteConsoleCommandInfo.name));
	if (execute == nullptr) {
		context->LogWarn("Waypoints: ExecuteConsoleCommand not found in D2RCore.");
		return;
	}
	const bool ok = execute("waypoints");
	context->LogInfo(ok ? "Waypoints: unlocked." : "Waypoints: the command did not run.");
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
		UnlockWaypoints(context);
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
		if (g_levelId == DenOfEvilLevelId && g_threads->runOnGameThread(context, DropTownPortal, nullptr) != D2RL::Threads::Result::Success) {
			context->LogWarn("Town Portal: could not queue the drop.");
		}
	}
}

static auto MephistoCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	command->plugin->WriteConsoleMessage("Mephistorghast plugin 0.3.0 is running.");
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

	context->LogInfo("Mephistorghast plugin 0.3.0 loaded.");
	return context->RegisterConsoleCommand("mephisto", MephistoCommand, "Show that the Mephistorghast plugin is running.") &&
		context->RegisterConsoleCommand("mephisto-tp", TownPortalCommand, "Drop a Town Portal scroll at your feet (test).");
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {}
