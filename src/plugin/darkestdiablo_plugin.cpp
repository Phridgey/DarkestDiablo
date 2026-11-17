// Darkest Diablo — the D2RLoader plugin (the mod's code layer).
//
// Current features:
// - Arenas (Den of Evil, Burial Grounds): entering one starts its waves, one every 10 s; the
//   last wave has the boss (Corpsefire, Blood Raven). When everything is dead, a Town Portal
//   scroll goes into the inventory. Spawning uses the "cheat mspawn" / "cheat monster" commands.
// - Console commands (Ctrl + ` with enable_console on in d2rloader/config/d2rloader.toml):
//     dd           show that the plugin is running
//     dd-tp        drop a Town Portal scroll now (for testing)
//     dd-waves     restart the waves of the arena you stand in
//     dd-map       open or close the route map
// - Waypoints: when the player is ready in a game, the plugin runs D2RLoader's "cheat waypoints"
//   command, so a new character can reach the Den by waypoint on first load.
//   For now this unlocks every waypoint. Per-stop unlocks wait for a waypoint service
//   (D2RLoader/PluginSDK issue #9).
// - The Campfire (campfire.cpp): save slots, the hero roster, recruiting, permanent death.

#include <D2RLPlugin/api.h>
#include <D2RLPlugin/core_exports.h>
#include "campfire.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static constexpr D2RL::PluginInfo DarkestDiabloInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "darkestdiablo",
	.name        = "Darkest Diablo",
	.version     = "0.8.8",
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
static constexpr uint32_t ZombieClass    = 5;    // monstats row of zombie1 (Corpsefire is one)
static constexpr uint32_t BloodRavenClass = 267; // monstats row of bloodraven

// Den of Evil (level 8): waypoint at tile 9,5. Fallen east, zombies south-west, shamans south-east.
static constexpr Wave DenWaves[] = {
	{ { { "mspawn", "fallen1", 7, { 19, 13 } }, { "mspawn", "zombie1", 3, { 5, 17 } } }, 2, AnyClass },
	{ { { "mspawn", "fallen1", 5, { 19, 13 } }, { "mspawn", "zombie1", 4, { 5, 17 } }, { "mspawn", "fallenshaman1", 1, { 16, 18 } } }, 3, AnyClass },
	{ { { "mspawn", "fallen1", 4, { 19, 13 } }, { "mspawn", "zombie1", 4, { 5, 17 } }, { "mspawn", "fallenshaman1", 2, { 16, 18 } } }, 3, AnyClass },
	{ { { "mspawn", "fallen1", 4, { 19, 13 } }, { "mspawn", "zombie1", 3, { 5, 17 } }, { "mspawn", "fallenshaman1", 3, { 16, 18 } } }, 3, AnyClass },
	{ { { "mspawn", "fallen1", 3, { 19, 13 } }, { "mspawn", "zombie1", 4, { 5, 17 } }, { "mspawn", "fallenshaman1", 3, { 16, 18 } } }, 3, AnyClass },
	// Corpsefire + 5 minions.
	{ { { "monster", "super:40", 6, { 12, 20 } } }, 1, AnyClass },
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

static constexpr Arena Arenas[] = {
	{ "Den of Evil",    DenOfEvilLevelId, 1500, 1000, DenWaves,    static_cast<int>(sizeof(DenWaves) / sizeof(DenWaves[0])) },
	{ "Burial Grounds", 9,                1500, 1300, BurialWaves, static_cast<int>(sizeof(BurialWaves) / sizeof(BurialWaves[0])) },
};

static constexpr int     ArrivalDelayMs = 1000;   // wave 1: right on arrival the map isn't loaded yet
static constexpr int     WaveIntervalMs = 10000;  // each later wave: 10 s after the previous one spawned
static std::atomic<int>      g_arena { -1 };      // index into Arenas, -1 = none running
static std::atomic<int>      g_wave { 0 };        // waves spawned so far
static std::atomic<int>      g_remaining { 0 };   // spawned monsters not yet dead (all waves)
static std::atomic<uint32_t> g_boss { AnyClass }; // the boss to wait for, if a spawned wave had one
static std::atomic<bool>     g_bossDead { true };
static std::atomic<bool>     g_portalDropped { false };
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
	g_boss = AnyClass;
	g_bossDead = true;
	g_portalDropped = false;
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
	char message[96] {};
	std::snprintf(message, sizeof(message), "%s: cleared.", Arenas[index].name);
	context->LogInfo(message);
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
		g_boss = w.bossClass;
		g_bossDead = false;
	}
	g_remaining += total;
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
	if (g_boss.load() != AnyClass && event->monster.classId == g_boss.load()) {
		g_bossDead = true;
	}
	const int left = g_remaining.fetch_sub(1) - 1;
	char message[96] {};
	std::snprintf(message, sizeof(message), "%s: kill (class %u), %d alive.", arena.name, event->monster.classId, left);
	context->LogInfo(message);
	if (g_wave.load() >= arena.waveCount && left <= 0 && g_bossDead.load()) {
		g_threads->runOnUiThread(context, ClearArena, reinterpret_cast<void*>(g_run.load()));
	}
}

// ---- Route map (the waypoint "map") ------------------------------------------------------
// A plugin panel listing the act's stops as in the design sketch. A node button sends a UI
// message (see OnUiMessage); the plugin closes the panel and sends the player there with the
// console's goto. The layout also ships as a mod file (src/data/.../RouteMaphd.json), which
// overrides this built-in copy, so layout changes only need a game restart. For now every UI message is also logged, to find the one the
// town waypoint sends, so the map can replace Blizzard's list.
static const D2RL::PanelService*     g_panels = nullptr;
static D2RL::Panels::RegistrationHandle g_routeMap = D2RL::Panels::InvalidHandle;

static constexpr char RouteMapLayout[] = R"json({
 "type": "Panel",
 "name": "darkestdiablo/RouteMap",
 "fields": {
  "rect": "$LeftPanelRect",
  "anchor": "$LeftPanelAnchor",
  "priority": 8500
 },
 "children": [
  {
   "type": "ImageWidget",
   "name": "Background",
   "fields": {
    "rect": {
     "x": 0,
     "y": 0
    },
    "filename": "PANEL\\Waypoints\\Waypoints_Base",
    "frame": 0
   }
  },
  {
   "type": "ImageWidget",
   "name": "LeftHinge",
   "fields": {
    "rect": "$LeftHingeRect",
    "filename": "$LeftHingeSprite"
   }
  },
  {
   "type": "TextBoxWidget",
   "name": "Title",
   "fields": {
    "rect": "$InGamePanelTitleRect",
    "text": "Act I - Choose your path",
    "style": "$StyleTitleBlock"
   }
  },
  {
   "type": "ButtonWidget",
   "name": "NodeDen",
   "fields": {
    "rect": {
     "x": 290,
     "y": 250
    },
    "filename": "Panel\\Modals\\ModalButton",
    "focusIndicatorFilename": "Controller/HoverImages/ModalButton_Hover",
    "pressedFrame": 1,
    "disabledFrame": 2,
    "hoveredFrame": 3,
    "textString": "Den of Evil",
    "pointSize": "$MediumFontSize",
    "textColor": "$FontColorWhite",
    "onClickMessage": "PanelManager:OpenPanel:darkestdiablo/travel-8"
   }
  },
  {
   "type": "ButtonWidget",
   "name": "NodeBurial",
   "fields": {
    "rect": {
     "x": 290,
     "y": 360
    },
    "filename": "Panel\\Modals\\ModalButton",
    "focusIndicatorFilename": "Controller/HoverImages/ModalButton_Hover",
    "pressedFrame": 1,
    "disabledFrame": 2,
    "hoveredFrame": 3,
    "textString": "Burial Grounds",
    "pointSize": "$MediumFontSize",
    "textColor": "$FontColorWhite",
    "onClickMessage": "PanelManager:OpenPanel:darkestdiablo/travel-10"
   }
  },
  {
   "type": "ButtonWidget",
   "name": "Fork1",
   "fields": {
    "rect": {
     "x": 290,
     "y": 470
    },
    "filename": "Panel\\Modals\\ModalButton",
    "focusIndicatorFilename": "Controller/HoverImages/ModalButton_Hover",
    "pressedFrame": 1,
    "disabledFrame": 2,
    "hoveredFrame": 3,
    "textString": "Raven / Mausoleum / Crypt",
    "pointSize": "$MediumFontSize",
    "textColor": "$FontColorLightGray",
    "disabled": true
   }
  },
  {
   "type": "ButtonWidget",
   "name": "NodeTristram",
   "fields": {
    "rect": {
     "x": 290,
     "y": 580
    },
    "filename": "Panel\\Modals\\ModalButton",
    "focusIndicatorFilename": "Controller/HoverImages/ModalButton_Hover",
    "pressedFrame": 1,
    "disabledFrame": 2,
    "hoveredFrame": 3,
    "textString": "Tristram",
    "pointSize": "$MediumFontSize",
    "textColor": "$FontColorLightGray",
    "disabled": true
   }
  },
  {
   "type": "ButtonWidget",
   "name": "Fork2",
   "fields": {
    "rect": {
     "x": 290,
     "y": 690
    },
    "filename": "Panel\\Modals\\ModalButton",
    "focusIndicatorFilename": "Controller/HoverImages/ModalButton_Hover",
    "pressedFrame": 1,
    "disabledFrame": 2,
    "hoveredFrame": 3,
    "textString": "Pit / Countess / Malus",
    "pointSize": "$MediumFontSize",
    "textColor": "$FontColorLightGray",
    "disabled": true
   }
  },
  {
   "type": "ButtonWidget",
   "name": "NodeCatacombs",
   "fields": {
    "rect": {
     "x": 290,
     "y": 800
    },
    "filename": "Panel\\Modals\\ModalButton",
    "focusIndicatorFilename": "Controller/HoverImages/ModalButton_Hover",
    "pressedFrame": 1,
    "disabledFrame": 2,
    "hoveredFrame": 3,
    "textString": "Catacombs",
    "pointSize": "$MediumFontSize",
    "textColor": "$FontColorLightGray",
    "disabled": true
   }
  },
  {
   "type": "ButtonWidget",
   "name": "CloseButton",
   "fields": {
    "rect": {
     "x": 1075,
     "y": 9
    },
    "filename": "PANEL\\closebtn_4x",
    "hoveredFrame": 3,
    "tooltipString": "@d2r:strClose",
    "sound": "cursor_close_window_hd",
    "onClickMessage": "PanelManager:ClosePanel:darkestdiablo/RouteMap",
    "pressedFrame": 1
   }
  }
 ]
})json";

static auto OnUiMessage(const D2RL::PluginContext* context, const D2RL::SharedEvents::UiMessageEvent* event, void*) noexcept -> D2RL::SharedEvents::UiMessageAction {
	using Action = D2RL::SharedEvents::UiMessageAction;
	if (context == nullptr || event == nullptr) {
		return Action::Continue;
	}
	const char* target = event->target != nullptr ? event->target : "";
	const char* command = event->command != nullptr ? event->command : "";
	const char* text = event->text != nullptr ? event->text : "";
	char message[256] {};
	if (std::strcmp(target, "MonsterHealth") != 0 && std::strcmp(target, "InputMessage") != 0) {  // those fire all the time
		std::snprintf(message, sizeof(message), "UI message: %s:%s:%s", target, command, text);
		context->LogInfo(message);
	}
	if (Campfire::OnUiMessage(context, target, command, text)) {
		return Action::Consume;
	}
	// Node buttons send "PanelManager:OpenPanel:darkestdiablo/travel-<level id>". The game drops
	// button messages with targets or commands it doesn't know, so the travel order rides on a
	// real command with a made-up panel name. Caught here and consumed before the game sees it.
	static constexpr char TravelPrefix[] = "darkestdiablo/travel-";
	if (std::strcmp(target, "PanelManager") == 0 && std::strcmp(command, "OpenPanel") == 0 &&
		std::strncmp(text, TravelPrefix, sizeof(TravelPrefix) - 1) == 0) {
		const int level = std::atoi(text + sizeof(TravelPrefix) - 1);
		if (level > 0) {
			if (g_panels != nullptr) {
				g_panels->closePanel(context, g_routeMap);
			}
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
		if (arena >= 0 && arena != g_arena.load()) {
			ResetArena();          // a new arena, or the same one after leaving: start from wave 1
			g_arena = arena;
			Later(context, ArrivalDelayMs, StartNextWave);
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
	command->plugin->WriteConsoleMessage("Darkest Diablo plugin 0.8.8 is running.");
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
			.flags      = D2RL::Panels::PanelFlags::CloseOnEscape,
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
	if (context->QueryService(&sharedEvents) == D2RL::ServiceQueryResult::Success && D2RL::HasSharedEventServiceField(sharedEvents, D2RL::SharedEventServiceRequiredSize)) {
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

	context->LogInfo("Darkest Diablo plugin 0.8.8 loaded.");
	return Campfire::Load(context) &&
		context->RegisterConsoleCommand("dd", InfoCommand, "Show that the Darkest Diablo plugin is running.") &&
		context->RegisterConsoleCommand("dd-tp", TownPortalCommand, "Drop a Town Portal scroll at your feet (test).") &&
		context->RegisterConsoleCommand("dd-waves", WavesCommand, "Restart the waves of the arena you stand in (test).") &&
		context->RegisterConsoleCommand("dd-map", MapCommand, "Open or close the route map.");
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {}
