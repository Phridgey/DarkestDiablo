// Ascend in the game. See ascension.h and docs/ascend.md.
//
// The hero starts in town and goes to the arena at once ("goto"). From the first step inside, the
// save says "fight" (legacy.h): leaving the game in any way is a death. Stepping out (Town Portal)
// leads straight back. The boss comes out once the hero is near its spot. Its death writes "won",
// spills the loot and brings Tyrael; walking up to Tyrael asks whether to ascend.
//
// One background thread polls the game-state API every 200 ms; console commands run on the UI
// thread.
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
struct Arena { int level, originX, originY; Cell hero, boss, guide; int advised; };
constexpr Arena Arenas[5] = {
	{ 36,  4500, 1600, { 9, 19 },  { 9, 6 },   { 9, 11 },  15 },
	{ 56,  3000, 1300, { 16, 27 }, { 15, 12 }, { 16, 21 }, 33 },
	{ 101, 3500, 1300, { 29, 18 }, { 12, 14 }, { 12, 12 }, 51 },
	{ 106, 2500, 1000, { 12, 20 }, { 12, 10 }, { 12, 14 }, 69 },
	{ 130, 2500, 2684, { 27, 40 }, { 25, 36 }, { 27, 37 }, 87 },
};
constexpr int SpawnReach   = 50;  // sub-tiles; the game only spawns near the hero
// Tyrael (990_ascension.py) can't be clicked himself; a click on him walks the hero there. A portal
// object crashed the game when clicked (2026-11-25): it leads to level 0.
constexpr int GuideReach   = 5;
constexpr int AttackSpeed = 150, CastSpeed = 135;  // percent; the speed of movement is in the boss's row
constexpr char PromptPanel[] = "AscendPrompt";

const D2RL::ThreadService*    g_threads   = nullptr;
const D2RL::PanelService*     g_panels    = nullptr;
D2RL::Panels::RegistrationHandle g_prompt = D2RL::Panels::InvalidHandle;

std::mutex g_mutex;  // guards everything below
std::vector<int>      g_supers[5], g_classes[5];
int                   g_life[5] {};
std::string g_hero;        // planned or ascending
int  g_act = 0;            // 1-5; 0 = nothing planned
bool g_ready = false;      // the planned hero is in the game
bool g_inside = false;     // the fight has begun
bool g_seen = false, g_boosted = false, g_won = false, g_guideUp = false, g_armed = true;
int  g_level = -1, g_tries = 0, g_tick = 0, g_guideX = 0, g_guideY = 0, g_guideTries = 0, g_guideRow = -1;
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

// The drops a monster's six can't hold: "spoils" (990_ascension.py), helpless creatures that come out
// around the hero and are killed at once, each dropping its own loot. If the kill misses one it
// stands there with next to no life, to be smashed. (Making the items through the SDK, on the ground,
// crashed the game 3 ms later, 2026-11-24.) UI thread.
constexpr int Spoils = 2;  // of each of the two kinds

void __cdecl KillSpoils(const D2RL::PluginContext* context, void*) noexcept {
	for (int i = 0; i < 2 * Spoils; ++i) {
		RunConsole(context, "cheat killmonster");
	}
}

void __cdecl SpillLoot(const D2RL::PluginContext* context, void*) noexcept {
	const Game::Unit hero = Game::Hero();
	int act = 0;
	{
		const std::lock_guard lock(g_mutex);
		act = g_act;
	}
	if (act == 0 || !hero) {
		return;
	}
	char command[96];
	for (const char* kind : { "g", "j" }) {
		std::snprintf(command, sizeof(command), "cheat mspawn dd_ascendloot%d_%s,%d @ %d,%d", act, kind, Spoils, hero.x + 2, hero.y + 2);
		RunConsole(context, command);
	}
	Later(context, 700, KillSpoils);
}

void __cdecl NoteGuide(const D2RL::PluginContext* context, void*) noexcept;

// Where the guide comes out: the middle of the room, or beside the hero if that is too far away for
// the game to spawn anything, or refused it (second try). UI thread.
void __cdecl CallGuide(const D2RL::PluginContext* context, void*) noexcept {
	char command[96];
	{
		const std::lock_guard lock(g_mutex);
		const Game::Unit hero = Game::Hero();
		if (g_act == 0 || !g_won || g_guideUp || !hero) {
			return;
		}
		const Arena& a = Arenas[g_act - 1];
		int x = X(a, a.guide), y = Y(a, a.guide);
		if (g_guideTries++ > 0 || Distance(hero.x, hero.y, x, y) > SpawnReach) {
			x = hero.x + 5, y = hero.y - 5;
		}
		std::snprintf(command, sizeof(command), "cheat mspawn dd_ascendguide,1 @ %d,%d", x, y);
	}
	RunConsole(context, command);
	Later(context, 700, NoteGuide);
}

void __cdecl NoteGuide(const D2RL::PluginContext* context, void*) noexcept {
	{
		const std::lock_guard lock(g_mutex);
		if (g_act == 0 || !g_won) {
			return;
		}
		for (const auto& m : Game::Monsters(-1, false)) {
			if (g_guideRow >= 0 && static_cast<int>(m.unit.row) == g_guideRow) {
				g_guideX = m.unit.x, g_guideY = m.unit.y;
				g_guideUp = true;
				g_armed = false;  // the question waits until the hero has stepped away once
				Log(context, "Ascend: Tyrael stands at %d,%d.", m.unit.x, m.unit.y);
				return;
			}
		}
		if (g_guideTries >= 4) {
			context->LogWarn("Ascend: Tyrael did not appear; Save and Exit still ascends.");
			return;
		}
	}
	Later(context, 500, CallGuide);
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
	Later(context, 1200, SpillLoot);
	Later(context, 3500, CallGuide);
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
		} else if (g_guideUp) {
			const double d = Distance(hero.x, hero.y, g_guideX, g_guideY);
			if (!g_armed && d > GuideReach + 3) {
				g_armed = true;
			} else if (g_armed && d <= GuideReach) {
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
		std::string key;
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
		} else if (key == "ascendguide" && words >> n) {
			g_guideRow = n;
		}
	}
	Log(context, "Ascend: %d bosses, %d of them in Act I.", static_cast<int>(g_supers[0].size() + g_supers[1].size() + g_supers[2].size() + g_supers[3].size() + g_supers[4].size()),
		static_cast<int>(g_supers[0].size()));
}

auto PromptLayout() -> std::string {
	const std::string act = "PanelManager:OpenPanel:darkestdiablo/";
	const auto button = [&](const char* name, int x, const char* text, const char* action) {
		return std::string("{\"type\":\"ButtonWidget\",\"name\":\"") + name + "\",\"fields\":{\"rect\":{\"x\":" + std::to_string(x) +
			",\"y\":380},\"filename\":\"FrontEnd\\\\HD\\\\Final\\\\FrontEnd_ButtonMed\",\"textString\":\"" + text + "\",\"onClickMessage\":\"" + act + action +
			"\",\"pressedFrame\":1,\"hoveredFrame\":3,\"disabledFrame\":2,\"text/style\":\"$StyleFEButtonText\",\"textColor\":\"$FontColorRed\"}}";
	};
	std::string lines;
	int y = 150;
	for (const std::string& part : Wrap("This hero leaves the camp for good. Everything it wears and carries goes to the camp's stash. Anything left on the ground is lost.", 48)) {
		lines += "{\"type\":\"TextBoxWidget\",\"name\":\"Text" + std::to_string(y) + "\",\"fields\":{\"rect\":{\"x\":-780,\"y\":" + std::to_string(y) + ",\"width\":1560,\"height\":54},\"text\":\"" + part +
			"\",\"style\":{\"fontColor\":\"$FontColorWhite\",\"alignment\":{\"h\":\"center\"},\"pointSize\":\"$LargeFontSize\",\"dropShadow\":\"$DefaultDropShadow\"}}},";
		y += 54;
	}
	return std::string("{\"type\":\"Panel\",\"name\":\"darkestdiablo/") + PromptPanel + "\",\"fields\":{\"anchor\":{\"x\":0.5,\"y\":0.35},\"fitToParent\":true},\"children\":["
		"{\"type\":\"NineTileImageWidget\",\"name\":\"Back\",\"fields\":{\"filename\":\"PANEL/TooltipBorder/TooltipBorder\",\"backgroundColor\":[0.06,0.0,0.0,0.95],\"tileScale\":1.0,"
		"\"rect\":{\"x\":-850,\"y\":0,\"width\":1700,\"height\":560}}},"
		"{\"type\":\"TextBoxWidget\",\"name\":\"Title\",\"fields\":{\"rect\":{\"x\":-650,\"y\":40,\"width\":1300,\"height\":80},\"text\":\"Ascend?\","
		"\"style\":{\"fontColor\":\"$FontColorRed\",\"alignment\":{\"h\":\"center\"},\"pointSize\":\"$XExtraLargeFontSize\",\"dropShadow\":\"$DefaultDropShadow\"}}},"
		+ lines +
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
	g_ready = g_inside = g_seen = g_boosted = g_won = g_guideUp = false;
	g_armed = true;
	g_tries = g_guideTries = 0;
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
