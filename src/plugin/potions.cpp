// Darkest Diablo — the permanent potions in the game: the four slots on screen, the keys, the charge
// file and what a potion does to the hero. The rules are in potions.h; docs/potions.md says how it
// plays and what is still to be checked.
//
// The loader can't make, change or remove an item in the belt, so the potions are not items: the
// plugin draws them over the belt's place and works their effects itself. Life, mana and stamina are
// changed through gamewrite.h, like every write.

#include "potions.h"
#include "potionsgame.h"
#include "stashgame.h"
#include "campfire.h"
#include "gameapi.h"
#include "gamestate.h"
#include "gamewrite.h"
#include "routepages.h"
#include <D2RLPlugin/core_exports.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <utility>

namespace PotionsGame {

namespace {

namespace fs = std::filesystem;
using Container = D2RL::Items::ItemContainer;
using Clock = std::chrono::steady_clock;

constexpr char ActionPrefix[] = "darkestdiablo/potion-";
constexpr int  TickMs    = 100;
constexpr int  HudMs     = 500;   // the slots are redrawn at most this often
constexpr int  OwnerMs   = 1000;  // how often level, belt and passives are looked up
constexpr int  SaveMs    = 5000;
constexpr int  RescanMs  = 10000; // at least this long between looks for the hero's copies

// states.txt rows a supporting potion ends.
constexpr int Freeze = 1, Poison = 2, Cold = 11;

const D2RL::ThreadService*    g_threads   = nullptr;
const D2RL::PanelService*     g_panels    = nullptr;
const D2RL::WidgetService*    g_widgets   = nullptr;
const D2RL::ResourceService*  g_resources = nullptr;
const D2RL::InventoryService* g_inventory = nullptr;
const D2RL::ItemService*      g_items     = nullptr;

// Never call the Campfire or the stash while holding this: they send UI messages under their own
// locks, which come back here.
std::recursive_mutex g_mutex;
using Lock = std::lock_guard<std::recursive_mutex>;
Potions::State g_state;
Potions::Owner g_owner;
fs::path       g_file;

std::atomic<bool> g_inGame { false };
std::atomic<int>  g_belt { 0 };
std::atomic<int>  g_picking { -1 };
std::atomic<bool> g_hudQueued { false };
std::atomic<bool> g_toBag { false };  // dd-potions bag: throwing potions go to the bag instead
std::mt19937      g_rng { std::random_device {}() };  // UI thread

// What is working on the hero now.
struct Restore { uint16_t stat, max; double perSecond; double left; };  // whole points
struct Ward { int kind; Clock::time_point end; };
std::vector<Restore> g_restores;
std::vector<Ward>    g_wards;
double g_rejuvenate = 0;  // share of all life and mana to give at the next tick

void Log(const D2RL::PluginContext* context, const char* format, ...) noexcept {
	char message[400] {};
	va_list args;
	va_start(args, format);
	std::vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	context->LogInfo(message);
}

// Locked. Only LoadHero makes the file (`create`): once it is gone the hero was buried (camp.h), and
// writing it again would give its potions to a later hero of the same name.
void Save(const D2RL::PluginContext* context, bool create = false) {
	std::error_code ec;
	if (g_file.empty() || (!create && !fs::exists(g_file, ec))) {
		return;
	}
	fs::create_directories(g_file.parent_path(), ec);
	if (!Camp::WriteFile(g_file, Potions::Serialize(g_state))) {
		context->LogWarn("Potions: could not write the hero's potions file.");
	}
}

// ---- The slots on screen -----------------------------------------------------------------------
// As in stash.cpp: each new content goes to the next panel of a short row. UI thread only.
constexpr int RowSize = 3;
D2RL::Panels::RegistrationHandle    g_hud[RowSize] {};
D2RL::Resources::RegistrationHandle g_hudLayout[RowSize] {};
int         g_hudCount = 0, g_hudNow = -1;
std::string g_hudContent;

auto HudName(int i) -> std::string { return "darkestdiablo/PotionHud" + std::to_string(i); }

auto IsOpen(const D2RL::PluginContext* context, int i) noexcept -> bool {
	D2RL::Panels::PanelInfo info { .structSize = D2RL::Panels::PanelInfoSize };
	return i >= 0 && g_panels->getPanelInfo(context, g_hud[i], &info) == D2RL::Panels::Result::Success &&
		info.presentationState == D2RL::Panels::PresentationState::Open;
}

void Unload(const D2RL::PluginContext* context, const std::string& panel) noexcept {
	if (g_widgets != nullptr) {
		const D2RL::Widgets::UiAction action { .structSize = D2RL::Widgets::UiActionSize, .target = "PanelManager", .command = "UnloadPanel", .text = panel.c_str() };
		(void)g_widgets->dispatchUiAction(context, &action);
	}
}

auto RegisterLayout(const D2RL::PluginContext* context, const std::string& name, std::string json, D2RL::Resources::RegistrationHandle& handle) -> bool {
	for (size_t at = json.find(Stash::PanelMark); at != std::string::npos; at = json.find(Stash::PanelMark)) {
		json.replace(at, sizeof(Stash::PanelMark) - 1, name);
	}
	const std::string path = "data/global/ui/layouts/" + name + "hd.json";
	const D2RL::Resources::ResourceRegistration resource {
		.structSize = D2RL::Resources::ResourceRegistrationSize,
		.path       = path.c_str(),
		.bytes      = json.data(),
		.byteCount  = json.size(),
	};
	return g_resources->registerResource(context, &resource, &handle) == D2RL::Resources::Result::Success;
}

void __cdecl ShowHud(const D2RL::PluginContext* context, void*) noexcept {
	g_hudQueued = false;
	try {
		if (g_hudCount == 0) {
			return;
		}
		if (!g_inGame.load()) {
			if (IsOpen(context, g_hudNow)) {
				g_panels->closePanel(context, g_hud[g_hudNow]);
			}
			return;
		}
		std::string json;
		{
			const Lock lock(g_mutex);
			json = Potions::HudLayout(g_state, g_owner, g_picking.load());
		}
		if (g_hudNow >= 0 && json == g_hudContent) {
			if (!IsOpen(context, g_hudNow)) {  // the game closes panels on its own (loading)
				g_panels->openPanel(context, g_hud[g_hudNow]);
			}
			return;
		}
		const int old = g_hudNow, next = (old + 1) % g_hudCount;
		const std::string name = HudName(next);
		if (g_hudLayout[next] != D2RL::Resources::InvalidHandle) {
			g_resources->unregisterResource(context, g_hudLayout[next]);
			g_hudLayout[next] = D2RL::Resources::InvalidHandle;
		}
		if (!RegisterLayout(context, name, json, g_hudLayout[next])) {
			Log(context, "Potions: %s could not take its new layout.", name.c_str());
			return;
		}
		const auto result = g_panels->openPanel(context, g_hud[next]);
		static bool s_logged = false;
		if (result != D2RL::Panels::Result::Success && !s_logged) {
			s_logged = true;
			Log(context, "Potions: opening %s gave code %d.", name.c_str(), static_cast<int>(result));
		}
		g_hudNow = next;
		g_hudContent = result == D2RL::Panels::Result::Success ? json : std::string();
		if (old >= 0 && old != next) {
			if (IsOpen(context, old)) {
				g_panels->closePanel(context, g_hud[old]);
			}
			Unload(context, HudName(old));
		}
	} catch (...) {
		context->LogWarn("Potions: drawing the slots failed.");
	}
}

void QueueHud(const D2RL::PluginContext* context) noexcept {
	if (!g_hudQueued.exchange(true) && g_threads->runOnUiThread(context, ShowHud, nullptr) != D2RL::Threads::Result::Success) {
		g_hudQueued = false;
	}
}

// ---- Items ----------------------------------------------------------------------------------
auto CodeText(uint32_t code) -> std::string {
	std::string text;
	for (int i = 0; i < 4; ++i) {
		const char c = static_cast<char>((code >> (8 * i)) & 0xFF);
		if (c != ' ' && c != '\0') {
			text += c;
		}
	}
	return text;
}

auto CodeNumber(const std::string& text) -> uint32_t {
	uint32_t code = 0;
	for (size_t i = 0; i < 4; ++i) {
		code |= static_cast<uint32_t>(static_cast<uint8_t>(i < text.size() ? text[i] : ' ')) << (8 * i);
	}
	return code;
}

struct Look { std::string pot; int belt = 0; int pots = 0; };  // `pot`: the item to count, else the belt is looked for

auto __cdecl LookAtItem(const D2RL::PluginContext*, const D2RL::Items::ItemInfo* item, void* out) noexcept -> D2RL::Inventory::IterationAction {
	try {
		auto* look = static_cast<Look*>(out);
		const std::string code = item != nullptr ? CodeText(item->code) : std::string();
		if (look->pot.empty()) {
			look->belt = (std::max)(look->belt, Potions::BeltCharges(code));
		} else if (code == look->pot) {
			look->pots += (std::max)(1, item->quantity);
		}
	} catch (...) {
	}
	return D2RL::Inventory::IterationAction::Continue;
}

// Game thread.
void __cdecl LookAtBelt(const D2RL::PluginContext* context, void*) noexcept {
	D2RL::PlayerHandle player = D2RL::InvalidPlayerHandle;
	Look look;
	const D2RL::Inventory::ItemFilter filter { .structSize = D2RL::Inventory::ItemFilterSize, .containerMask = D2RL::Items::ContainerBit(Container::Equipment) };
	if (g_inventory->getLocalPlayer(context, &player) == D2RL::Inventory::Result::Success &&
		g_inventory->forEachInventoryItem(context, player, &filter, LookAtItem, &look) == D2RL::Inventory::Result::Success) {
		g_belt = look.belt;
	}
}

// Game thread. A throwing potion's use after "dd-potions bag": a small stack in the bag. `data` is the kind; the charge was
// taken already and comes back if nothing could be made.
void __cdecl MakePots(const D2RL::PluginContext* context, void* data) noexcept {
	try {
		const int kind = static_cast<int>(reinterpret_cast<intptr_t>(data));
		const char* problem = nullptr;
		const Potions::Pot* pot = nullptr;
		{
			const Lock lock(g_mutex);
			pot = &Potions::PotFor(kind, g_owner);
		}
		D2RL::PlayerHandle player = D2RL::InvalidPlayerHandle;
		Look look;
		look.pot = pot->code;
		const D2RL::Inventory::ItemFilter filter { .structSize = D2RL::Inventory::ItemFilterSize, .containerMask = D2RL::Items::AllItemContainers };
		if (g_inventory->getLocalPlayer(context, &player) != D2RL::Inventory::Result::Success ||
			g_inventory->forEachInventoryItem(context, player, &filter, LookAtItem, &look) != D2RL::Inventory::Result::Success) {
			problem = "Potions: the hero's bag can't be read.";
		} else if (look.pots + Potions::ThrownPerUse > Potions::ThrownCarried) {
			problem = "You have enough of these throwing potions already.";
		} else {
			const D2RL::Items::ItemCreateSpec spec {
				.structSize      = D2RL::Items::ItemCreateSpecSize,
				.code            = CodeNumber(pot->code),
				.quality         = D2RL::Items::Quality::Normal,
				.qualityRecordId = D2RL::Items::RandomQualityRecord,
				.itemLevel       = 1,
				.seedMode        = D2RL::Items::SeedMode::Random,
				.quantity        = static_cast<uint32_t>(Potions::ThrownPerUse),
				.durability      = D2RL::Items::DefaultValue,
				.destination     = {
					.structSize = D2RL::Items::ItemDestinationSize,
					.container  = Container::Inventory,
					.placement  = D2RL::Items::Placement::Automatic,
				},
			};
			D2RL::ItemHandle made = D2RL::InvalidItemHandle;
			const auto result = g_items->createItem(context, player, &spec, &made);
			Log(context, "Potions: %d %s made (code %d).", Potions::ThrownPerUse, pot->code, static_cast<int>(result));
			if (result != D2RL::Items::Result::Success) {
				problem = "No room in the bag for throwing potions.";
			}
		}
		if (problem != nullptr) {
			const Lock lock(g_mutex);
			g_state.Gain(kind, Potions::UseCost, g_owner);
			context->WriteConsoleMessage(problem);
		}
	} catch (...) {
		context->LogWarn("Potions: making throwing potions failed.");
	}
}

// ---- Drinking ---------------------------------------------------------------------------------
auto RunConsole(const char* command) noexcept -> bool {
	namespace CE = D2RL::CoreExports;
	const HMODULE core = GetModuleHandleA(CE::CoreDllName);
	const auto execute = core == nullptr ? nullptr : reinterpret_cast<CE::ExecuteConsoleCommandFn>(GetProcAddress(core, CE::ExecuteConsoleCommandInfo.name));
	return execute != nullptr && execute(command);
}

// Locked.
void StartWard(int kind, double strength) {
	const auto end = Clock::now() + std::chrono::milliseconds(static_cast<int>(Potions::SupportSeconds * 1000 * strength));
	for (auto& ward : g_wards) {
		if (ward.kind == kind) {
			ward.end = (std::max)(ward.end, end);
			return;
		}
	}
	g_wards.push_back({ kind, end });
}

std::vector<uintptr_t> g_copies;      // with g_copiesMutex
Clock::time_point      g_scanned {};
std::atomic<int>       g_failedWrites { 0 };
std::mutex             g_copiesMutex;

// Not on the UI thread. The hero's copies, looked for again when the hero is elsewhere in memory (slow).
auto HeroCopies(const D2RL::PluginContext* context) -> std::vector<uintptr_t> {
	const std::lock_guard<std::mutex> guard(g_copiesMutex);
	const Game::Unit hero = Game::Hero();
	std::erase_if(g_copies, [&](uintptr_t copy) { return !GameWrite::SameUnit(copy, hero); });
	const bool missing = g_copies.size() < 2 || g_failedWrites >= 5 || std::find(g_copies.begin(), g_copies.end(), hero.address) == g_copies.end();
	if (hero && missing && Clock::now() - g_scanned >= std::chrono::milliseconds(RescanMs)) {
		g_copies = GameWrite::Copies(hero);
		g_scanned = Clock::now();
		g_failedWrites = 0;
		Log(context, "Potions: the hero has %d copies in memory.", static_cast<int>(g_copies.size()));
	}
	return g_copies;
}

// ---- Throwing -----------------------------------------------------------------------------------
// Every hero has the skill Throw Potion (131a_throw_skill.py), which throws nothing by itself. A slot's
// press puts the potion's missile in it and casts it at the cursor: with the skill key that holds it, or
// any skill key that holds it for a moment, or the right button. Only a cast that is seen takes a charge.
// Every step is in the game folder's d2rloader/logs/potion-throws.log, which builds and restarts leave alone.
// Filled while the plugin loads, from loot_data.txt.
int                              g_throwSkill = 0;
std::map<std::string, int>       g_throwMissiles;  // by potion; "none": what flies otherwise
std::vector<std::pair<int, int>> g_throwRefs;      // skills and their missiles
std::atomic<bool>          g_throwing { false };
std::atomic<int>           g_bag { 0 };         // the inventory is open: a right click there would use an item
constexpr uint32_t         CastMode = 10;

struct Message { std::string text; bool console; };

void __cdecl OnUi(const D2RL::PluginContext* context, void* data) noexcept {
	auto* message = static_cast<Message*>(data);
	try {
		if (message->console) {
			RunConsole(message->text.c_str());
		} else {
			context->WriteConsoleMessage(message->text.c_str());
		}
	} catch (...) {
	}
	delete message;
}

void ToUi(const D2RL::PluginContext* context, const std::string& text, bool console) {
	auto* message = new Message { text, console };
	if (g_threads->runOnUiThread(context, OnUi, message) != D2RL::Threads::Result::Success) {
		delete message;
	}
}

// The game window in front, with the cursor over the world: not over the HUD.
auto CursorOverWorld() noexcept -> bool {
	const HWND window = GetForegroundWindow();
	DWORD process = 0;
	GetWindowThreadProcessId(window, &process);
	POINT at {};
	RECT client {};
	if (process != GetCurrentProcessId() || !GetCursorPos(&at) || !ScreenToClient(window, &at) || !GetClientRect(window, &client)) {
		return false;
	}
	return at.x >= 0 && at.y >= 0 && at.x < client.right && at.y < client.bottom * 88 / 100;
}

// A line in the log that lasts, and the same in the plugin's own. Returns the line.
auto Note(const D2RL::PluginContext* context, const char* format, ...) noexcept -> std::string {
	char message[700] {};
	va_list args;
	va_start(args, format);
	std::vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	context->LogInfo((std::string("Potions: throw: ") + message).c_str());
	try {
		wchar_t exe[MAX_PATH] {};
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		const fs::path file = fs::path(exe).parent_path() / "d2rloader" / "logs" / "potion-throws.log";
		std::error_code ec;
		const bool large = fs::exists(file, ec) && fs::file_size(file, ec) > 200000;
		std::ofstream out(file, large ? std::ios::trunc : std::ios::app);
		SYSTEMTIME t {};
		GetLocalTime(&t);
		char stamp[40] {};
		std::snprintf(stamp, sizeof(stamp), "%02d-%02d %02d:%02d:%02d.%03d ", t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
		out << stamp << Campfire::Version << " " << message << "\n";
	} catch (...) {
	}
	return message;
}

void PressRight(bool down) noexcept {
	const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;  // the button that counts as right
	INPUT input {};
	input.type = INPUT_MOUSE;
	input.mi.dwFlags = swapped ? (down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP) : (down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP);
	SendInput(1, &input, sizeof(input));
}

void PressKey(int key, bool down) noexcept {
	INPUT input {};
	input.type = INPUT_KEYBOARD;
	input.ki.wVk = static_cast<WORD>(key);
	input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(static_cast<UINT>(key), MAPVK_VK_TO_VSC));
	input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
	SendInput(1, &input, sizeof(input));
}

std::vector<uintptr_t> g_keyTables;  // one throw at a time uses these
Clock::time_point      g_keyScan {};

// The skill keys on the keyboard (from <hero>.key, then Custom.key): number 0-15 and key; and the folder.
auto SavesFolder() -> fs::path {
	const fs::path progress = Campfire::RouteProgressFile();  // <saves>/camp/slot<N>/progress/<hero>.txt
	return progress.empty() ? fs::path {} : progress.parent_path().parent_path().parent_path().parent_path();
}

auto KeyboardSkillKeys() -> std::vector<std::pair<int, int>> {
	const fs::path saves = SavesFolder();
	if (saves.empty()) {
		return {};
	}
	for (const fs::path& file : { saves / (Campfire::RouteProgressFile().stem().string() + ".key"), saves / "Custom.key" }) {
		std::error_code ec;
		auto keys = fs::exists(file, ec) ? Potions::SkillKeys(Camp::ReadFile(file)) : std::vector<std::pair<int, int>> {};
		if (!keys.empty()) {
			return keys;
		}
	}
	return {};
}

auto IsDown(int key) noexcept -> bool { return (GetAsyncKeyState(key) & 0x8000) != 0; }

// What our skill sends off (gamewrite.h, SetMissile). The tables are the game's, for one game.
struct ThrowTable { Game::SkillTable table; std::vector<std::size_t> columns; };
std::mutex              g_tablesMutex;
std::vector<ThrowTable> g_throwTables;
std::string             g_tablesSeen;

// Game thread, when the hero is ready.
void __cdecl FindThrowTables(const D2RL::PluginContext* context, void*) noexcept {
	try {
		const std::lock_guard<std::mutex> guard(g_tablesMutex);
		g_throwTables.clear();
		g_tablesSeen.clear();
		std::vector<Game::SkillTable> tables;
		const D2RL::DataTableService* service = nullptr;
		if (context->QueryService(&service) == D2RL::ServiceQueryResult::Success && D2RL::HasDataTableServiceField(service, D2RL::DataTableServiceRequiredSize)) {
			for (const auto bank : { D2RL::DataTables::Bank::Classic, D2RL::DataTables::Bank::Lod, D2RL::DataTables::Bank::Rotw }) {
				D2RL::DataTables::TableView view { .structSize = D2RL::DataTables::TableViewSize, .bank = bank };
				const auto result = service->getTable(context, bank, D2RL::DataTables::TableId::Skills, &view);
				const Game::SkillTable table { reinterpret_cast<uintptr_t>(view.rows), view.rowCount, view.rowSize };
				std::size_t columns = 0;
				if (result == D2RL::DataTables::Result::Success && table.rows != 0 &&
					std::none_of(tables.begin(), tables.end(), [&](const Game::SkillTable& t) { return t.rows == table.rows; })) {
					tables.push_back(table);
					const auto at = GameWrite::MissileColumns(table, g_throwRefs);
					columns = at.size();
					if (columns == 2 && g_throwSkill > 0 && static_cast<uint32_t>(g_throwSkill) < table.count) {
						g_throwTables.push_back({ table, at });
					}
				}
				g_tablesSeen += " [bank " + std::to_string(static_cast<int>(bank)) + ": " + std::to_string(static_cast<int>(result)) + ", " + std::to_string(view.rowCount) +
					" rows of " + std::to_string(view.rowSize) + ", " + std::to_string(columns) + " missile columns]";
			}
		} else {
			g_tablesSeen = " no table service";
		}
		Game::SetSkillTables(tables);
		Log(context, "Potions: throw tables:%s", g_tablesSeen.c_str());
	} catch (...) {
	}
}

// Puts a potion's missile in our skill ("none": nothing is thrown). True when a table took it.
auto LoadMissile(const std::string& code) -> bool {
	const std::lock_guard<std::mutex> guard(g_tablesMutex);
	const auto missile = g_throwMissiles.find(code);
	std::vector<int> ours;
	for (const auto& [name, id] : g_throwMissiles) {
		ours.push_back(id);
	}
	int done = 0;
	for (const auto& t : missile == g_throwMissiles.end() ? std::vector<ThrowTable> {} : g_throwTables) {
		done += GameWrite::SetMissile(t.table, g_throwSkill, t.columns, missile->second, ours) ? 1 : 0;
	}
	return done > 0;
}

// Whatever happens, the key comes up, the skill keys hold what they held, the player's right skill is
// theirs again, and our skill throws nothing until the next press.
struct ThrowInHand {
	std::vector<GameWrite::SkillKeyTable> tables {};
	std::vector<GameWrite::RightSkill>    right {};
	int skill = 0;
	int key = 0;  // pressed: a key, or -1 the right button
	void LetGo() {
		if (key != 0) {
			key > 0 ? PressKey(key, false) : PressRight(false);
			key = 0;
			std::this_thread::sleep_for(std::chrono::milliseconds(120));  // the game has taken the cast
		}
		GameWrite::GiveBack(tables, skill);
		tables.clear();
		GameWrite::RestoreRight(right);
		right.clear();
	}
	~ThrowInHand() {
		try {
			LetGo();
			for (int i = 0; i < 60 && Game::Hero().mode == CastMode; ++i) {  // the missile leaves during the cast
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(80));
			LoadMissile("none");
		} catch (...) {
		}
	}
};

auto HaveSkill(const std::vector<uintptr_t>& copies, int skill) -> bool {
	return copies.size() >= 2 && std::all_of(copies.begin(), copies.end(), [&](uintptr_t copy) {
		const auto places = Game::SkillsOf(copy);
		return places && places->Find(skill) != 0;
	});
}

// What the hero's copies know of a skill, for the log.
auto SkillReport(const std::vector<uintptr_t>& copies, int skill) -> std::string {
	std::string text;
	for (const uintptr_t copy : copies) {
		const auto places = Game::SkillsOf(copy);
		int right = -1;
		for (const auto& [id, node] : places ? places->skills : std::vector<std::pair<int, uintptr_t>> {}) {
			right = node == places->right ? id : right;
		}
		text += places ? " [" + std::to_string(places->skills.size()) + " skills, right " + std::to_string(right) + (places->Find(skill) != 0 ? ", has it]" : ", lacks it]") : " [no list]";
	}
	return text.empty() ? " none" : text;
}

// Waits for the hero to start casting; `modes` is what was seen.
auto WaitForCast(uint32_t before, std::string& modes) -> bool {
	bool wasOther = before != CastMode;  // an earlier cast still going on is not ours
	uint32_t last = before;
	modes = std::to_string(before);
	for (int i = 0; i < 40; ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		const uint32_t mode = Game::Hero().mode;
		if (mode != last) {
			modes += ">" + std::to_string(mode);
			last = mode;
		}
		if (wasOther && mode == CastMode) {
			return true;
		}
		wasOther = wasOther || mode != CastMode;
	}
	return false;
}

std::atomic<bool> g_dumped { false };  // the skill list's surroundings are in the log (once a run)

// Returns true when a potion left the hand.
auto ThrowAtCursor(const D2RL::PluginContext* context, const std::string& code) -> bool {
	const auto fail = [&](const std::string& why) {
		ToUi(context, "Throw: " + why, false);
		return false;
	};
	const int skill = g_throwSkill;
	const int area = Game::World().area;
	if (area == 1 || area == 40 || area == 75 || area == 103 || area == 109) {
		return fail("not in town.");
	}
	if (g_bag.load() != 0 || !CursorOverWorld()) {
		return fail(Note(context, "bag %d, cursor over the world %d; nothing done", g_bag.load(), CursorOverWorld() ? 1 : 0));
	}
	const auto copies = HeroCopies(context);
	const std::string hero = SkillReport(copies, skill);
	if (hero.find("no list") != std::string::npos && !g_dumped.exchange(true)) {
		for (const auto& line : Game::SkillSearchDump(Game::Hero().address)) {
			Note(context, "search %s", line.substr(0, 640).c_str());
		}
	}
	if (hero.find("lacks it") != std::string::npos) {
		return fail(Note(context, "the hero does not have skill %d (Throw Potion):%s", skill, hero.c_str()));
	}
	ThrowInHand hand;  // from here on everything is put back
	hand.skill = skill;
	if (!LoadMissile(code)) {
		return fail(Note(context, "%s could not be put in skill %d; tables:%s", code.c_str(), skill, g_tablesSeen.c_str()));
	}
	const auto keys = KeyboardSkillKeys();
	// Where the skill keys are in memory, and what they hold.
	std::vector<int> onKeys;
	const auto look = [&] {
		for (const uintptr_t at : g_keyTables) {
			const auto held = GameWrite::SkillKeysHeld(at);
			onKeys = onKeys.empty() ? held : onKeys;
		}
	};
	look();
	if (onKeys.empty() && Clock::now() - g_keyScan >= std::chrono::milliseconds(RescanMs)) {
		std::vector<std::string> misses;
		g_keyTables = GameWrite::FindSkillKeys(&misses);
		g_keyScan = Clock::now();
		Note(context, "%d skill key table(s) in memory, %d near miss(es)", static_cast<int>(g_keyTables.size()), static_cast<int>(misses.size()));
		for (const auto& miss : misses) {
			Note(context, "near miss %s", miss.c_str());
		}
		look();
	}
	const bool inMemory = !onKeys.empty();
	if (!inMemory && !SavesFolder().empty()) {
		onKeys = Potions::SkillsOnKeys(Camp::ReadFile(SavesFolder() / (Campfire::RouteProgressFile().stem().string() + ".ctl")));
	}
	std::string modes;
	const auto press = [&](int key, const char* how) {
		const uint32_t before = Game::Hero().mode;
		const bool already = IsDown(key);  // the potion slot's own key is the skill key too: the game casts by itself
		if (!already) {
			hand.key = key;
			PressKey(key, true);
		}
		const bool cast = WaitForCast(before, modes);
		Note(context, "%s by key 0x%02x (%s%s): modes %s, hero%s: %s", code.c_str(), key, how, already ? ", already down" : "", modes.c_str(),
			SkillReport(copies, skill).c_str(), cast ? "cast" : "no cast");
		return cast;
	};
	// The player has Throw Potion on a skill key: that one.
	for (const auto& [number, key] : keys) {
		if (static_cast<std::size_t>(number) < onKeys.size() && onKeys[static_cast<std::size_t>(number)] == skill) {
			if (press(key, inMemory ? "the player's" : "the player's, from the file")) {
				return true;
			}
			hand.LetGo();
			break;
		}
	}
	// Otherwise any skill key, which holds it for a moment.
	const auto free = std::find_if(keys.rbegin(), keys.rend(), [](const auto& k) { return k.first < 8 && !IsDown(k.second); });
	if (inMemory && free != keys.rend()) {
		for (const uintptr_t copy : copies) {
			const auto places = Game::SkillsOf(copy);
			for (const auto& [id, node] : places ? places->skills : std::vector<std::pair<int, uintptr_t>> {}) {
				if (node == places->right && id != skill) {
					hand.right.push_back({ copy, id, skill });
				}
			}
		}
		hand.tables = GameWrite::HoldSkill(g_keyTables, skill);
		if (!hand.tables.empty() && press(free->second, "held for a moment")) {
			return true;
		}
		hand.LetGo();
	}
	// The right button, when the hero's skill list is known and the player isn't holding it.
	if (HaveSkill(copies, skill) && g_bag.load() == 0 && CursorOverWorld() && !IsDown(VK_RBUTTON)) {
		hand.right = GameWrite::SelectRight(copies, skill);
		if (!hand.right.empty()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(30));
			const uint32_t before = Game::Hero().mode;
			hand.key = -1;
			PressRight(true);
			const bool cast = WaitForCast(before, modes);
			Note(context, "%s by the right button: modes %s, hero%s: %s", code.c_str(), modes.c_str(), SkillReport(copies, skill).c_str(), cast ? "cast" : "no cast");
			if (cast) {
				return true;
			}
		}
	}
	Note(context, "%s not thrown: %d keyboard skill keys, keys in memory %d, hero%s, tables:%s", code.c_str(), static_cast<int>(keys.size()), inMemory ? 1 : 0, hero.c_str(), g_tablesSeen.c_str());
	return fail("nothing was thrown, no charge taken. Put the skill Throw Potion on a skill key once and try again.");
}

// "dd-potions throwtest": a throw for no charge, in time to close the console and point at the ground.
void ThrowTest(const D2RL::PluginContext* context, std::string code) noexcept {
	try {
		std::this_thread::sleep_for(std::chrono::seconds(4));
		if (g_inGame.load()) {
			ThrowAtCursor(context, code);
		}
	} catch (...) {
	}
	g_throwing = false;
}

void Throw(const D2RL::PluginContext* context, int kind, std::string code) noexcept {
	try {
		if (ThrowAtCursor(context, code)) {
			const Lock lock(g_mutex);
			g_state.Use(kind, g_owner);
			QueueHud(context);
		}
	} catch (...) {
		context->LogWarn("Potions: a throw failed.");
	}
	g_throwing = false;
}

void Drink(const D2RL::PluginContext* context, int slot) noexcept {
	try {
		const Game::HeroDetails hero = Game::HeroInfo();
		if (!g_inGame.load() || !hero.unit || hero.dead || hero.life <= 0) {
			return;
		}
		const Lock lock(g_mutex);
		const int kind = g_state.slots[static_cast<size_t>(slot)];
		if (Potions::State::Pointless(kind, hero.life, hero.maxLife, hero.mana, hero.maxMana)) {
			return;
		}
		if (Potions::Thrown(kind) && !g_toBag.load()) {
			if (g_state.Ready(kind, g_owner) && !g_throwing.exchange(true)) {
				try {
					std::thread(Throw, context, kind, std::string(Potions::PotFor(kind, g_owner).code)).detach();
				} catch (...) {
					g_throwing = false;
				}
			}
			return;
		}
		if (!g_state.Use(kind, g_owner)) {
			return;
		}
		const auto pour = [&](uint16_t stat, uint16_t max, const Potions::Grade& grade, double factor) {
			g_restores.push_back({ stat, max, grade.amount * factor / grade.seconds, grade.amount * factor });
		};
		if (kind == Potions::Heal) {
			pour(Game::StatId::Life, Game::StatId::MaxLife, Potions::GradeAt(Potions::HealGrades, g_owner.level), Potions::HealFactor(hero.classId));
		} else if (kind == Potions::Mana) {
			pour(Game::StatId::Mana, Game::StatId::MaxMana, Potions::GradeAt(Potions::ManaGrades, g_owner.level), Potions::ManaFactor(hero.classId));
		} else if (kind == Potions::Rejuv) {
			g_rejuvenate = (std::max)(g_rejuvenate, g_state.blend == 2 ? 1.0 : 0.35);
		} else if (Potions::Support(kind)) {
			StartWard(kind, 1);
		} else if (g_toBag.load()) {
			g_threads->runOnGameThread(context, MakePots, reinterpret_cast<void*>(static_cast<intptr_t>(kind)));
		}
		for (const int echo : g_state.Echoes(kind, g_owner)) {
			StartWard(echo, Potions::EchoStrength);
		}
		Log(context, "Potions: %s used, %.1f charges left (life %d of %d, mana %d of %d).", Potions::KindIds[kind], g_state.Charge(kind), hero.life, hero.maxLife, hero.mana, hero.maxMana);
		QueueHud(context);
	} catch (...) {
		context->LogWarn("Potions: using a potion failed.");
	}
}

// Ticker thread: what works over time.
void Work(const D2RL::PluginContext* context, double seconds) {
	struct Gain { uint16_t stat, max; double points; };
	std::vector<Gain> gains;
	std::vector<int>  wards;
	double rejuvenate = 0;
	{
		const Lock lock(g_mutex);
		if (g_restores.empty() && g_wards.empty() && g_rejuvenate <= 0) {
			return;
		}
	}
	const Game::HeroDetails hero = Game::HeroInfo();
	if (!hero.unit || hero.dead || hero.life <= 0) {
		return;
	}
	const auto copies = HeroCopies(context);  // before anything is poured out: it can be slow
	{
		const Lock lock(g_mutex);
		for (auto& restore : g_restores) {
			const double points = (std::min)(restore.left, restore.perSecond * seconds);
			restore.left -= points;
			gains.push_back({ restore.stat, restore.max, points });
		}
		std::erase_if(g_restores, [](const Restore& r) { return r.left <= 1e-6; });
		std::erase_if(g_wards, [](const Ward& w) { return w.end <= Clock::now(); });
		for (const auto& ward : g_wards) {
			wards.push_back(ward.kind);
		}
		rejuvenate = std::exchange(g_rejuvenate, 0.0);
	}
	int written = 0;
	static double s_rest[Game::StatId::MaxStamina + 1] {};  // the parts of a 256th still to be given
	for (const auto& gain : gains) {
		double& rest = s_rest[gain.stat];
		rest += gain.points * 256;
		const int whole = static_cast<int>(rest);
		rest -= whole;
		written += GameWrite::AddStat(copies, gain.stat, gain.max, whole, 0);
	}
	if (rejuvenate > 0) {
		written += GameWrite::AddStat(copies, Game::StatId::Life, Game::StatId::MaxLife, 0, rejuvenate);
		written += GameWrite::AddStat(copies, Game::StatId::Mana, Game::StatId::MaxMana, 0, rejuvenate);
	}
	for (const int kind : wards) {
		// End frame 1 is long past: the game ends the state at its next look.
		if (kind == Potions::Antidote) {
			GameWrite::SetStateEnd(copies, Poison, 1);
		} else if (kind == Potions::Thawing) {
			GameWrite::SetStateEnd(copies, Cold, 1);
			GameWrite::SetStateEnd(copies, Freeze, 1);
		} else if (kind == Potions::Stamina) {
			GameWrite::AddStat(copies, Game::StatId::Stamina, Game::StatId::MaxStamina, 0, 1.0);
		}
	}
	if (!gains.empty() || rejuvenate > 0) {
		g_failedWrites = written > 0 ? 0 : g_failedWrites.load() + 1;
		if (g_failedWrites == 5) {
			Log(context, "Potions: the hero's life and mana could not be changed (%d copies).", static_cast<int>(copies.size()));
		}
	}
}

// Not from under g_mutex.
void LookAtOwner(const D2RL::PluginContext* context) {
	auto nodes = StashGame::Nodes();
	GameState::Ready();
	const int level = Game::HeroInfo().level;
	g_threads->runOnGameThread(context, LookAtBelt, nullptr);
	const Lock lock(g_mutex);
	g_owner.nodes = std::move(nodes);
	g_owner.belt = g_belt.load();
	if (level > 0) {
		g_owner.level = level;
	}
}

void Ticker(const D2RL::PluginContext* context) noexcept {
	auto last = Clock::now(), hud = last, owner = last, saved = last;
	for (;;) {
		std::this_thread::sleep_for(std::chrono::milliseconds(TickMs));
		const auto now = Clock::now();
		const double seconds = (std::min)(1.0, std::chrono::duration<double>(now - last).count());
		last = now;
		const auto due = [&](Clock::time_point& since, int ms) {
			if (now - since < std::chrono::milliseconds(ms)) {
				return false;
			}
			since = now;
			return true;
		};
		if (!g_inGame.load()) {
			continue;
		}
		try {
			if (due(owner, OwnerMs)) {
				LookAtOwner(context);
			}
			{
				const Lock lock(g_mutex);
				g_state.Tick(seconds, g_owner);
			}
			Work(context, seconds);
			if (due(hud, HudMs)) {
				QueueHud(context);
			}
			if (due(saved, SaveMs)) {
				const Lock lock(g_mutex);
				Save(context);
			}
		} catch (...) {
		}
	}
}

// ---- Route map pages -----------------------------------------------------------------------------
auto Labels() -> const std::map<std::string, std::string>& {
	static const std::map<std::string, std::string> labels = [] {
		std::map<std::string, std::string> out;
		for (const auto& l : RouteNodeLabels) {
			out[l.node] = l.label;
		}
		return out;
	}();
	return labels;
}

// ---- Keys, buttons, console ---------------------------------------------------------------------
void __cdecl DrinkSlot(const D2RL::PluginContext* context, void* data) noexcept {
	Drink(context, static_cast<int>(reinterpret_cast<intptr_t>(data)));
}

auto __cdecl OnKey(const D2RL::PluginContext* context, const D2RL::Input::ActionEvent* event, void* slot) noexcept -> D2RL::Input::ActionResult {
	if (context == nullptr || event == nullptr || event->kind != D2RL::Input::ActionEventKind::Pressed) {
		return D2RL::Input::ActionResult::Ignored;
	}
	g_threads->runOnUiThread(context, DrinkSlot, slot);
	return D2RL::Input::ActionResult::Handled;
}

void OnAction(const D2RL::PluginContext* context, const std::string& action) {
	char kind[16] {};
	int slot = 0;
	if (std::sscanf(action.c_str(), "use-%d", &slot) == 1 && slot >= 1 && slot <= Potions::SlotCount) {
		Drink(context, slot - 1);
	} else if (std::sscanf(action.c_str(), "pick-%d", &slot) == 1 && slot >= 1 && slot <= Potions::SlotCount) {
		g_picking = g_picking.load() == slot - 1 ? -1 : slot - 1;
	} else if (std::sscanf(action.c_str(), "equip-%d-%15s", &slot, kind) == 2) {
		const Lock lock(g_mutex);
		const int wanted = Potions::KindFromId(kind);
		if ((wanted != Potions::None || std::strcmp(kind, "none") == 0) && g_state.Equip(slot - 1, wanted, g_owner)) {
			Save(context);
		}
		g_picking = -1;
	}
	QueueHud(context);
}

// "dd-potions": what the hero has. Tests: "dd-potions fill", "give <kind>", "concoct", "full", "use <1-4>".
auto PotionsCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	try {
		const auto* context = command->plugin;
		if (!g_inGame.load()) {
			context->WriteConsoleMessage("Potions: no game.");
			return D2RL::ConsoleCommandResult::Failed;
		}
		std::istringstream in(command->args != nullptr ? command->args : "");
		std::string what, value;
		in >> what >> value;
		if (what == "throwtest" && Potions::ThrowingOn) {
			std::string code;
			{
				const Lock lock(g_mutex);
				code = Potions::PotFor(value == "gas" ? Potions::Gas : Potions::Fire, g_owner).code;
			}
			if (!g_throwing.exchange(true)) {
				std::thread(ThrowTest, context, code).detach();
			}
			return D2RL::ConsoleCommandResult::Handled;
		}
		if (what == "use") {
			g_threads->runOnUiThread(context, DrinkSlot, reinterpret_cast<void*>(static_cast<intptr_t>(std::clamp(std::atoi(value.c_str()), 1, Potions::SlotCount) - 1)));
			return D2RL::ConsoleCommandResult::Handled;
		}
		const Lock lock(g_mutex);
		const int kind = Potions::KindFromId(value);
		if (what == "bag") {
			g_toBag = !g_toBag.load();
			context->WriteConsoleMessage(g_toBag.load() ? "Throwing potions go to the bag." : "Throwing potions are thrown from the slot.");
		} else if (what == "fill") {
			g_state.Fill(g_owner);
		} else if (what == "give" && Potions::Support(kind)) {
			g_state.Give(kind, g_owner);
		} else if (what == "concoct") {
			g_state.Concoct(1, g_owner);
		} else if (what == "full") {
			g_state.Concoct(2, g_owner);
		}
		Save(context);
		std::string text = "Potions: level " + std::to_string(g_owner.level) + ", belt +" + std::to_string(g_owner.belt) + ".";
		for (int k = 0; k < Potions::KindCount; ++k) {
			if (g_state.Owns(k, g_owner)) {
				char part[64] {};
				std::snprintf(part, sizeof(part), " %s %.1f/%.1f", Potions::KindIds[k], g_state.Charge(k), g_state.Capacity(k, g_owner));
				text += part;
			}
		}
		for (const auto& [affix, stop] : g_state.affixes) {
			text += " [" + affix + " at " + stop + "]";
		}
		context->WriteConsoleMessage(text.c_str());
		Log(context, "%s", text.c_str());
		QueueHud(context);
		return D2RL::ConsoleCommandResult::Handled;
	} catch (...) {
		return D2RL::ConsoleCommandResult::Failed;
	}
}

void LoadHero(const D2RL::PluginContext* context) {
	const fs::path progress = Campfire::RouteProgressFile();
	if (progress.empty()) {
		return;
	}
	const Route::Progress route = Route::Load(progress);
	LookAtOwner(context);
	const Lock lock(g_mutex);
	g_file = progress.parent_path().parent_path() / "potions" / (progress.stem().string() + ".potions.txt");
	std::error_code ec;
	const bool known = fs::exists(g_file, ec);
	g_state = known ? Potions::Parse(Camp::ReadFile(g_file)) : Potions::State {};
	if (!known) {
		g_state.Fill(g_owner);
	}
	g_state.Roll(route, g_rng, g_owner);
	g_restores.clear();
	g_wards.clear();
	g_rejuvenate = 0;
	Save(context, true);
	Log(context, "Potions: %s (%s), %d affix(es) to win.", g_file.filename().string().c_str(), known ? "read" : "new", static_cast<int>(g_state.affixes.size()));
}

// UI thread, when a conversation opens: the healers who restore life and mana fill the potions too.
void FillAtHealer(const D2RL::PluginContext* context) noexcept {
	static constexpr uint32_t Healers[] = { 148, 178, 255, 405, 513 };  // monstats: Akara, Fara, Ormus, Jamella, Malah
	constexpr int Reach = 12;  // sub-tiles, as for Meshif (darkestdiablo_plugin.cpp)
	try {
		for (const Game::MonsterDetails& m : Game::Monsters(Reach, true)) {
			if (Game::ClassOf(m.unit.row) != Game::MonsterClass::Npc) {
				continue;
			}
			// Nearest first: the one being talked to.
			if (std::find(std::begin(Healers), std::end(Healers), m.unit.row) != std::end(Healers)) {
				Refill(context);
				Log(context, "Potions: filled by a healer (%u).", m.unit.row);
			}
			return;
		}
	} catch (...) {
	}
}

}  // namespace

auto Load(const D2RL::PluginContext* context) noexcept -> bool {
	try {
		if (context->QueryService(&g_threads) != D2RL::ServiceQueryResult::Success || !D2RL::HasThreadServiceField(g_threads, D2RL::ThreadServiceRequiredSize) ||
			context->QueryService(&g_resources) != D2RL::ServiceQueryResult::Success || !D2RL::HasResourceServiceField(g_resources, D2RL::ResourceServiceRequiredSize) ||
			context->QueryService(&g_panels) != D2RL::ServiceQueryResult::Success || !D2RL::HasPanelServiceField(g_panels, D2RL::PanelServiceRequiredSize) ||
			context->QueryService(&g_inventory) != D2RL::ServiceQueryResult::Success || !D2RL::HasInventoryServiceField(g_inventory, D2RL::InventoryServiceRequiredSize) ||
			context->QueryService(&g_items) != D2RL::ServiceQueryResult::Success || !D2RL::HasItemServiceField(g_items, D2RL::ItemServiceRequiredSize)) {
			g_threads = nullptr;
			context->LogWarn("Potions: a service is missing; no potions.");
			return true;
		}
		if (context->QueryService(&g_widgets) != D2RL::ServiceQueryResult::Success || !D2RL::HasWidgetServiceField(g_widgets, D2RL::WidgetServiceRequiredSize)) {
			g_widgets = nullptr;
		}
		for (int i = 0; i < RowSize; ++i) {
			const std::string id = "PotionHud" + std::to_string(i);
			const D2RL::Panels::PanelRegistration registration {
				.structSize = D2RL::Panels::PanelRegistrationSize,
				.flags      = D2RL::Panels::PanelFlags::None,
				.localId    = id.c_str(),
			};
			if (g_panels->registerPanel(context, &registration, &g_hud[i]) != D2RL::Panels::Result::Success) {
				Log(context, "Potions: could not register the %s panel.", id.c_str());
				break;
			}
			++g_hudCount;
		}
		const D2RL::InputService* input = nullptr;
		if (context->QueryService(&input) == D2RL::ServiceQueryResult::Success && D2RL::HasInputServiceField(input, D2RL::InputServiceRequiredSize)) {
			static constexpr D2RL::Input::Key Keys[] = { D2RL::Input::Key::Number1, D2RL::Input::Key::Number2, D2RL::Input::Key::Number3, D2RL::Input::Key::Number4 };
			for (int i = 0; i < Potions::SlotCount; ++i) {
				const std::string id = "potion-" + std::to_string(i + 1), name = "Use potion " + std::to_string(i + 1);
				const D2RL::Input::ActionRegistration action {
					.structSize       = D2RL::Input::ActionRegistrationSize,
					.logicalId        = id.c_str(),
					.displayName      = name.c_str(),
					.category         = "Darkest Diablo",
					.defaultPrimary   = { .key = Keys[i], .modifier = D2RL::Input::Modifier::None },
					.defaultSecondary = { .key = D2RL::Input::Key::None },
					.callback         = OnKey,
					.userData         = reinterpret_cast<void*>(static_cast<intptr_t>(i)),
				};
				D2RL::Input::ActionHandle handle = D2RL::Input::InvalidHandle;
				const auto result = input->registerAction(context, &action, &handle);
				D2RL::Input::Binding binding {};
				if (result == D2RL::Input::Result::Success) {
					(void)input->getBinding(context, handle, D2RL::Input::BindingSlot::Primary, &binding);
				}
				Log(context, "Potions: key for slot %d: code %d, key 0x%02x.", i + 1, static_cast<int>(result), static_cast<unsigned>(binding.key));
			}
		}
		std::thread(Ticker, context).detach();
		Log(context, "Potions: ready (%d panels).", g_hudCount);
		return context->RegisterConsoleCommand("dd-potions", PotionsCommand, "Show the hero's potions. Tests: dd-potions fill, give <antidote|thawing|stamina>, concoct, full, bag, use <1-4>, throwtest [fire|gas].");
	} catch (...) {
		g_threads = nullptr;
		context->LogWarn("Potions: setting up failed; no potions.");
		return true;
	}
}

auto OnUiMessage(const D2RL::PluginContext* context, const char* target, const char* command, const char* text) noexcept -> bool {
	if (g_threads == nullptr) {
		return false;
	}
	if (std::strncmp(target, "PlayerInventoryPanel", 20) == 0 && std::strcmp(command, "Close") == 0) {
		g_bag = 0;
	}
	if (std::strcmp(target, "PanelManager") != 0) {
		return false;
	}
	if (std::strncmp(text, "PlayerInventory", 15) == 0) {
		if (std::strcmp(command, "OpenPanel") == 0) {
			g_bag = 1;
		} else if (std::strcmp(command, "ClosePanel") == 0 || std::strcmp(command, "UnloadPanel") == 0) {
			g_bag = 0;
		}
	}
	if (std::strcmp(command, "OpenPanel") != 0) {
		return false;
	}
	if (std::strcmp(text, "NpcDialogPanel") == 0 && g_inGame.load()) {
		FillAtHealer(context);
		return false;
	}
	if (std::strncmp(text, ActionPrefix, sizeof(ActionPrefix) - 1) != 0) {
		return false;
	}
	try {
		OnAction(context, text + sizeof(ActionPrefix) - 1);
	} catch (...) {
		context->LogWarn("Potions: a button failed.");
	}
	return true;
}

void OnGameplayEvent(const D2RL::PluginContext* context, const D2RL::Lifecycle::GameplayEvent* event) noexcept {
	using Kind = D2RL::Lifecycle::GameplayEventKind;
	if (g_threads == nullptr) {
		return;
	}
	try {
		if (event->kind == Kind::LocalPlayerReady) {
			g_picking = -1;
			g_bag = 0;
			LoadHero(context);
			g_inGame = !g_file.empty();
			if (Potions::ThrowingOn) {
				g_threads->runOnGameThread(context, FindThrowTables, nullptr);
			}
			QueueHud(context);
		} else if (event->kind == Kind::GameLeft) {
			g_inGame = false;
			{
				const std::lock_guard<std::mutex> guard(g_tablesMutex);  // the tables go with the game
				g_throwTables.clear();
			}
			Game::SetSkillTables({});
			{
				const Lock lock(g_mutex);
				Save(context);
				g_file.clear();
				g_restores.clear();
				g_wards.clear();
				g_rejuvenate = 0;
			}
			g_picking = -1;
			QueueHud(context);
		}
	} catch (...) {
		context->LogWarn("Potions: reading the hero's potions failed.");
	}
}

void OnMonsterDeath(const D2RL::Lifecycle::MonsterDeathEvent* event) noexcept {
	if (g_threads == nullptr || !g_inGame.load()) {
		return;
	}
	try {
		if (Game::ClassOf(event->monster.classId) != Game::MonsterClass::Enemy) {
			return;
		}
		Potions::Rank rank = Potions::Rank::Normal;
		int level = -1;
		for (const Game::MonsterDetails& m : Game::Monsters(-1, false)) {
			if (m.unit.id == event->monster.id && m.unit.row == event->monster.classId) {
				rank = m.unique || m.superUnique || m.minion ? Potions::Rank::Unique : m.champion ? Potions::Rank::Champion : Potions::Rank::Normal;
				level = Game::Stat(m.unit, Game::StatId::Level, 0, false).value_or(-1);
				break;
			}
		}
		const Lock lock(g_mutex);
		g_state.Kill(rank, level, g_owner);
	} catch (...) {
	}
}

void SetThrowData(const std::string& kind, const std::string& name, int number) {
	if (kind == "throwskill") {
		g_throwSkill = number;
	} else if (kind == "throwmissile") {
		g_throwMissiles[name] = number;
	} else {
		g_throwRefs.emplace_back(std::atoi(name.c_str()), number);
	}
}

void OnStopCleared(const D2RL::PluginContext* context, const char* stop) noexcept {
	if (g_threads == nullptr || !g_inGame.load()) {
		return;
	}
	try {
		const Lock lock(g_mutex);
		if (const Potions::Affix* affix = g_state.Cleared(stop, g_owner); affix != nullptr) {
			Save(context);
			const std::string id = affix->id;
			const std::string text = id == "concoct" || id == "full" ? "Healing and mana are one potion now: " + Potions::Name(Potions::Rejuv, g_state, g_owner) + "."
				: "You found the " + std::string(affix->label) + " Potion. The arrows over the potion slots choose what goes where.";
			context->WriteConsoleMessage(text.c_str());
			Log(context, "Potions: %s cleared, affix %s.", stop, affix->id);
			QueueHud(context);
		}
	} catch (...) {
		context->LogWarn("Potions: a stop's affix failed.");
	}
}

void Refill(const D2RL::PluginContext* context) noexcept {
	if (g_threads != nullptr && g_inGame.load()) {
		const Lock lock(g_mutex);
		g_state.Fill(g_owner);
		QueueHud(context);
	}
}

auto RoutePage(const std::string& page, int act) -> std::string {
	const Lock lock(g_mutex);
	return Potions::WithAffixes(page, act, g_state, Labels());
}

auto RouteLine(int act) -> std::string {
	const Lock lock(g_mutex);
	return Potions::AffixLine(g_state, act, Labels());
}

}
