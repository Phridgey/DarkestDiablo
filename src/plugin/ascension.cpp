// Ascend in the game. See ascension.h and docs/ascend.md.
//
// The hero starts in town and goes to the arena at once ("goto"). From the first step inside, the
// save says "fight" (legacy.h): leaving the game in any way is a death. Stepping out (Town Portal)
// leads straight back. The boss comes out once the hero is near its spot. Its death writes "won",
// spills the loot and opens a portal; walking up to the portal asks whether to ascend.
//
// One background thread polls the game-state API every 200 ms; console commands run on the UI
// thread, item work on the game thread.
#include "ascension.h"
#include "campfire.h"
#include "gameapi.h"
#include "gamewrite.h"
#include <D2RLPlugin/core_exports.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

namespace Ascension {

namespace {

struct Cell { int x, y; };
// Spots are map cells of src/maps/ascend<act>.json, read off the rooms; not yet checked in game.
struct Arena { int level, originX, originY; Cell hero, boss, portal; int advised; };
constexpr Arena Arenas[5] = {
	{ 36,  4500, 1600, { 9, 19 },  { 9, 6 },   { 9, 11 },  15 },
	{ 56,  3000, 1300, { 16, 27 }, { 15, 12 }, { 16, 21 }, 33 },
	{ 101, 3500, 1300, { 29, 18 }, { 12, 14 }, { 12, 12 }, 51 },
	{ 106, 2500, 1000, { 12, 20 }, { 12, 10 }, { 12, 14 }, 69 },
	{ 130, 2500, 2684, { 27, 40 }, { 25, 36 }, { 27, 37 }, 87 },
};
constexpr int SpawnReach   = 50;  // sub-tiles; the game only spawns near the hero
constexpr int PortalRow    = 60;  // objects.txt: PortalPermanent (red, stays open)
constexpr int PortalReach  = 4;
constexpr int AttackSpeed = 150, CastSpeed = 135;  // percent; the speed of movement is in the boss's row
constexpr int BonusLevel[5] = { 36, 54, 72, 90, 99 };  // item level of the plugin's drops: affixes of the next act
constexpr char PromptPanel[] = "AscendPrompt";

const D2RL::ThreadService*    g_threads   = nullptr;
const D2RL::PanelService*     g_panels    = nullptr;
const D2RL::InventoryService* g_inventory = nullptr;
const D2RL::ItemService*      g_items     = nullptr;
D2RL::Panels::RegistrationHandle g_prompt = D2RL::Panels::InvalidHandle;

std::mutex g_mutex;  // guards everything below
std::vector<int>      g_supers[5], g_classes[5];
std::vector<uint32_t> g_bases[5];
int                   g_life[5] {};
std::string g_hero;        // planned or ascending
int  g_act = 0;            // 1-5; 0 = nothing planned
bool g_ready = false;      // the planned hero is in the game
bool g_inside = false;     // the fight has begun
bool g_seen = false, g_boosted = false, g_won = false, g_portalUp = false, g_armed = true;
int  g_level = -1, g_tries = 0, g_tick = 0, g_portalX = 0, g_portalY = 0;
int  g_bossX = 0, g_bossY = 0;
std::atomic<uintptr_t> g_game { 1 };
std::atomic<bool> g_active { false };

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
	const HMODULE core = GetModuleHandleA(CE::CoreDllName);
	const auto execute = core == nullptr ? nullptr : reinterpret_cast<CE::ExecuteConsoleCommandFn>(GetProcAddress(core, CE::ExecuteConsoleCommandInfo.name));
	const bool ok = execute != nullptr && execute(command);
	Log(context, "Ascend: console %s -> %s", command, ok ? "sent" : "refused");
	return ok;
}

void Later(const D2RL::PluginContext* context, int delayMs, D2RL::Threads::Callback callback) noexcept {
	const uintptr_t game = g_game.load();
	try {
		std::thread([=] {
			std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
			if (game == g_game.load()) {
				g_threads->runOnUiThread(context, callback, reinterpret_cast<void*>(game));
			}
		}).detach();
	} catch (...) {
	}
}

auto X(const Arena& a, Cell c) noexcept -> int { return (a.originX + c.x) * 5 + 2; }
auto Y(const Arena& a, Cell c) noexcept -> int { return (a.originY + c.y) * 5 + 2; }
auto Distance(int ax, int ay, int bx, int by) noexcept -> double { return std::hypot(ax - bx, ay - by); }

void __cdecl GoToArena(const D2RL::PluginContext* context, void*) noexcept {
	int level = 0;
	{
		const std::lock_guard lock(g_mutex);
		if (g_act == 0 || !g_ready || g_level == Arenas[g_act - 1].level) {
			return;
		}
		level = Arenas[g_act - 1].level;
	}
	char command[32];
	std::snprintf(command, sizeof(command), "goto %d", level);
	RunConsole(context, command);
}

// "goto" puts the hero anywhere in the level.
void __cdecl PlaceHero(const D2RL::PluginContext* context, void*) noexcept {
	char command[64] {};
	{
		const std::lock_guard lock(g_mutex);
		if (g_act == 0 || g_level != Arenas[g_act - 1].level) {
			return;
		}
		const Arena& a = Arenas[g_act - 1];
		std::snprintf(command, sizeof(command), "location %d,%d", X(a, a.hero), Y(a, a.hero));
	}
	RunConsole(context, command);
}

void __cdecl SpawnBoss(const D2RL::PluginContext* context, void*) noexcept {
	char command[96] {};
	{
		const std::lock_guard lock(g_mutex);
		if (g_act == 0 || g_seen || g_won || g_supers[g_act - 1].empty()) {
			return;
		}
		const Arena& a = Arenas[g_act - 1];
		static std::mt19937 rng { std::random_device {}() };
		const auto& rows = g_supers[g_act - 1];
		const int row = rows[std::uniform_int_distribution<size_t>(0, rows.size() - 1)(rng)];
		// A spot that refuses the boss: try near the hero from the third time on.
		int x = X(a, a.boss), y = Y(a, a.boss);
		const Game::Unit hero = Game::Hero();
		if (g_tries >= 3 && hero) {
			x = hero.x + 8, y = hero.y - 8;
		}
		std::snprintf(command, sizeof(command), "cheat monster super:%d @ %d,%d", row, x, y);
	}
	RunConsole(context, "cheat nominions 1");
	RunConsole(context, command);
	RunConsole(context, "cheat resetnominions");
}

// Background thread (Copies searches memory, about 1-2 s).
void Boost(const D2RL::PluginContext* context, Game::Unit boss, int life) noexcept {
	try {
		const auto copies = GameWrite::Copies(boss);
		const int before = Game::Stat(boss, Game::StatId::MaxLife).value_or(-1);
		const int now = Game::Stat(boss, Game::StatId::Life).value_or(before);
		const int max = life > 0 ? GameWrite::SetStat(copies, Game::StatId::MaxLife, life) : 0;
		// What was hit off meanwhile stays off.
		const int cur = life > 0 && before > 0 ? GameWrite::SetStat(copies, Game::StatId::Life, (std::max)(1, static_cast<int>(static_cast<double>(life) * now / before))) : 0;
		const int attack = GameWrite::SetStat(copies, Game::StatId::AttackRate, AttackSpeed);
		const int cast = GameWrite::SetStat(copies, 69, CastSpeed);  // other_animrate
		Log(context, "Ascend: boss life %d -> %d (%d+%d written), attack rate %d written, other rate %d written, %d copies.", before, life, max, cur, attack, cast,
			static_cast<int>(copies.size()));
	} catch (...) {
		context->LogWarn("Ascend: the boss's numbers could not be written.");
	}
}

auto Create(const D2RL::PluginContext* context, D2RL::PlayerHandle player, uint32_t code, D2RL::Items::Quality quality, int level, int x, int y) noexcept -> bool {
	const D2RL::Items::ItemCreateSpec spec {
		.structSize      = D2RL::Items::ItemCreateSpecSize,
		.code            = code,
		.quality         = quality,
		.qualityRecordId = D2RL::Items::RandomQualityRecord,
		.itemLevel       = static_cast<uint32_t>(level),
		.seedMode        = D2RL::Items::SeedMode::Random,
		.quantity        = D2RL::Items::DefaultValue,
		.durability      = D2RL::Items::DefaultValue,
		.destination     = {
			.structSize = D2RL::Items::ItemDestinationSize,
			.container  = D2RL::Items::ItemContainer::Ground,
			.placement  = D2RL::Items::Placement::Exact,
			.x          = static_cast<uint32_t>(x),
			.y          = static_cast<uint32_t>(y),
		},
	};
	D2RL::ItemHandle made = D2RL::InvalidItemHandle;
	return g_items->createItem(context, player, &spec, &made) == D2RL::Items::Result::Success;
}

// Game thread: the drops a monster's six can't hold. Sets and uniques come from its own loot
// (making one here crashed the game, CLAUDE.md).
void __cdecl SpillLoot(const D2RL::PluginContext* context, void*) noexcept {
	using D2RL::Items::Quality;
	D2RL::PlayerHandle player = D2RL::InvalidPlayerHandle;
	if (g_items == nullptr || g_inventory->getLocalPlayer(context, &player) != D2RL::Inventory::Result::Success) {
		context->LogWarn("Ascend: no item service; only the boss's own loot.");
		return;
	}
	std::vector<uint32_t> bases;
	int level = 0, x = 0, y = 0;
	{
		const std::lock_guard lock(g_mutex);
		if (g_act == 0) {
			return;
		}
		bases = g_bases[g_act - 1];
		level = BonusLevel[g_act - 1];
		x = g_bossX, y = g_bossY;
	}
	std::mt19937 rng { std::random_device {}() };
	std::shuffle(bases.begin(), bases.end(), rng);
	struct Drop { uint32_t code; Quality quality; };
	std::vector<Drop> drops;
	for (size_t i = 0; i < bases.size() && i < 9; ++i) {
		drops.push_back({ bases[i], Quality::Rare });
	}
	using D2RL::Items::MakeItemCode;
	for (int i = 0; i < 2; ++i) {
		drops.push_back({ MakeItemCode("rin"), Quality::Rare });
		drops.push_back({ MakeItemCode("amu"), Quality::Rare });
		drops.push_back({ MakeItemCode("jew"), Quality::Rare });
		drops.push_back({ MakeItemCode("cm1"), Quality::Magic });
		drops.push_back({ MakeItemCode("cm3"), Quality::Magic });
	}
	drops.push_back({ MakeItemCode("cm2"), Quality::Magic });
	int made = 0;
	for (size_t i = 0; i < drops.size(); ++i) {  // a growing spiral around the corpse
		const double angle = 2.4 * static_cast<double>(i), reach = 3.0 + 0.45 * static_cast<double>(i);
		const int dx = static_cast<int>(reach * std::cos(angle)), dy = static_cast<int>(reach * std::sin(angle));
		made += Create(context, player, drops[i].code, drops[i].quality, level, x + dx, y + dy) ||
			Create(context, player, drops[i].code, drops[i].quality, level, x, y) ? 1 : 0;
	}
	Log(context, "Ascend: %d of %d extra items dropped at item level %d.", made, static_cast<int>(drops.size()), level);
}

void __cdecl NotePortal(const D2RL::PluginContext* context, void*) noexcept {
	const std::lock_guard lock(g_mutex);
	if (g_act == 0 || !g_won) {
		return;
	}
	for (const Game::Unit& o : Game::Objects(-1, static_cast<uint32_t>(PortalRow))) {
		if (o.placed && Distance(o.x, o.y, g_portalX, g_portalY) <= 30) {
			g_portalX = o.x, g_portalY = o.y;
			g_portalUp = true;
			g_armed = false;  // the question waits until the hero has stepped away once
			Log(context, "Ascend: portal at %d,%d.", o.x, o.y);
			return;
		}
	}
	context->LogWarn("Ascend: the portal did not appear; Save and Exit still ascends.");
}

// UI thread. Objects appear at the hero's feet, so the hero goes there and back (zone.cpp).
void __cdecl OpenPortal(const D2RL::PluginContext* context, void*) noexcept {
	int x = 0, y = 0;
	const Game::Unit hero = Game::Hero();
	{
		const std::lock_guard lock(g_mutex);
		if (g_act == 0 || !g_won || !hero) {
			return;
		}
		const Arena& a = Arenas[g_act - 1];
		g_portalX = x = X(a, a.portal), g_portalY = y = Y(a, a.portal);
	}
	char command[64];
	std::snprintf(command, sizeof(command), "location %d,%d", x, y);
	RunConsole(context, command);
	std::snprintf(command, sizeof(command), "cheat ospawn %d", PortalRow);
	RunConsole(context, command);
	std::snprintf(command, sizeof(command), "location %d,%d", hero.x, hero.y);
	RunConsole(context, command);
	context->WriteConsoleMessage("The way is open. Take what you want, then step up to the portal to ascend.");
	Later(context, 500, NotePortal);
}

void __cdecl ShowPrompt(const D2RL::PluginContext* context, void*) noexcept {
	if (g_panels != nullptr && g_prompt != D2RL::Panels::InvalidHandle) {
		g_panels->openPanel(context, g_prompt);
	}
}

void __cdecl HidePrompt(const D2RL::PluginContext* context, void*) noexcept {
	if (g_panels != nullptr && g_prompt != D2RL::Panels::InvalidHandle) {
		g_panels->closePanel(context, g_prompt);
	}
}

// True when the boss has just died. Locked: the Campfire is told by the caller, without the lock.
auto TickFight(const D2RL::PluginContext* context, const Game::Unit& hero) -> bool {
	const Arena& a = Arenas[g_act - 1];
	const Game::MonsterDetails* boss = nullptr;
	const auto monsters = Game::Monsters(-1, false);
	for (const auto& m : monsters) {
		if (std::find(g_classes[g_act - 1].begin(), g_classes[g_act - 1].end(), static_cast<int>(m.unit.row)) != g_classes[g_act - 1].end()) {
			boss = &m;
			break;
		}
	}
	if (boss != nullptr && boss->alive) {
		g_seen = true;
		g_bossX = boss->unit.x, g_bossY = boss->unit.y;
		if (!g_boosted) {
			g_boosted = true;
			Log(context, "Ascend: the boss stands (class %u, level row, %d affixes seen).", boss->unit.row, static_cast<int>(boss->enchantments.size()));
			std::thread(Boost, context, boss->unit, g_life[g_act - 1]).detach();
		}
	} else if (boss != nullptr && g_seen && !Game::HeroInfo().dead) {
		g_won = true;
		return true;
	} else if (!g_seen && g_tick % 15 == 0 && g_tries < 20 && (g_tries >= 3 || Distance(hero.x, hero.y, X(a, a.boss), Y(a, a.boss)) <= SpawnReach)) {
		++g_tries;
		g_threads->runOnUiThread(context, SpawnBoss, nullptr);
	}
	return false;
}

void Win(const D2RL::PluginContext* context, const std::string& hero) {
	Campfire::AscensionWon(hero);
	Log(context, "Ascend: the boss is dead; %s has won.", hero.c_str());
	g_threads->runOnGameThread(context, SpillLoot, nullptr);
	Later(context, 1500, OpenPortal);
}

void Tick(const D2RL::PluginContext* context) noexcept {
	try {
		std::unique_lock lock(g_mutex);
		++g_tick;
		if (g_act == 0 || !g_inside || g_level != Arenas[g_act - 1].level) {
			return;
		}
		const Game::Unit hero = Game::Hero();
		if (!hero) {
			return;
		}
		if (!g_won) {
			if (TickFight(context, hero)) {
				const std::string name = g_hero;
				lock.unlock();
				Win(context, name);
			}
		} else if (g_portalUp) {
			const double d = Distance(hero.x, hero.y, g_portalX, g_portalY);
			if (!g_armed && d > PortalReach + 4) {
				g_armed = true;
			} else if (g_armed && d <= PortalReach) {
				g_armed = false;
				g_threads->runOnUiThread(context, ShowPrompt, nullptr);
			}
		}
	} catch (...) {
	}
}

void ReadLines(const D2RL::PluginContext* context) {
	std::istringstream in(Campfire::LootData());
	std::string line;
	while (std::getline(in, line)) {
		std::istringstream words(line);
		std::string key, code;
		int act = 0, n = 0;
		words >> key;
		if (key.rfind("ascend", 0) != 0 || !(words >> act) || act < 1 || act > 5) {
			continue;
		}
		if (key == "ascendsuper" && words >> n) {
			g_supers[act - 1].push_back(n);
		} else if (key == "ascendclass" && words >> n) {
			g_classes[act - 1].push_back(n);
		} else if (key == "ascendhp" && words >> n) {
			g_life[act - 1] = n;
		} else if (key == "ascenditem" && words >> code && code.size() == 3) {
			g_bases[act - 1].push_back(D2RL::Items::MakeItemCode(code[0], code[1], code[2]));
		}
	}
	Log(context, "Ascend: %d bosses, %d of them in Act I.", static_cast<int>(g_supers[0].size() + g_supers[1].size() + g_supers[2].size() + g_supers[3].size() + g_supers[4].size()),
		static_cast<int>(g_supers[0].size()));
}

auto PromptLayout() -> std::string {
	const std::string act = "PanelManager:OpenPanel:darkestdiablo/";
	const auto button = [&](const char* name, int x, const char* text, const char* action) {
		return std::string("{\"type\":\"ButtonWidget\",\"name\":\"") + name + "\",\"fields\":{\"rect\":{\"x\":" + std::to_string(x) +
			",\"y\":330},\"filename\":\"FrontEnd\\\\HD\\\\Final\\\\FrontEnd_ButtonMed\",\"textString\":\"" + text + "\",\"onClickMessage\":\"" + act + action +
			"\",\"pressedFrame\":1,\"hoveredFrame\":3,\"disabledFrame\":2,\"text/style\":\"$StyleFEButtonText\",\"textColor\":\"$FontColorRed\"}}";
	};
	return std::string("{\"type\":\"Panel\",\"name\":\"darkestdiablo/") + PromptPanel + "\",\"fields\":{\"anchor\":{\"x\":0.5,\"y\":0.35},\"fitToParent\":true},\"children\":["
		"{\"type\":\"NineTileImageWidget\",\"name\":\"Back\",\"fields\":{\"filename\":\"PANEL/TooltipBorder/TooltipBorder\",\"backgroundColor\":[0.06,0.0,0.0,0.95],\"tileScale\":1.0,"
		"\"rect\":{\"x\":-700,\"y\":0,\"width\":1400,\"height\":500}}},"
		"{\"type\":\"TextBoxWidget\",\"name\":\"Title\",\"fields\":{\"rect\":{\"x\":-650,\"y\":40,\"width\":1300,\"height\":80},\"text\":\"Ascend?\","
		"\"style\":{\"fontColor\":\"$FontColorRed\",\"alignment\":{\"h\":\"center\"},\"pointSize\":\"$XExtraLargeFontSize\",\"dropShadow\":\"$DefaultDropShadow\"}}},"
		"{\"type\":\"TextBoxWidget\",\"name\":\"Text\",\"fields\":{\"rect\":{\"x\":-650,\"y\":140,\"width\":1300,\"height\":180},"
		"\"text\":\"This hero leaves the camp for good. Everything it wears and carries goes to the camp's stash. Anything left on the ground is lost.\","
		"\"style\":{\"fontColor\":\"$FontColorWhite\",\"alignment\":{\"h\":\"center\"},\"pointSize\":\"$LargeFontSize\",\"dropShadow\":\"$DefaultDropShadow\"}}}," +
		button("Yes", -560, "Ascend", "ascend-yes") + "," + button("No", 60, "Not yet", "ascend-no") + "]}";
}

}  // namespace

auto Load(const D2RL::PluginContext* context) noexcept -> bool {
	try {
		if (context->QueryService(&g_threads) != D2RL::ServiceQueryResult::Success || !D2RL::HasThreadServiceField(g_threads, D2RL::ThreadServiceRequiredSize)) {
			g_threads = nullptr;
			context->LogWarn("Ascend: thread service missing; nobody can ascend.");
			return true;
		}
		if (context->QueryService(&g_inventory) != D2RL::ServiceQueryResult::Success || !D2RL::HasInventoryServiceField(g_inventory, D2RL::InventoryServiceRequiredSize) ||
			context->QueryService(&g_items) != D2RL::ServiceQueryResult::Success || !D2RL::HasItemServiceField(g_items, D2RL::ItemServiceRequiredSize)) {
			g_items = nullptr;
		}
		const D2RL::ResourceService* resources = nullptr;
		if (context->QueryService(&g_panels) == D2RL::ServiceQueryResult::Success && D2RL::HasPanelServiceField(g_panels, D2RL::PanelServiceRequiredSize) &&
			context->QueryService(&resources) == D2RL::ServiceQueryResult::Success && D2RL::HasResourceServiceField(resources, D2RL::ResourceServiceRequiredSize)) {
			const std::string json = PromptLayout();
			const std::string path = std::string("data/global/ui/layouts/darkestdiablo/") + PromptPanel + "hd.json";
			const D2RL::Resources::ResourceRegistration layout {
				.structSize = D2RL::Resources::ResourceRegistrationSize,
				.path       = path.c_str(),
				.bytes      = json.data(),
				.byteCount  = json.size(),
			};
			const D2RL::Panels::PanelRegistration panel {
				.structSize = D2RL::Panels::PanelRegistrationSize,
				.flags      = D2RL::Panels::PanelFlags::CloseOnEscape,
				.localId    = PromptPanel,
			};
			D2RL::Resources::RegistrationHandle handle = D2RL::Resources::InvalidHandle;
			if (resources->registerResource(context, &layout, &handle) != D2RL::Resources::Result::Success ||
				g_panels->registerPanel(context, &panel, &g_prompt) != D2RL::Panels::Result::Success) {
				g_prompt = D2RL::Panels::InvalidHandle;
				context->LogWarn("Ascend: no portal question; Save and Exit ascends after the win.");
			}
		} else {
			g_panels = nullptr;
		}
		ReadLines(context);
		std::thread([context] {
			for (;;) {
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
				Tick(context);
			}
		}).detach();
	} catch (...) {
		context->LogWarn("Ascend: setting up failed.");
	}
	return true;
}

void Plan(const std::string& hero, int act) noexcept {
	const std::lock_guard lock(g_mutex);
	const bool able = g_threads != nullptr && act >= 1 && act <= 5 && !g_supers[act - 1].empty() && !hero.empty();
	g_hero = able ? hero : std::string();
	g_act = able ? act : 0;
	g_ready = g_inside = g_seen = g_boosted = g_won = g_portalUp = false;
	g_armed = true;
	g_tries = 0;
}

void OnReady(const D2RL::PluginContext* context, const std::string& hero) noexcept {
	{
		const std::lock_guard lock(g_mutex);
		if (g_act == 0) {
			return;
		}
		if (g_hero != hero) {  // another hero came in instead
			g_act = 0;
			return;
		}
		g_ready = true;
	}
	g_active = true;
	Later(context, 2500, GoToArena);
}

void OnLevelChanged(const D2RL::PluginContext* context, int levelId) noexcept {
	std::string began;
	bool back = false;
	{
		const std::lock_guard lock(g_mutex);
		g_level = levelId;
		if (g_act == 0 || !g_ready) {
			return;
		}
		if (levelId == Arenas[g_act - 1].level) {
			if (!g_inside) {
				g_inside = true;
				began = g_hero;
			}
		} else {
			back = g_inside;  // a Town Portal: no way out
		}
	}
	if (!began.empty()) {
		Campfire::AscensionBegan(began);
		Log(context, "Ascend: %s stands in the arena. There is no way back.", began.c_str());
	}
	Later(context, 1000, back ? GoToArena : PlaceHero);
	if (back) {
		context->WriteConsoleMessage("There is no way out.");
	}
}

void OnMonsterDied(const D2RL::PluginContext* context, uint32_t classId) noexcept {
	try {
		std::unique_lock lock(g_mutex);
		if (g_act == 0 || !g_inside || g_won || !g_seen ||
			std::find(g_classes[g_act - 1].begin(), g_classes[g_act - 1].end(), static_cast<int>(classId)) == g_classes[g_act - 1].end() || Game::HeroInfo().dead) {
			return;
		}
		g_won = true;
		const std::string name = g_hero;
		lock.unlock();
		Win(context, name);
	} catch (...) {
	}
}

void OnGameLeft() noexcept {
	++g_game;
	g_active = false;
	const std::lock_guard lock(g_mutex);
	g_act = 0;
	g_level = -1;
	g_ready = g_inside = false;
	g_hero.clear();
}

auto OnAction(const D2RL::PluginContext* context, const char* action) noexcept -> bool {
	if (std::strcmp(action, "ascend-no") == 0) {
		HidePrompt(context, nullptr);
		return true;
	}
	if (std::strcmp(action, "ascend-yes") == 0) {
		HidePrompt(context, nullptr);
		bool won = false;
		{
			const std::lock_guard lock(g_mutex);
			won = g_won;
		}
		if (won) {
			Campfire::LeaveForCampfire(context);
		}
		return true;
	}
	return false;
}

auto AdvisedLevel(int act) noexcept -> int {
	return act >= 1 && act <= 5 ? Arenas[act - 1].advised : 1;
}

auto Planned() noexcept -> bool {
	const std::lock_guard lock(g_mutex);
	return g_act != 0;
}

auto Active() noexcept -> bool {
	return g_active.load();
}

}
