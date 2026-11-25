// Darkest Diablo — the Campfire (see campfire.h, camp.h and docs/campfire.md).
//
// How each part reaches the game:
// - Save slots: a plugin panel over the main menu at launch. Picking another slot moves the save
//   files (camp.h, SwitchSlot) and asks the menu to load again.
// - The Campfire is the game's hero screen (the eight classes around the fire). The game's own
//   figures stay for classes that can be made right now; the rest are hidden. The camp's living
//   heroes are drawn by a panel of ours laid over it ("Fire"), whose layout is written each time
//   from the camp file, so every hero stands there in the gear it was last seen in.
// - Clicking a hero loads it: the other heroes' files wait on the bench (camp.h), the main menu
//   loads with that one hero in its list, and the plugin presses the menu's Play.
// - The game's character list is only passed through. The camp is kept in line with the save
//   files every time the menu loads (camp.h, Reconcile).
// - Death: the game's "You died" window or a resurrection marks the hero as fallen. A softcore
//   hero is sent out of the game at once. Back at the menu the files go to the graveyard.
// - The traveler: a chance each time a hero comes back to town; a panel offers up to 3 classes.
// - In town, using the bonfire (a stash chest to the game) goes to the Campfire; the real stash
//   chest still opens the stash. The hero's distance to the fire tells them apart (gamememory.h).
//
// Console (Ctrl + `): dd-camp, dd-slot <1-3>, dd-traveler.

#include "campfire.h"
#include "stashfile.h"
#include "modernstash.h"
#include "ascension.h"
#include "legacy.h"
#include "camp.h"
#include "gamememory.h"
#include "gamestate.h"
#include "gameapi.h"
#include "gamewrite.h"
#include "route.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <exception>
#include <mutex>
#include <sstream>
#include <thread>

namespace Campfire {

namespace {

namespace fs = std::filesystem;
using Lock = std::lock_guard<std::recursive_mutex>;

constexpr char PluginId[]      = "darkestdiablo";
constexpr char TravelerPanel[] = "Traveler";
constexpr char CampPanel[]     = "Campfire";
constexpr char CurtainPanel[]  = "Curtain";
constexpr char CurtainLayout[] = "data/global/ui/layouts/darkestdiablo/Curtainhd.json";
// A black screen over the game's main menu while it is only being passed through.
constexpr char CurtainJson[] = "{\"type\":\"Panel\",\"name\":\"darkestdiablo/Curtain\",\"fields\":{\"anchor\":{\"x\":0.5},\"fitToParent\":true,\"priority\":9500},"
	"\"children\":[{\"type\":\"RectangleWidget\",\"name\":\"Black\",\"fields\":{\"anchor\":{\"x\":-0.5},\"fitToScreen\":true,\"color\":[0.0,0.0,0.0,1.0]},"
	"\"children\":[{\"type\":\"ClickCatcherWidget\",\"name\":\"catcher\",\"fields\":{\"fitToParent\":true}}]}]}";
constexpr char ActionPrefix[]  = "darkestdiablo/";  // buttons send PanelManager:OpenPanel:darkestdiablo/<action>

const D2RL::ThreadService* g_threads = nullptr;
const D2RL::PanelService*  g_panels  = nullptr;
const D2RL::WidgetService* g_widgets = nullptr;
const D2RL::ResourceService*  g_resources = nullptr;
const D2RL::InventoryService* g_inventory = nullptr;
const D2RL::ItemService*      g_items     = nullptr;
// A panel whose content changes while the game runs (who is by the fire, what each save holds).
// The game keeps a panel it has shown and does not read its layout again, so each new content
// goes to the next of a row of spare panels ("Fire0", "Fire1", ...) that has not been shown yet.
struct LivePanel {
	const char* base;
	int count;
	std::vector<D2RL::Panels::RegistrationHandle> panels;
	std::vector<D2RL::Resources::RegistrationHandle> layouts;
	std::vector<bool> shown;
	int current = -1;
	std::string content;
};
// Few spares, used over and over: a panel that was replaced is forgotten by the game at once, so it
// reads the new layout when its turn comes again. (Until 0.23.0 parts were shown and hidden by name
// instead, with 14 + 6 spares. After a few changes of save nothing could be hidden any more, as if
// the loader only looks up so many widgets a session: every name, glow, retire button and the restart
// notice showed at once. So what changes is now drawn anew, and almost nothing is hidden.)
LivePanel g_fire  { "Fire", 4 };     // the figures and the corner buttons
LivePanel g_top   { "FireTop", 3 };  // names, glow, Enter, the retire buttons: drawn anew for every state
LivePanel g_slots { "SaveSlots", 3 };
constexpr char PanelNameMark[] = "@PANEL@";
D2RL::Panels::RegistrationHandle g_travelerPanel = D2RL::Panels::InvalidHandle;
D2RL::Panels::RegistrationHandle g_campPanel     = D2RL::Panels::InvalidHandle;

std::recursive_mutex g_mutex;   // guards everything below
fs::path     g_live;            // the save folder the game reads
int          g_slot = 1;
Camp::State  g_camp;
std::mt19937 g_rng { std::random_device {}() };

bool g_slotPicker     = true;   // camp/options.txt: "slot_picker 0" turns the launch screen off
int  g_travelerChance = 25;     // camp/options.txt: "traveler_chance <0-100>", percent per return to town

bool g_slotChosen    = false;   // the slot screen was answered this launch
bool g_awaitingMenu  = false;   // asked the menu to load again after a slot change
int  g_stashSlot     = 0;       // the save whose shared stash the game holds in memory (0: no game played yet; see camp.h, Pending)
bool g_openedAtStart = false;   // a save change waiting from the last session was made at this start: go straight to its Campfire
bool g_inGame        = false;
int  g_levelId       = -1;
constexpr int RogueEncampment = 1;  // levels.txt Id
int  g_selectedClass = -1;      // on the hero screen
int  g_pendingClass  = -1;      // Create was pressed for this class; its save has not been seen yet
int  g_pickedClass   = -1;      // the class last clicked on the hero screen: pressing Enter in the name box makes a hero without a Create message
bool g_died          = false;   // the hero in the current game died
int  g_reachedLevel  = 0;       // character level reported during the current game
fs::file_time_type g_joinTime {};

bool        g_fireOn      = true;   // camp/options.txt: "campfire_screen 0" goes back to the game's character list
bool        g_fireReady   = false;  // the Fire panel's layout is registered
bool        g_wantFire    = false;  // when the main menu has loaded, go on to the Campfire
bool        g_deleteMode  = false;  // Delete was pressed: the next hero clicked is buried
std::string g_pendingPlay;          // the hero to load once the main menu is up
std::string g_currentHero;          // the hero in the current game ("" when unknown or at the menu)
std::string g_gearNow;              // what the hero in the current game wears (see RecordGear)

struct Spot { int x = 0, y = 0, w = 0, h = 0; std::string px, py, pz, yaw, look; bool known = false; };
bool g_atFire    = false;  // the Campfire screen is up
bool g_slotsOpen = false;
bool g_slotNotice = false;  // the slot screen shows only "close the game and start it again"
bool g_createMode = false;  // Create was pressed at the Campfire: the name box is showing
bool g_exiting    = false;  // Exit was pressed at the Campfire: the game passes its menu on the way out
int  g_ascendAsk  = -1;     // Ascend was pressed for this hero: only the question shows
std::string g_ascendedNote;  // at the Campfire after an ascension, until a game starts
std::string g_lootData;
Ascend::Shapes g_shapes;    // how items are stored in saves (ascend.h)
int  g_chosenHero = -1;     // the hero picked at the Campfire; Enter loads it
int  g_hoveredHero = -1;    // the class under the mouse at the Campfire (from the game's Hover messages)
int  g_clickedHero = -1;    // the hero under the mouse when the left button went down (see WatchFireClicks)
std::atomic<bool> g_watchingClicks { false };
bool g_emptyFire = false;       // the hero screen is up with nobody at the fire, under the save slots
bool g_travelerArrives = false; // the fire was used with a traveler waiting: portal and figure are showing
bool g_travelerMode = false;
bool g_travelerLater = false;   // "Not now" at the Campfire: the traveler waits until the next game
int  g_justRecruited = -1;      // Accept was pressed for this class: the Campfire reopens ready to make it // a traveler waits: the Campfire shows only the offered classes until one is accepted
D2RL::Panels::RegistrationHandle g_curtainPanel = D2RL::Panels::InvalidHandle;
fs::path g_dataDir;         // the mod's d2rloader folder: camp_data.txt, SaveSlots.layout.json
int g_confirmSlot = 0;      // the save the player is being asked about deleting (0: none)
int g_confirmStep = 0;      // 1: first question, 2: second question
Spot g_spots[Camp::ClassCount];
struct ItemLook { char kind = 0; std::string value; };
std::vector<std::pair<std::string, ItemLook>> g_looks;  // item code -> how it looks on a figure

// ---- The state log -----------------------------------------------------------------------
// The loader rewrites darkestdiablo.log every time the game starts, so a restart used to wipe
// the results of a dd-state check. Every "State:" and "Write:" line (dd-state, dd-state watch,
// dd-set) also goes to d2rloader\logs\darkestdiablo-state.log, which is only ever added to.
// Over 5 MB at start, it becomes darkestdiablo-state.old.log (replacing the older one).

std::mutex    g_stateLogLock;
std::ofstream g_stateLog;

auto StateLogPath() -> fs::path {
	HMODULE self = nullptr;
	wchar_t path[MAX_PATH] {};
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&StateLogPath),
	        &self) || GetModuleFileNameW(self, path, MAX_PATH) == 0) {
		return {};
	}
	return fs::path(path).parent_path().parent_path() / "logs" / "darkestdiablo-state.log";  // plugins\..\logs
}

auto LocalStamp() -> std::string {
	SYSTEMTIME t {};
	GetLocalTime(&t);
	char text[32];
	std::snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u:%02u.%03u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
	return text;
}

void StateLogLine(const char* line) noexcept {
	try {
		const std::lock_guard<std::mutex> guard(g_stateLogLock);
		if (g_stateLog.is_open()) {
			g_stateLog << '[' << LocalStamp() << "] " << line << '\n';
			g_stateLog.flush();
		}
	} catch (...) {
	}
}

// Called once when the plugin loads.
void OpenStateLog() noexcept {
	try {
		const fs::path file = StateLogPath();
		if (file.empty()) {
			return;
		}
		std::error_code ec;
		fs::create_directories(file.parent_path(), ec);
		if (fs::exists(file, ec) && fs::file_size(file, ec) > 5u * 1024 * 1024) {
			fs::path old = file;
			old.replace_filename("darkestdiablo-state.old.log");
			fs::remove(old, ec);
			fs::rename(file, old, ec);
		}
		const std::lock_guard<std::mutex> guard(g_stateLogLock);
		g_stateLog.open(file, std::ios::app);
	} catch (...) {
	}
	StateLogLine((std::string("--- session start ") + Campfire::Version + " " + LocalStamp()).c_str());
}

void Log(const D2RL::PluginContext* context, const char* format, ...) noexcept {
	char message[512] {};
	va_list args;
	va_start(args, format);
	std::vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	context->LogInfo(message);
	if (std::strncmp(message, "State:", 6) == 0 || std::strncmp(message, "Write:", 6) == 0) {
		StateLogLine(message);
	}
}

auto IsTown(int levelId) noexcept -> bool {
	return levelId == 1 || levelId == 40 || levelId == 75 || levelId == 103 || levelId == 109;
}

// ---- Camp state --------------------------------------------------------------------------

void ReadOptions() {
	std::istringstream in(Camp::ReadFile(g_live / "camp" / "options.txt"));
	std::string key;
	int value = 0;
	while (in >> key >> value) {
		if (key == "slot_picker") {
			g_slotPicker = value != 0;
		} else if (key == "traveler_chance") {
			g_travelerChance = std::clamp(value, 0, 100);
		} else if (key == "campfire_screen") {
			g_fireOn = value != 0;
		}
	}
}

// The hero played in the game that just ended: the save written last, after the game was joined.
auto PlayedHero() -> std::string {
	std::string name;
	fs::file_time_type newest = g_joinTime;
	std::error_code ec;
	for (const auto& hero : Camp::HeroNames(g_live)) {
		const auto written = fs::last_write_time(g_live / (hero + ".d2s"), ec);
		if (!ec && written >= newest) {
			newest = written;
			name = hero;
		}
	}
	return name;
}

// Brings the camp in line with the save files. `afterGame`: a game just ended, so deaths and
// levels reported during it are applied to the hero that was played.
void Reconcile(const D2RL::PluginContext* context, bool afterGame) {
	Camp::Hints hints;
	// A fight for ascension that was left: lost unless it was won (docs/ascend.md).
	std::string ascender;
	const Legacy::Fight fight = g_inGame ? Legacy::Fight::None : Legacy::ReadFight(g_live, g_slot, ascender);
	if (fight == Legacy::Fight::Won && g_camp.HeroNamed(ascender) != nullptr) {
		Camp::Unbench(g_live, g_slot);
		std::string error;
		const std::string stamp = std::to_string(std::time(nullptr));
		if (Legacy::Keep(g_live, g_slot, ascender, stamp, error) && Camp::Bury(g_live, g_slot, ascender, error)) {
			Camp::MarkFallen(g_camp, ascender);
			Camp::SaveState(g_live, g_slot, g_camp);
			Legacy::WriteFight(g_live, g_slot, Legacy::Fight::None, "");
			g_ascendedNote = ascender + " has ascended. The next hero you play brings the belongings to the stash.";
			Log(context, "Camp: %s has ascended.", ascender.c_str());
		} else {
			Log(context, "Camp: %s has won, but could not ascend yet (%s); tried again next time.", ascender.c_str(), error.c_str());
		}
		g_died = false;
	} else if (fight != Legacy::Fight::None) {
		if (fight == Legacy::Fight::Fighting) {
			Log(context, "Camp: %s did not come back from the arena.", ascender.c_str());
			hints.diedName = ascender;
		}
		Legacy::WriteFight(g_live, g_slot, Legacy::Fight::None, "");
	}
	hints.newClass = g_pendingClass >= 0 ? g_pendingClass : g_pickedClass;
	if (afterGame) {
		const std::string played = PlayedHero();
		if (g_died) {
			hints.diedName = played;
		}
		hints.leveledName = played;
		hints.leveledTo = g_reachedLevel;
	}
	if (afterGame && !g_gearNow.empty()) {
		if (Camp::Hero* hero = g_camp.HeroNamed(hints.leveledName)) {
			hero->gear = g_gearNow;
		}
	}
	g_gearNow.clear();
	const size_t heroesBefore = g_camp.heroes.size();
	for (const auto& line : Camp::Reconcile(g_live, g_slot, g_camp, hints)) {
		Log(context, "Camp: %s", line.c_str());
	}
	if (g_camp.heroes.size() > heroesBefore || !g_inGame) {
		g_pendingClass = -1;
		g_pickedClass = -1;
	}
	if (afterGame) {
		g_died = false;
		g_reachedLevel = 0;
	}
	if (!Camp::SaveState(g_live, g_slot, g_camp)) {
		context->LogWarn("Camp: could not write camp.txt.");
	}
}

auto Describe() -> std::string {
	std::string out = "Save " + std::to_string(g_slot) + ": " + Camp::Summary(g_camp) + ".";
	const auto list = [&](const char* label, const std::vector<int>& classes) {
		if (!classes.empty()) {
			out += std::string(" ") + label + ":";
			for (const int c : classes) {
				out += std::string(" ") + Camp::ClassNames[c];
			}
			out += ".";
		}
	};
	list("Recruits", g_camp.recruits);
	list("Fallen", g_camp.fallen);
	list("Traveler offers", g_camp.offer);
	return out;
}

// ---- UI helpers (UI thread) ---------------------------------------------------------------

void Dispatch(const D2RL::PluginContext* context, const char* target, const char* command, const char* text = "") noexcept {
	if (g_widgets == nullptr) {
		return;
	}
	const D2RL::Widgets::UiAction action {
		.structSize = D2RL::Widgets::UiActionSize,
		.target     = target,
		.command    = command,
		.text       = text,
	};
	const auto result = g_widgets->dispatchUiAction(context, &action);
	Log(context, "Camp: sent %s:%s:%s (code %d).", target, command, text, static_cast<int>(result));
}

// Shows or hides one named widget of a panel. Returns false if it was not found.
auto SetVisible(const D2RL::PluginContext* context, const char* panel, const char* widget, bool visible) noexcept -> bool {
	if (g_widgets == nullptr) {
		return false;
	}
	D2RL::Widgets::WidgetHandle panelHandle = D2RL::Widgets::InvalidHandle;
	D2RL::Widgets::WidgetHandle handle = D2RL::Widgets::InvalidHandle;
	return g_widgets->findPanel(context, panel, &panelHandle) == D2RL::Widgets::Result::Success &&
		g_widgets->findWidget(context, panelHandle, widget, &handle) == D2RL::Widgets::Result::Success &&
		g_widgets->setWidgetVisible(context, handle, visible) == D2RL::Widgets::Result::Success;
}

auto FullName(const char* localId) -> std::string {
	return std::string(PluginId) + "/" + localId;
}

auto Open(const D2RL::PluginContext* context, D2RL::Panels::RegistrationHandle panel, const char* name) noexcept -> bool {
	if (g_panels == nullptr || panel == D2RL::Panels::InvalidHandle) {
		return false;
	}
	const auto result = g_panels->openPanel(context, panel);
	if (result != D2RL::Panels::Result::Success) {
		Log(context, "Camp: could not open the %s panel (code %d).", name, static_cast<int>(result));
		return false;
	}
	return true;
}

void Close(const D2RL::PluginContext* context, D2RL::Panels::RegistrationHandle panel) noexcept {
	if (g_panels != nullptr && panel != D2RL::Panels::InvalidHandle) {
		g_panels->closePanel(context, panel);
	}
}

auto PoolName(const LivePanel& pool) -> std::string {
	return FullName((std::string(pool.base) + std::to_string(pool.current > 0 ? pool.current : 0)).c_str());
}

auto PoolHandle(const LivePanel& pool) -> D2RL::Panels::RegistrationHandle {
	return pool.current >= 0 && pool.current < static_cast<int>(pool.panels.size()) ? pool.panels[pool.current] : D2RL::Panels::InvalidHandle;
}

// Registers the row of spare panels. Only possible while the plugin loads.
void PoolRegister(const D2RL::PluginContext* context, LivePanel& pool, D2RL::Panels::PanelFlags flags) {
	for (int i = 0; i < pool.count; ++i) {
		const std::string id = std::string(pool.base) + std::to_string(i);
		const D2RL::Panels::PanelRegistration registration {
			.structSize = D2RL::Panels::PanelRegistrationSize,
			.flags      = flags,
			.localId    = id.c_str(),
		};
		D2RL::Panels::RegistrationHandle panel = D2RL::Panels::InvalidHandle;
		if (g_panels->registerPanel(context, &registration, &panel) != D2RL::Panels::Result::Success) {
			Log(context, "Camp: could not register the %s panel.", id.c_str());
			break;
		}
		pool.panels.push_back(panel);
		pool.layouts.push_back(D2RL::Resources::InvalidHandle);
		pool.shown.push_back(false);
	}
}

// Gives the pool new content (a layout with PanelNameMark where the panel's name goes). If it
// differs from what the current panel holds, the next spare panel takes it. Returns false if
// no panel could take it.
auto PoolSet(const D2RL::PluginContext* context, LivePanel& pool, std::string json) -> bool {
	if (g_resources == nullptr || pool.panels.empty()) {
		return false;
	}
	if (pool.current >= 0 && json == pool.content) {
		return true;
	}
	if (pool.current >= 0 && !pool.shown[pool.current]) {
		// never shown: the game has not read it yet, so it can simply be replaced
	} else {
		if (pool.current >= 0 && g_panels != nullptr) {
			g_panels->closePanel(context, pool.panels[pool.current]);
		}
		pool.current = (pool.current + 1) % static_cast<int>(pool.panels.size());
	}
	const int i = pool.current;
	const std::string name = PoolName(pool);
	if (pool.shown[i]) {  // every spare has been used: ask the game to forget this one
		Dispatch(context, "PanelManager", "UnloadPanel", name.c_str());
		pool.shown[i] = false;
	}
	pool.content = json;
	for (size_t at = json.find(PanelNameMark); at != std::string::npos; at = json.find(PanelNameMark)) {
		json.replace(at, sizeof(PanelNameMark) - 1, name);
	}
	if (pool.layouts[i] != D2RL::Resources::InvalidHandle) {
		g_resources->unregisterResource(context, pool.layouts[i]);
		pool.layouts[i] = D2RL::Resources::InvalidHandle;
	}
	const std::string path = "data/global/ui/layouts/" + name + "hd.json";
	const D2RL::Resources::ResourceRegistration resource {
		.structSize = D2RL::Resources::ResourceRegistrationSize,
		.path       = path.c_str(),
		.bytes      = json.data(),
		.byteCount  = static_cast<uint32_t>(json.size()),
	};
	const auto result = g_resources->registerResource(context, &resource, &pool.layouts[i]);
	Log(context, "Camp: panel %s takes %d bytes (code %d).", name.c_str(), static_cast<int>(json.size()), static_cast<int>(result));
	return result == D2RL::Resources::Result::Success;
}

auto PoolOpen(const D2RL::PluginContext* context, LivePanel& pool, const char* what) noexcept -> bool {
	if (!Open(context, PoolHandle(pool), what)) {
		return false;
	}
	pool.shown[pool.current] = true;
	return true;
}

void PoolClose(const D2RL::PluginContext* context, const LivePanel& pool) noexcept {
	Close(context, PoolHandle(pool));
	if (&pool == &g_fire) {
		Close(context, PoolHandle(g_top));
	}
}

auto PoolIsOpen(const D2RL::PluginContext* context, const LivePanel& pool) noexcept -> bool {
	D2RL::Panels::PanelInfo info { .structSize = D2RL::Panels::PanelInfoSize };
	const auto panel = PoolHandle(pool);
	return g_panels != nullptr && panel != D2RL::Panels::InvalidHandle && g_panels->getPanelInfo(context, panel, &info) == D2RL::Panels::Result::Success &&
		info.presentationState == D2RL::Panels::PresentationState::Open;
}

// Shows the pool with this content. New content opens in the next panel before the old one closes,
// so nothing blinks; the old one is forgotten at once.
auto PoolSwap(const D2RL::PluginContext* context, LivePanel& pool, const std::string& json, const char* what) -> bool {
	if (pool.current >= 0 && json == pool.content) {
		return PoolIsOpen(context, pool) || PoolOpen(context, pool, what);
	}
	const int old = pool.current;
	const std::string oldName = PoolName(pool);
	const bool oldShown = old >= 0 && pool.shown[old];
	const auto oldPanel = PoolHandle(pool);
	if (oldShown) {  // PoolSet closes the panel it replaces; here that comes after the new one is up
		pool.current = (old + 1) % static_cast<int>(pool.panels.size());
		pool.content.clear();
		if (pool.shown[pool.current]) {
			Dispatch(context, "PanelManager", "UnloadPanel", PoolName(pool).c_str());
			pool.shown[pool.current] = false;
		}
	}
	if (!PoolSet(context, pool, json)) {
		return false;
	}
	const bool opened = PoolOpen(context, pool, what);
	if (oldShown && old != pool.current) {
		Close(context, oldPanel);
		Dispatch(context, "PanelManager", "UnloadPanel", oldName.c_str());
		pool.shown[old] = false;
	}
	return opened;
}

// Runs `callback` on the UI thread after `delayMs`.
void Later(const D2RL::PluginContext* context, int delayMs, D2RL::Threads::Callback callback) noexcept {
	if (g_threads == nullptr) {
		return;
	}
	std::thread([context, delayMs, callback] {
		std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
		g_threads->runOnUiThread(context, callback, nullptr);
	}).detach();
}

void __cdecl GoToFire(const D2RL::PluginContext* context, void*) noexcept;
void RefreshFire(const D2RL::PluginContext* context);

// ---- Save slots ---------------------------------------------------------------------------

auto SlotsJson() -> std::string;

// The save slot screen as it should look now: the three saves, one of the two questions asked before
// a save is deleted, or the restart notice.
void ShowSlotWidgets(const D2RL::PluginContext* context) noexcept {
	try {
		if (const std::string json = g_slotsOpen ? SlotsJson() : std::string(); !json.empty()) {
			PoolSwap(context, g_slots, json, "save slot");
		}
	} catch (...) {
	}
}

// Opens the slot screen as it should look now (see ShowSlotWidgets). Returns false if it can't.
auto OpenSlotsPanel(const D2RL::PluginContext* context) -> bool {
	const std::string json = SlotsJson();
	if (json.empty() || !PoolSwap(context, g_slots, json, "save slot")) {
		return false;
	}
	g_slotsOpen = true;
	return true;
}

void __cdecl ShowSlots(const D2RL::PluginContext* context, void*) noexcept {
	const Lock lock(g_mutex);
	if (g_inGame || g_slotsOpen) {
		return;
	}
	g_confirmSlot = 0;
	g_confirmStep = 0;
	g_slotNotice = false;
	if (!OpenSlotsPanel(context)) {
		g_slotChosen = true;  // carry on with the slot that is live
	}
}

// The menu did not load again after a slot change: the list on screen is the old slot's. Keep the
// slot screen up and tell the player to restart; the new slot is already live on disk.
void __cdecl CheckMenuReloaded(const D2RL::PluginContext* context, void*) noexcept {
	const Lock lock(g_mutex);
	if (!g_awaitingMenu) {
		return;
	}
	g_awaitingMenu = false;
	context->LogWarn("Camp: the menu did not load again after the slot change. Restart the game to play the new slot.");
	g_slotNotice = true;
	OpenSlotsPanel(context);
}

void __cdecl ReloadMenu(const D2RL::PluginContext* context, void*) noexcept {
	Dispatch(context, "FrontEndNavigation", "ToFrontEnd");
	Later(context, 3000, CheckMenuReloaded);
}

// ---- Restarting for another save ----------------------------------------------------------
// The game keeps the stash of its first game in memory until it closes (camp.h, Pending), so after
// a game has been played another save only opens after a restart. The plugin starts a small
// helper that waits for this game to close and starts it again with the same command line, then
// closes the game the way Exit does. The files move while the game starts again (Load).

void __cdecl OpenCurtain(const D2RL::PluginContext* context, void*) noexcept;
void __cdecl BackToMenu(const D2RL::PluginContext* context, void*) noexcept;
void __cdecl ExitTimedOut(const D2RL::PluginContext* context, void*) noexcept;
void __cdecl PressExit(const D2RL::PluginContext* context, void*) noexcept;

auto Base64(const std::string& bytes) -> std::string {
	static constexpr char Digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	for (size_t i = 0; i < bytes.size(); i += 3) {
		const uint32_t n = (static_cast<uint8_t>(bytes[i]) << 16) |
			(i + 1 < bytes.size() ? static_cast<uint8_t>(bytes[i + 1]) << 8 : 0) |
			(i + 2 < bytes.size() ? static_cast<uint8_t>(bytes[i + 2]) : 0);
		out += Digits[(n >> 18) & 63];
		out += Digits[(n >> 12) & 63];
		out += i + 1 < bytes.size() ? Digits[(n >> 6) & 63] : '=';
		out += i + 2 < bytes.size() ? Digits[n & 63] : '=';
	}
	return out;
}

auto StartRelauncher(std::string& error) -> bool {
	std::wstring exe(32768, L'\0');
	exe.resize(GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size())));
	if (exe.empty()) {
		error = "could not find the game's program file";
		return false;
	}
	// The arguments this game was started with: the command line after the program name.
	const std::wstring line = GetCommandLineW();
	size_t start = line.empty() ? 0 : line[0] == L'"' ? line.find(L'"', 1) : line.find(L' ');
	start = start == std::wstring::npos ? line.size() : start + 1;
	std::wstring args = line.substr(start);  // start is at most line.size() here
	args.erase(0, args.find_first_not_of(L' ') == std::wstring::npos ? args.size() : args.find_first_not_of(L' '));
	const auto quote = [](const std::wstring& text) {
		std::wstring out = L"'";
		for (const wchar_t c : text) {
			out += c == L'\'' ? std::wstring(L"''") : std::wstring(1, c);
		}
		return out + L"'";
	};
	const std::wstring pid = std::to_wstring(GetCurrentProcessId());
	const std::wstring script =
		L"Wait-Process -Id " + pid + L" -Timeout 120 -ErrorAction SilentlyContinue; "
		L"if (Get-Process -Id " + pid + L" -ErrorAction SilentlyContinue) { exit }; "
		L"Start-Sleep -Seconds 2; "
		L"Start-Process -FilePath " + quote(exe) + L" -WorkingDirectory " + quote(fs::path(exe).parent_path().wstring()) +
		(args.empty() ? L"" : L" -ArgumentList " + quote(args));
	// -EncodedCommand takes the script as UTF-16LE in base64, so no quoting can break it.
	std::string utf16;
	for (const wchar_t c : script) {
		utf16 += static_cast<char>(c & 0xFF);
		utf16 += static_cast<char>((c >> 8) & 0xFF);
	}
	std::wstring command = L"powershell.exe -NoProfile -NonInteractive -WindowStyle Hidden -EncodedCommand ";
	for (const char c : Base64(utf16)) {
		command += static_cast<wchar_t>(c);
	}
	STARTUPINFOW startup {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process {};
	// Out of the game's job if it has one, so the helper outlives the game.
	BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &startup, &process);
	if (!ok) {
		ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
	}
	if (!ok) {
		error = "could not start the restart helper (error " + std::to_string(GetLastError()) + ")";
		return false;
	}
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return true;
}

// The hero screen can't close the game; the main menu can. Go there behind a black screen and
// press its Exit (see CharacterSelectPanel in OnUiMessage).
void ExitFromFire(const D2RL::PluginContext* context) {
	g_exiting = true;
	g_atFire = false;
	g_emptyFire = false;
	g_slotsOpen = false;
	PoolClose(context, g_slots);
	PoolClose(context, g_fire);
	g_threads->runOnUiThread(context, OpenCurtain, nullptr);
	Later(context, 50, BackToMenu);
	Later(context, 8000, ExitTimedOut);
}

// Keeps the slot screen up with "close the game and start it again" written on it.
void ShowRestartNotice(const D2RL::PluginContext* context) {
	g_slotNotice = true;
	OpenSlotsPanel(context);
}

// The game closing and starting itself again for a save change is switched off: Sam rejected it
// (2026-11-21) and a better way is being worked on. Until then the slot screen asks the player to
// close the game and start it again; the change is made at the next start.
constexpr bool AutoRestart = false;

// Notes `change` for the next start and restarts the game. Returns a message for the console.
auto RestartWith(const D2RL::PluginContext* context, const Camp::Pending& change) -> std::string {
	Camp::Pending pending = Camp::ReadPending(g_live);  // keep a change noted earlier this session
	pending.deletes.insert(pending.deletes.end(), change.deletes.begin(), change.deletes.end());
	if (change.open != 0) {
		pending.open = change.open;
	}
	if (!Camp::WritePending(g_live, pending)) {
		Log(context, "Camp: could not write camp/next.txt; nothing changes.");
		return "Could not note the save change.";
	}
	g_slotChosen = true;
	g_slotsOpen = false;
	PoolClose(context, g_slots);
	std::string error = "the automatic restart is off (AutoRestart)";
	const bool fromFire = g_fireOn && g_fireReady && g_threads != nullptr && (g_atFire || g_emptyFire);
	if (!AutoRestart || !StartRelauncher(error)) {
		Log(context, "Camp: %s. The change is made at the next start.", error.c_str());
		ShowRestartNotice(context);
		return "Close the game and start it again to make the change.";
	}
	Log(context, "Camp: the game holds save %d's stash, so it restarts to make the change.", g_stashSlot);
	if (fromFire) {
		ExitFromFire(context);
	} else {
		Later(context, 50, PressExit);  // the main menu is up: its Exit closes the game
	}
	return "Restarting the game for the save change.";
}

// Makes `slot` live. Only from the main menu. Returns a message for the console.
auto PickSlot(const D2RL::PluginContext* context, int slot) -> std::string {
	if (g_inGame) {
		return "Leave the game first: slots can only change at the main menu.";
	}
	g_slotChosen = true;
	g_slotsOpen = false;
	PoolClose(context, g_slots);
	Camp::Unbench(g_live, g_slot);
	const bool fire = g_fireOn && g_fireReady && g_threads != nullptr;
	std::string said = "Save " + std::to_string(slot) + " is already loaded.";
	if (slot != g_slot && g_stashSlot != 0 && !(Camp::StashJoined(g_live) && Camp::ModernStashJoined(g_live))) {  // the game holds this save's stash: restart (camp.h, Pending)
		Camp::Pending change;
		change.open = slot;
		return RestartWith(context, change);
	}
	if (slot != g_slot) {
		std::string error;
		if (!Camp::SwitchSlot(g_live, slot, error)) {
			Log(context, "Camp: could not switch to save %d: %s", slot, error.c_str());
			return "Could not switch save: " + error;
		}
		g_slot = slot;
		g_camp = Camp::LoadState(g_live, g_slot);
		g_pendingClass = -1;
		g_pickedClass = -1;
		Reconcile(context, false);
		Log(context, "Camp: switched to save %d. %s", slot, Describe().c_str());
		said = "Switched to save " + std::to_string(slot) + ".";
		if (!fire) {  // the game's character list has to be read again
			g_awaitingMenu = true;
			if (g_threads != nullptr) {
				g_threads->runOnUiThread(context, ReloadMenu, nullptr);
			}
			return said;
		}
	}
	if (fire) {  // the Campfire reads the camp itself; the menu's list is read again when a hero is loaded
		if (g_atFire || g_emptyFire) {
			g_emptyFire = false;
			RefreshFire(context);
		} else {
			g_threads->runOnUiThread(context, GoToFire, nullptr);
		}
	}
	return said;
}

// ---- The Campfire screen ------------------------------------------------------------------

// tools/build.py writes d2rloader/camp_data.txt: where each class stands, and how items look.
void ReadCampData(const D2RL::PluginContext* context) {
	std::vector<fs::path> places;
	if (context->pluginDirectory != nullptr) {
		places.push_back(fs::path(context->pluginDirectory).parent_path());
		places.push_back(fs::path(context->pluginDirectory));
	}
	if (context->modDirectory != nullptr) {
		const fs::path mod(context->modDirectory);
		places.push_back(mod / "d2rloader");
		places.push_back(mod.parent_path() / "d2rloader");
	}
	std::error_code ec;
	for (const auto& place : places) {
		if (fs::exists(place / "camp_data.txt", ec)) {
			g_dataDir = place;
			break;
		}
	}
	if (g_dataDir.empty()) {
		Log(context, "Camp: camp_data.txt not found (looked in %d places, first: %s).", static_cast<int>(places.size()), places.empty() ? "-" : places.front().string().c_str());
	}
	std::istringstream in(Camp::ReadFile(g_dataDir / "camp_data.txt"));
	std::string line;
	int spots = 0;
	while (std::getline(in, line)) {
		std::istringstream words(line);
		std::string key, name;
		words >> key >> name;
		if (key == "spot") {
			const int cls = Camp::ClassFromName(name);
			if (cls >= 0) {
				Spot& s = g_spots[cls];
				words >> s.x >> s.y >> s.w >> s.h >> s.px >> s.py >> s.pz >> s.yaw;
				s.known = !s.yaw.empty();
				spots += s.known ? 1 : 0;
			}
		} else if (key == "look") {
			const int cls = Camp::ClassFromName(name);
			std::string rest;
			std::getline(words, rest);
			const size_t start = rest.find_first_not_of(' ');
			if (cls >= 0 && start != std::string::npos) {
				g_spots[cls].look = rest.substr(start);
				while (!g_spots[cls].look.empty() && g_spots[cls].look.back() == '\r') {
					g_spots[cls].look.pop_back();
				}
			}
		} else if (key == "item") {
			ItemLook look;
			words >> look.kind;
			std::getline(words, look.value);
			const size_t start = look.value.find_first_not_of(' ');
			look.value = start == std::string::npos ? "" : look.value.substr(start);
			while (!look.value.empty() && (look.value.back() == '\r' || look.value.back() == ' ')) {
				look.value.pop_back();
			}
			g_looks.emplace_back(name, look);
		}
	}
	Log(context, "Camp: camp_data.txt gives %d class spots and %d item looks.", spots, static_cast<int>(g_looks.size()));
}

auto LookOf(const std::string& code) -> const ItemLook* {
	for (const auto& [known, look] : g_looks) {
		if (known == code) {
			return &look;
		}
	}
	return nullptr;
}

// The figure fields for what a hero wears ("4:hax 5:buc 3:qui": 3 body, 4 right arm, 5 left arm).
auto GearFields(const std::string& gear) -> std::string {
	static constexpr const char* Weights[] = { "Light", "Medium", "Heavy" };
	static constexpr const char* Parts[]   = { "torso", "legs", "rightArm", "leftArm", "spec1", "spec2" };
	std::string out;
	std::istringstream words(gear);
	std::string word;
	while (words >> word) {
		const size_t colon = word.find(':');
		if (colon == std::string::npos) {
			continue;
		}
		const int where = std::atoi(word.c_str());
		const ItemLook* look = LookOf(word.substr(colon + 1));
		if (look == nullptr) {
			continue;
		}
		if (look->kind == 'w' && (where == 4 || where == 5)) {
			out += std::string(",\"player/") + (where == 4 ? "rightHand" : "leftHand") + "\":\"" + look->value + "\"";
		} else if (look->kind == 's' && (where == 4 || where == 5)) {
			out += ",\"player/shield\":\"" + look->value + "\"";
		} else if (look->kind == 'b' && where == 3) {
			std::istringstream weights(look->value);
			int weight = 0;
			for (int part = 0; part < 6 && (weights >> weight); ++part) {
				out += std::string(",\"player/") + Parts[part] + "\":\"" + Weights[std::clamp(weight, 0, 2)] + "\"";
			}
		}
	}
	return out;
}

// The mod version, small, in the bottom right corner of our full-screen panels.
auto VersionWidgetJson() -> std::string {
	return std::string("{\"type\":\"Widget\",\"name\":\"VersionCorner\",\"fields\":{\"anchor\":{\"x\":0.5,\"y\":1.0}},\"children\":["
		"{\"type\":\"TextBoxWidget\",\"name\":\"Version\",\"fields\":{\"rect\":{\"x\":-760,\"y\":-90,\"width\":720,\"height\":50},\"text\":\"Darkest Diablo ") + Campfire::Version +
		"\",\"style\":{\"fontColor\":\"$FontColorLightGray\",\"alignment\":{\"h\":\"right\"},\"pointSize\":\"$MediumFontSize\",\"dropShadow\":\"$DefaultDropShadow\"}}}]}";
}

auto FireButton(const std::string& name, int x, int y, const std::string& text, const std::string& message, const char* art, const char* color) -> std::string {
	return "{\"type\":\"ButtonWidget\",\"name\":\"" + name + "\",\"fields\":{\"rect\":{\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) +
		"},\"filename\":\"" + art + "\",\"textString\":\"" + text + "\",\"onClickMessage\":\"" + message +
		"\",\"pressedFrame\":1,\"hoveredFrame\":3,\"disabledFrame\":2,\"text/style\":\"$StyleFEButtonText\",\"pointSize\":\"$LargeFontSize\",\"textColor\":\"" + color + "\"}}";
}

// A traveler waits at the Campfire when the camp has heroes and an offer still open (with no
// heroes, the offer is simply what the hero screen may make).
auto TravelerWaits() -> bool {
	return !g_camp.offer.empty() && !g_camp.heroes.empty() && !g_travelerLater;
}

// The classes that may be clicked at the Campfire right now (besides the camp's heroes).
auto Allowed() -> std::vector<int> {
	return g_travelerMode ? g_camp.offer : Camp::Creatable(g_camp, g_rng);
}

// The selection glow: one soft gold picture behind the picked hero's name and one around the lit
// Enter button (src/patches/042_campfire_glow.py).
// The name under each hero: plain text since 0.20.0 (was a framed plate, busy with neighbours).
// The glow picture's size comes from 042_campfire_glow.py.
constexpr int PlateW = 380, PlateH = 60, PlateY = 1440, PlateDrop = 70;
constexpr int PlateGlowW = 392, PlateGlowH = 153;
constexpr int EnterW = 824, EnterH = 196, EnterY = 1740, EnterMargin = 40;  // FrontEnd_ButtonLarge at 1.35, measured in the game

auto GlowWidget(const std::string& picture, int x, int y) -> std::string {
	return "{\"type\":\"ImageWidget\",\"name\":\"" + picture + "\",\"fields\":{\"rect\":{\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) +
		"},\"filename\":\"PANEL\\\\DarkestDiablo\\\\" + picture + "\"}},";
}

// The act a hero is in: that of its first open stop.
auto ActOfHero(const std::string& name) -> int {
	const int tier = Route::Load(Route::ProgressFile(g_live, g_slot, name)).CurrentTier();
	return Route::ActOfTier((std::min)(tier, Route::TierCount - 1));
}

// The question after Ascend: nothing else is in the layout (hidden buttons take clicks).
auto AscendLayoutJson(const Camp::Hero& hero) -> std::string {
	static constexpr char Med[] = "FrontEnd\\\\HD\\\\Final\\\\FrontEnd_ButtonMed";
	static constexpr const char* Bosses[] = { "Andariel", "Duriel", "Mephisto", "Diablo", "Baal" };
	const int actNumber = ActOfHero(hero.name);
	const int advised = Ascension::AdvisedLevel(actNumber);
	// A letter of the large font is about 29 wide (measured in a screenshot), so 66 letters need
	// about 1920 of the box's 2300.
	constexpr int Top = 330, BoxW = 2300, Margin = 70, LineH = 54, Gap = 40;
	constexpr size_t Letters = 66;
	std::string kids;
	int line = 0, y = Top + Margin;
	const auto text = [&](const std::string& words, const char* color, const char* size, int height) {
		for (const std::string& part : Ascension::Wrap(words, Letters)) {
			kids += "{\"type\":\"TextBoxWidget\",\"name\":\"Ask" + std::to_string(line++) + "\",\"fields\":{\"rect\":{\"x\":" + std::to_string(-BoxW / 2 + Margin) + ",\"y\":" + std::to_string(y) +
				",\"width\":" + std::to_string(BoxW - 2 * Margin) + ",\"height\":" + std::to_string(height) + "},\"text\":\"" + part + "\",\"style\":{\"fontColor\":\"" + color +
				"\",\"alignment\":{\"h\":\"center\"},\"pointSize\":\"" + size + "\",\"dropShadow\":\"$DefaultDropShadow\"}}},";
			y += height;
		}
		y += Gap;
	};
	text(hero.name + " seeks Ascension", "$FontColorRed", "$XExtraLargeFontSize", 90);
	text(std::string(Bosses[actNumber - 1]) + " waits in Ascended form: more life, harder hits, faster, with four elite powers.", "$FontColorWhite", "$LargeFontSize", LineH);
	text("There is no way out. Dying, leaving or quitting before the boss is dead is a permanent death, and everything on this hero is lost.", "$FontColorRed", "$LargeFontSize", LineH);
	text("Win, and this hero leaves the camp for good. All it wears and carries, and what is in its Cube, waits in the camp's stash.", "$FontColorGoldYellow", "$LargeFontSize", LineH);
	text("Meant to be fought alone, one on one. A mercenary may come along, but its gear does NOT come home. Stash anything you want to keep from it first, and leave the mercenary behind.", "$FontColorWhite", "$LargeFontSize", LineH);
	if (hero.level < advised) {
		text("WARNING: " + hero.name + " is level " + std::to_string(hero.level) + ". This fight is meant for level " + std::to_string(advised) + " and up. Good luck...", "$FontColorRed", "$LargeFontSize", LineH);
	}
	text("Are you sure?", "$FontColorWhite", "$LargeFontSize", LineH);
	const std::string act = "PanelManager:OpenPanel:darkestdiablo/";
	kids += FireButton("AskYes", -560, y + 10, "Ascend", act + "ascend-accept", Med, "$FontColorRed") + ",";
	kids += FireButton("AskNo", 80, y + 10, "Turn back", act + "ascend-cancel", Med, "$LightButtonTextColor") + ",";
	kids += VersionWidgetJson();
	// The box, as high as the text needs, behind everything.
	kids = "{\"type\":\"NineTileImageWidget\",\"name\":\"AskBack\",\"fields\":{\"filename\":\"PANEL/TooltipBorder/TooltipBorder\",\"backgroundColor\":[0.07,0.0,0.0,0.97],\"tileScale\":1.0,"
		"\"rect\":{\"x\":" + std::to_string(-BoxW / 2) + ",\"y\":" + std::to_string(Top) + ",\"width\":" + std::to_string(BoxW) + ",\"height\":" + std::to_string(y + 10 + 120 + Margin - Top) + "}}}," + kids;
	return "{\"type\":\"Panel\",\"name\":\"@PANEL@\",\"fields\":{\"anchor\":{\"x\":0.5},\"fitToParent\":true},\"children\":[" + kids + "]}";
}

// The Fire panel's layout for the camp as it is now: each living hero as a figure in its gear
// where its class stands, and the Options, Dismiss and Exit buttons. What changes while the screen is
// up (names, glow, Enter...) is in FireTopJson.
auto FireLayoutJson() -> std::string {
	static constexpr char Med[]   = "FrontEnd\\\\HD\\\\Final\\\\FrontEnd_ButtonMed";
	const std::string act = "PanelManager:OpenPanel:darkestdiablo/";
	std::string kids;
	// While a traveler waits, the screen holds only the offered classes, the question and its two
	// buttons: hidden widgets still take clicks, so nothing else may be in the layout (0.11.18:
	// the hidden Enter button took the clicks meant for Accept and Not now).
	const bool traveler = TravelerWaits();
	if (g_ascendAsk >= 0 && !traveler && g_camp.HeroOf(g_ascendAsk) != nullptr) {
		return AscendLayoutJson(*g_camp.HeroOf(g_ascendAsk));
	}
	const auto figure = [&](int c, const std::string& look) {
		const Spot& s = g_spots[c];
		const std::string cls = Camp::ClassNames[c];
		kids += "{\"type\":\"HDCharacterSelectorWidget\",\"name\":\"" + cls + "\",\"fields\":{\"rect\":{\"x\":" + std::to_string(s.x) + ",\"y\":" + std::to_string(s.y) +
			",\"width\":" + std::to_string(s.w) + ",\"height\":" + std::to_string(s.h) + "},\"player/playerClass\":\"" + cls +
			"\",\"player/position\":[" + s.px + "," + s.py + "," + s.pz + "],\"player/yawDegrees\":" + s.yaw + look + ",\"focusIndicatorVisible\":false}},";
	};
	// The camp's heroes, each in what it was last seen wearing.
	for (const auto& hero : g_camp.heroes) {
		const Spot& s = g_spots[hero.cls];
		if (!s.known || traveler) {
			continue;
		}
		const std::string cls = Camp::ClassNames[hero.cls];
		figure(hero.cls, GearFields(hero.gear));
	}
	// The classes that may join right now, and a waiting traveler's offer, in the game's
	// standard look.
	const std::vector<int> joining = traveler ? g_camp.offer : Camp::Creatable(g_camp, g_rng);
	for (const int c : joining) {
		if (g_spots[c].known && g_camp.HeroOf(c) == nullptr) {
			figure(c, g_spots[c].look.empty() ? "" : "," + g_spots[c].look);
		}
	}
	if (traveler) {
		kids += "{\"type\":\"TextBoxWidget\",\"name\":\"TravelerHint\",\"fields\":{\"rect\":{\"x\":-900,\"y\":210,\"width\":1800,\"height\":70},"
			"\"text\":\"A traveler joins your camp. Choose who stays by the fire.\","
			"\"style\":{\"fontColor\":\"$FontColorGoldYellow\",\"alignment\":{\"h\":\"center\"},\"pointSize\":\"$LargeFontSize\",\"dropShadow\":\"$DefaultDropShadow\"}}},";
		kids += FireButton("TravelerLater", 60, 1760, "Not now", act + "traveler-not-now", Med, "$LightButtonTextColor") + ",";
		kids += VersionWidgetJson();
		return "{\"type\":\"Panel\",\"name\":\"@PANEL@\",\"fields\":{\"anchor\":{\"x\":0.5},\"fitToParent\":true},\"children\":[" + kids + "]}";
	}
	kids += "{\"type\":\"Widget\",\"name\":\"LeftButtons\",\"fields\":{\"anchor\":{\"x\":-0.5,\"y\":1.0}},\"children\":[" +
		FireButton("Options", 100, -500, "@d2r:OptionsHD", "PanelManager:OpenPanel:SettingsPanel", Med, "$LightButtonTextColor") + "," +
		FireButton("Delete", 100, -360, "Dismiss", act + "delete-mode", Med, "$LightButtonTextColor") + "]}";
	// Exit Game in the top left corner (Sam), away from the buttons used while at the fire.
	kids += ",{\"type\":\"Widget\",\"name\":\"ExitCorner\",\"fields\":{\"anchor\":{\"x\":-0.5,\"y\":0.0}},\"children\":[" +
		FireButton("ExitGame", 100, 60, "@d2r:strexitentiregame", "FrontEndNavigation:ToExit", Med, "$LightButtonTextColor") + "]}";
	kids += "," + VersionWidgetJson();
	return "{\"type\":\"Panel\",\"name\":\"@PANEL@\",\"fields\":{\"anchor\":{\"x\":0.5},\"fitToParent\":true},\"children\":[" + kids + "]}";
}

// What lies over the figures right now: the names (the picked hero's in white in its light), Enter
// (lit once a hero is picked), the retire buttons, or the traveler's Accept.
auto FireTopJson() -> std::string {
	static constexpr char Med[]   = "FrontEnd\\\\HD\\\\Final\\\\FrontEnd_ButtonMed";
	static constexpr char Large[] = "FrontEnd\\\\HD\\\\Final\\\\FrontEnd_ButtonLarge";
	static constexpr char Plate[] = "Panel\\\\Modals\\\\ModalButton";
	const std::string act = "PanelManager:OpenPanel:darkestdiablo/";
	std::string kids;
	if (g_travelerMode) {
		if (Camp::State::Has(g_camp.offer, g_selectedClass)) {
			kids += FireButton("TravelerAccept", -420, 1760, "Accept", act + "traveler-accept", Med, "$LightButtonTextColor") + ",";
		}
	} else {
		for (const auto& hero : g_camp.heroes) {
			const Spot& s = g_spots[hero.cls];
			if (!s.known) {
				continue;
			}
			const std::string cls = Camp::ClassNames[hero.cls];
			if (g_deleteMode) {
				kids += FireButton("Bury" + cls, s.x + (s.w - 300) / 2, 1590, "Dismiss " + hero.name, act + "bury-" + cls, Plate, "$FontColorRed") + ",";
				continue;
			}
			// No frame, just the name and "Lv N" small under it, dim unless picked. Every other hero's,
			// from left to right, stands lower, so neighbours' names don't run together (Sam).
			const bool picked = hero.cls == g_chosenHero && !g_createMode;
			const int x = s.x + (s.w - PlateW) / 2;
			const int rank = static_cast<int>(std::count_if(g_camp.heroes.begin(), g_camp.heroes.end(),
				[&](const auto& other) { return g_spots[other.cls].known && g_spots[other.cls].x < s.x; }));
			const int y = PlateY + (rank % 2 == 1 ? PlateDrop : 0);
			if (picked) {
				kids += GlowWidget("HeroGlow", s.x + (s.w - PlateGlowW) / 2, y + (PlateH - PlateGlowH) / 2);
			}
			const auto text = [&](const std::string& name, int top, int h, const std::string& value, const char* size, const char* color) {
				kids += "{\"type\":\"TextBoxWidget\",\"name\":\"" + name + "\",\"fields\":{\"rect\":{\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y + top) +
					",\"width\":" + std::to_string(PlateW) + ",\"height\":" + std::to_string(h) + "},\"text\":\"" + value + "\",\"style\":{\"pointSize\":" + size +
					",\"alignment\":{\"v\":\"center\",\"h\":\"center\"},\"fontColor\":\"" + color + "\",\"dropShadow\":\"$DefaultDropShadow\"}}},";
			};
			text("Name" + cls, 0, 34, hero.name, "28", picked ? "$FontColorWhite" : "$FontColorDarkGrayGold");
			text("Rank" + cls, 34, 26, "Lv " + std::to_string(hero.level), "18", picked ? "$FontColorWhite" : "$FontColorDarkGrayBlue");
		}
		if (!g_ascendedNote.empty() && !g_deleteMode && !g_createMode) {
			kids += "{\"type\":\"TextBoxWidget\",\"name\":\"AscendedNote\",\"fields\":{\"rect\":{\"x\":-1300,\"y\":210,\"width\":2600,\"height\":70},\"text\":\"" + g_ascendedNote +
				"\",\"style\":{\"fontColor\":\"$FontColorGoldYellow\",\"alignment\":{\"h\":\"center\"},\"pointSize\":\"$LargeFontSize\",\"dropShadow\":\"$DefaultDropShadow\"}}},";
		}
		if (g_deleteMode) {
			kids += "{\"type\":\"TextBoxWidget\",\"name\":\"DeleteHint\",\"fields\":{\"rect\":{\"x\":-900,\"y\":210,\"width\":1800,\"height\":70},"
				"\"text\":\"Choose the hero to dismiss. Everything it carries is lost. This can't be undone. Press Dismiss again to keep everyone.\","
				"\"style\":{\"fontColor\":\"$FontColorRed\",\"alignment\":{\"h\":\"center\"},\"pointSize\":\"$LargeFontSize\",\"dropShadow\":\"$DefaultDropShadow\"}}},";
		}
		if (!g_camp.heroes.empty() && !g_createMode) {
			// Enter: the game's largest button, drawn bigger. Grey until a hero is picked; then it is
			// lit: warm stone, gold text and a soft gold light around it (under it: an image over a
			// button might take its clicks).
			const bool ready = g_chosenHero >= 0 && g_camp.HeroOf(g_chosenHero) != nullptr && !g_deleteMode;
			if (ready) {
				kids += GlowWidget("EnterGlow", -EnterW / 2 - EnterMargin, EnterY - EnterMargin);
			}
			kids += std::string("{\"type\":\"ButtonWidget\",\"name\":\"Enter\",\"fields\":{\"rect\":{\"x\":-412,\"y\":") + std::to_string(EnterY) + ",\"scale\":1.35},\"filename\":\"" + Large +
				"\",\"textString\":\"ENTER\",\"onClickMessage\":\"" + act + "enter\",\"pressedFrame\":1,\"hoveredFrame\":3,\"disabledFrame\":2,"
				"\"text/style\":\"$StyleFEButtonText\",\"pointSize\":\"$XExtraLargeFontSize\"," +
				(ready ? "\"tint\":{\"r\":1.0,\"g\":0.94,\"b\":0.80,\"a\":1.0},\"textColor\":\"$FontColorGoldYellow\"" : "\"textColor\":\"$FontColorGray\"") + "}},";
			if (ready) {  // Ascend, under Enter: blood red, only while a hero is picked
				kids += std::string("{\"type\":\"ButtonWidget\",\"name\":\"Ascend\",\"fields\":{\"rect\":{\"x\":-235,\"y\":1985},\"filename\":\"") + Med +
					"\",\"textString\":\"ASCEND\",\"onClickMessage\":\"" + act + "ascend\",\"pressedFrame\":1,\"hoveredFrame\":3,\"disabledFrame\":2,"
					"\"text/style\":\"$StyleFEButtonText\",\"tint\":{\"r\":0.75,\"g\":0.12,\"b\":0.10,\"a\":1.0},\"textColor\":\"$FontColorRed\"}},";
			}
		}
	}
	if (!kids.empty()) {
		kids.pop_back();  // the last comma
	}
	return "{\"type\":\"Panel\",\"name\":\"@PANEL@\",\"fields\":{\"anchor\":{\"x\":0.5},\"fitToParent\":true},\"children\":[" + kids + "]}";
}

// Hands the loader the Fire layout for the camp as it is now. The loader reads a panel's layout
// when the panel opens, so this runs before each opening.
auto RegisterFireLayout(const D2RL::PluginContext* context) -> bool {
	g_fireReady = PoolSet(context, g_fire, FireLayoutJson());
	Log(context, "Camp: Campfire layout for %d hero(es) (%s).", static_cast<int>(g_camp.heroes.size()), g_fireReady ? "ready" : "failed");
	return g_fireReady;
}

// Shows the right widgets for where the Campfire is: picking a hero, Delete, or making a hero.
void ShowDeleteMode(const D2RL::PluginContext* context) noexcept {
	const bool traveler = g_travelerMode || g_ascendAsk >= 0;
	try {
		if (g_ascendAsk >= 0) {
			Close(context, PoolHandle(g_top));
		} else if (g_atFire && PoolIsOpen(context, g_fire)) {
			PoolSwap(context, g_top, FireTopJson(), "Campfire names");
		}
	} catch (...) {
	}
	// The game's class name and description would shine through the question.
	static bool infoWasHidden = false;
	const bool infoHidden = g_ascendAsk >= 0;
	if (infoHidden || infoWasHidden) {
		for (const char* part : { "InfoContainer", "ClassTitle", "ClassDescription" }) {
			SetVisible(context, "CharacterCreatePanel", part, !infoHidden);
		}
		infoWasHidden = infoHidden;
	}
	// The game's own name box, version picker and Create button.
	// The game shows its name box again by itself, so each part of it is hidden too.
	int found = 0;
	for (const char* part : { "InputContainer", "NameLabel", "InputBackground", "InputText", "HardcoreCheckbox", "LadderToggle", "GameVersionContainer" }) {
		found += SetVisible(context, "CharacterCreatePanel", part, g_createMode) ? 1 : 0;
	}
	static int lastFound = -1;
	if (found != lastFound) {
		lastFound = found;
		Log(context, "Camp: name box parts reached: %d of 7.", found);
	}
	SetVisible(context, "CharacterCreatePanel", "ToGame", !traveler && g_ascendAsk < 0 && !Camp::Creatable(g_camp, g_rng).empty());
}

void __cdecl ShowFireState(const D2RL::PluginContext* context, void*) noexcept {
	const Lock lock(g_mutex);
	if (g_atFire) {
		ShowDeleteMode(context);
	}
}

void __cdecl OpenCurtain(const D2RL::PluginContext* context, void*) noexcept {
	const Lock lock(g_mutex);
	if (!g_inGame && !g_atFire && (g_wantFire || g_exiting || !g_pendingPlay.empty())) {
		Open(context, g_curtainPanel, "curtain");
	}
}

// If the game did not close after all (it asked first, or the exit was called off), the menu
// must not stay covered.
void __cdecl ExitTimedOut(const D2RL::PluginContext* context, void*) noexcept {
	const Lock lock(g_mutex);
	if (g_exiting) {
		g_exiting = false;
		Close(context, g_curtainPanel);
		if (!g_inGame && !g_atFire && g_fireOn && g_fireReady) {
			GoToFire(context, nullptr);
		}
	}
}

// A click on a hero while another hero plays its long "step forward" animation: the game
// ignores it and sends no Select at all (Sam's test, 0.11.63 log), but Sam wants the click to
// change the pick. Runs a little after the click: if the game did not pick the hero by itself,
// the plugin picks it (Enter, name plate) and asks the game to select its figure too.
void __cdecl ConfirmFireClick(const D2RL::PluginContext* context, void*) noexcept {
	const Lock lock(g_mutex);
	const int cls = g_clickedHero;
	g_clickedHero = -1;
	if (!g_atFire || g_deleteMode || g_createMode || g_travelerMode || cls < 0 || cls == g_chosenHero || g_camp.HeroOf(cls) == nullptr) {
		return;
	}
	Log(context, "Camp: the game ignored a click on the %s (an animation was playing); picking it anyway.", Camp::ClassNames[cls]);
	g_chosenHero = cls;
	g_selectedClass = -1;
	ShowDeleteMode(context);
	Dispatch(context, "CharacterCreate", "Select", Camp::ClassNames[cls]);  // the figure steps forward, if the game lets it
	g_chosenHero = cls;  // whatever the game answered, the click picked this hero
	ShowDeleteMode(context);
}

void __cdecl NoteFireClick(const D2RL::PluginContext* context, void*) noexcept {
	const Lock lock(g_mutex);
	if (g_atFire && g_hoveredHero >= 0 && g_hoveredHero != g_chosenHero) {
		g_clickedHero = g_hoveredHero;
		Later(context, 150, ConfirmFireClick);  // time for the game's own Select, when it sends one
	}
}

// While the Campfire is up, watches the left mouse button (Windows' own mouse state; the SDK
// passes no mouse clicks to plugins) and reports each click made in the game's window.
void WatchFireClicks(const D2RL::PluginContext* context) {
	if (g_watchingClicks.exchange(true) || g_threads == nullptr) {
		return;
	}
	std::thread([context] {
		bool down = false;
		for (;;) {
			{
				const Lock lock(g_mutex);
				if (!g_atFire) {
					break;
				}
			}
			const bool now = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
			if (!now && down) {  // released: the game's own Select comes on release too
				DWORD pid = 0;
				GetWindowThreadProcessId(GetForegroundWindow(), &pid);
				if (pid == GetCurrentProcessId()) {
					g_threads->runOnUiThread(context, NoteFireClick, nullptr);
				}
			}
			down = now;
			std::this_thread::sleep_for(std::chrono::milliseconds(15));
		}
		g_watchingClicks = false;
	}).detach();
}

// The hero screen has opened: lay the camp's figures over it.
void __cdecl OpenFire(const D2RL::PluginContext* context, void*) noexcept {
	const Lock lock(g_mutex);
	if (g_inGame || !g_fireOn || !g_fireReady) {
		return;
	}
	if (PoolOpen(context, g_fire, "Campfire")) {
		g_atFire = true;
		g_deleteMode = false;
		g_createMode = g_camp.heroes.empty();  // a new save has nobody to pick: go straight to making a hero
		g_travelerMode = TravelerWaits();       // a traveler waits: show only the offer first
		g_chosenHero = -1;
		g_hoveredHero = -1;
		g_clickedHero = -1;
		g_selectedClass = -1;
		WatchFireClicks(context);
		if (g_justRecruited >= 0) {             // after Accept: the new class steps in, name box ready
			g_createMode = true;
			g_selectedClass = g_justRecruited;
			g_pickedClass = g_justRecruited;
			g_justRecruited = -1;
		}
		ShowDeleteMode(context);
		Later(context, 300, ShowFireState);  // again once the game's screen has settled
		Close(context, g_curtainPanel);
		Log(context, "Camp: the Campfire is open. %s", Describe().c_str());
	}
}

// The camp changed while the Campfire is up (a hero was buried, another save was picked).
void RefreshFire(const D2RL::PluginContext* context) {
	PoolClose(context, g_fire);
	RegisterFireLayout(context);
	Later(context, 150, OpenFire);
}

// From the main menu on to the Campfire.
void __cdecl GoToFire(const D2RL::PluginContext* context, void*) noexcept {
	const Lock lock(g_mutex);
	if (g_inGame) {
		return;
	}
	Camp::Unbench(g_live, g_slot);
	g_ascendAsk = -1;
	RegisterFireLayout(context);
	Dispatch(context, "FrontEndNavigation", "ToCharacterCreate");
}

// At launch: the hero screen with nobody at the fire, as the save slots' background.
void __cdecl GoToEmptyFire(const D2RL::PluginContext* context, void*) noexcept {
	const Lock lock(g_mutex);
	if (g_inGame) {
		return;
	}
	g_emptyFire = true;
	Dispatch(context, "FrontEndNavigation", "ToCharacterCreate");
}

// Over the empty fire nothing of the game's hero screen is used: its name box, version picker,
// Create and the toggle beside them are hidden (the game shows the name box again by itself, so
// this runs a few times while the screen settles).
void __cdecl HideCreateParts(const D2RL::PluginContext* context, void*) noexcept {
	const Lock lock(g_mutex);
	if (!g_emptyFire) {
		return;
	}
	for (const char* part : { "InputContainer", "NameLabel", "InputBackground", "InputText", "HardcoreCheckbox", "LadderToggle",
	         "GameVersionContainer", "ToGame" }) {
		SetVisible(context, "CharacterCreatePanel", part, false);
	}
}

void __cdecl ShowEmptyFire(const D2RL::PluginContext* context, void*) noexcept {
	const Lock lock(g_mutex);
	HideCreateParts(context, nullptr);
	Close(context, g_curtainPanel);
	ShowSlots(context, nullptr);
	Later(context, 300, HideCreateParts);
	Later(context, 1000, HideCreateParts);
	Log(context, "Camp: the save slots, over the empty fire.");
}

// Change save at the Campfire: the camp's heroes go (their panel closes) and the save slots
// show over the empty fire.
void __cdecl LeaveFireForSlots(const D2RL::PluginContext* context, void*) noexcept {
	const Lock lock(g_mutex);
	if (g_inGame || !g_atFire || g_slotsOpen) {
		return;
	}
	g_atFire = false;
	g_emptyFire = true;
	PoolClose(context, g_fire);
	ShowEmptyFire(context, nullptr);
}

// The game did not start (a window was closed, or Play did nothing): back to the Campfire.
void __cdecl PlayTimedOut(const D2RL::PluginContext* context, void*) noexcept {
	const Lock lock(g_mutex);
	if (g_pendingPlay.empty() || g_inGame) {
		return;
	}
	Log(context, "Camp: %s was not loaded in time; back to the Campfire.", g_pendingPlay.c_str());
	g_pendingPlay.clear();
	Ascension::Plan("", 0);
	Close(context, g_curtainPanel);
	GoToFire(context, nullptr);
}

void __cdecl PressPlay(const D2RL::PluginContext* context, void*) noexcept {
	const Lock lock(g_mutex);
	if (g_pendingPlay.empty() || g_inGame) {
		return;
	}
	Log(context, "Camp: loading %s.", g_pendingPlay.c_str());
	Dispatch(context, "MainMenuHD", "Play");
	Later(context, 20000, PlayTimedOut);
}

// What the hero screen's own Back button sends; "ToFrontEnd" does nothing from that screen.
void __cdecl BackToMenu(const D2RL::PluginContext* context, void*) noexcept {
	Dispatch(context, "FrontEndNavigation", "ToCharacterSelect");
}

// Exit was pressed at the Campfire and the main menu has loaded behind the curtain: only the
// main menu can close the game, so press its Exit.
void __cdecl PressExit(const D2RL::PluginContext* context, void*) noexcept {
	Dispatch(context, "FrontEndNavigation", "ToExit");
}

// A hero at the fire was clicked: load it.
void PlayHero(const D2RL::PluginContext* context, int cls) {
	const Camp::Hero* hero = g_camp.HeroOf(cls);
	if (hero == nullptr || g_inGame || !g_pendingPlay.empty()) {
		return;
	}
	std::string error;
	if (!Ascension::Planned() && !g_shapes.items.empty() && !g_shapes.stats.empty()) {  // an ascended hero's items may travel with this one
		std::vector<std::string> living;
		for (const auto& h : g_camp.heroes) {
			living.push_back(h.name);
		}
		const std::string note = Legacy::Send(g_live, g_slot, hero->name, living, g_shapes);
		if (!note.empty()) {
			Log(context, "Camp: %s", note.c_str());
		}
	}
	Camp::SyncBindings(g_live);  // the hero about to play gets the camp's keys
	if (!Camp::Bench(g_live, g_slot, g_camp, hero->name, error)) {
		Log(context, "Camp: could not set the other heroes aside (%s); %s was not loaded.", error.c_str(), hero->name.c_str());
		Camp::Unbench(g_live, g_slot);
		return;
	}
	g_pendingPlay = hero->name;
	g_wantFire = true;
	Log(context, "Camp: %s the %s steps forward.", hero->name.c_str(), Camp::ClassNames[cls]);
	PoolClose(context, g_fire);
	if (g_threads != nullptr) {
		g_threads->runOnUiThread(context, BackToMenu, nullptr);
	}
}

// Delete: the hero is buried like one that died.
void BuryHero(const D2RL::PluginContext* context, int cls) {
	const Camp::Hero* hero = g_camp.HeroOf(cls);
	if (hero == nullptr || !g_deleteMode) {
		return;
	}
	const std::string name = hero->name;
	std::string error;
	if (!Camp::Bury(g_live, g_slot, name, error)) {
		Log(context, "Camp: could not bury %s (%s).", name.c_str(), error.c_str());
		return;
	}
	const std::string panel = PoolName(g_fire);
	Camp::MarkFallen(g_camp, name);
	Camp::SaveState(g_live, g_slot, g_camp);
	g_deleteMode = false;
	RefreshFire(context);  // an empty fire brings a traveler: show who may join
	Log(context, "Camp: %s the %s was deleted. That counts as a death. %s", name.c_str(), Camp::ClassNames[cls], Describe().c_str());
}

// What the hero in the game wears, for its figure at the Campfire. UI thread.
void __cdecl RecordGear(const D2RL::PluginContext* context, void*) noexcept {
	const Lock lock(g_mutex);
	if (!g_inGame || g_inventory == nullptr || g_items == nullptr) {
		return;
	}
	D2RL::PlayerHandle player {};
	if (g_inventory->getLocalPlayer(context, &player) != D2RL::Inventory::Result::Success) {
		return;
	}
	std::string gear;
	for (const int where : { 1, 3, 4, 5 }) {  // head, body, right arm, left arm
		D2RL::ItemHandle item {};
		D2RL::Items::ItemInfo info {};
		info.structSize = static_cast<uint32_t>(sizeof(info));
		if (g_inventory->getEquippedItem(context, player, where, &item) != D2RL::Inventory::Result::Success ||
			g_items->getItemInfo(context, item, &info) != D2RL::Items::Result::Success) {
			continue;
		}
		std::string code;
		for (int i = 0; i < 4; ++i) {
			const char c = static_cast<char>((info.code >> (8 * i)) & 0xFF);
			if (std::isalnum(static_cast<unsigned char>(c))) {
				code += c;
			}
		}
		if (!code.empty()) {
			gear += (gear.empty() ? "" : " ") + std::to_string(where) + ":" + code;
		}
	}
	if (gear != g_gearNow) {
		g_gearNow = gear;
		Log(context, "Camp: the hero wears [%s].", gear.c_str());
	}
}

// ---- Traveler -----------------------------------------------------------------------------

void __cdecl ShowTraveler(const D2RL::PluginContext* context, void*) noexcept {
	const Lock lock(g_mutex);
	if (!g_inGame || !IsTown(g_levelId)) {
		return;
	}
	if (g_camp.offer.empty()) {
		g_camp.offer = Camp::RollOffer(g_camp, g_rng);
		Camp::SaveState(g_live, g_slot, g_camp);
	}
	// A notice only: the choice is made at the Campfire, which the fire leads to.
	if (g_camp.offer.empty() || !Open(context, g_travelerPanel, "traveler")) {
		return;
	}
	Log(context, "Camp: a traveler joins the camp. %s", Describe().c_str());
}

// ---- Roster panel (in town) ---------------------------------------------------------------

void __cdecl ShowCamp(const D2RL::PluginContext* context, void*) noexcept {
	const Lock lock(g_mutex);
	Open(context, g_campPanel, "campfire");  // one button: rest, which saves and goes to the Campfire scene
}

void __cdecl PressSaveAndExit(const D2RL::PluginContext* context, void*) noexcept {
	Dispatch(context, "PausePanelMessage", "ExitGame");
}

// Save and Exit. The game only acts on ExitGame while its pause menu is open (sent alone it is
// ignored: 0.11.18), so the stash is closed, the pause menu opened, and Save and Exit pressed.
// If the game did not leave after all, the hero must not stay behind a black screen.
void __cdecl LeaveTimedOut(const D2RL::PluginContext* context, void*) noexcept {
	const Lock lock(g_mutex);
	if (g_inGame && g_levelId >= 0) {
		Close(context, g_curtainPanel);
		Log(context, "Camp: the game did not leave; curtain lifted.");
	}
}

void __cdecl LeaveGame(const D2RL::PluginContext* context, void*) noexcept {
	Open(context, g_curtainPanel, "curtain");  // black from here until the Campfire shows (OpenFire)
	Later(context, 8000, LeaveTimedOut);
	Dispatch(context, "PanelManager", "ClosePanel", "BankExpansionLayout");
	Dispatch(context, "PanelManager", "OpenPanel", "PauseLayout");
	Later(context, 250, PressSaveAndExit);
}

// Runs a console command through D2RCore's ExecuteConsoleCommand export (UI thread only).
// "Accepted" is not "worked": errors show in the in-game console only.
auto RunConsole(const D2RL::PluginContext* context, const char* command) noexcept -> bool {
	namespace CE = D2RL::CoreExports;
	static CE::ExecuteConsoleCommandFn execute = nullptr;
	if (execute == nullptr) {
		const HMODULE core = GetModuleHandleA(CE::CoreDllName);
		execute = core == nullptr ? nullptr : reinterpret_cast<CE::ExecuteConsoleCommandFn>(GetProcAddress(core, CE::ExecuteConsoleCommandInfo.name));
	}
	const bool ok = execute != nullptr && execute(command);
	Log(context, "Camp: console %s -> %s", command, ok ? "sent" : "refused");
	return ok;
}

// ---- The traveler at the fire ---------------------------------------------------------------
// The town lands somewhere new every game, so the fire's place can't be known ahead. When the
// fire is used the hero stands by it: "location player" makes that the spawn spot, Cain's
// portal opens there, the hooded traveler (045_traveler.py) steps out, and then the hero goes
// to the Campfire, where the traveler's classes wait (traveler mode).
void __cdecl TravelerGoesToFire(const D2RL::PluginContext* context, void*) noexcept {
	const Lock lock(g_mutex);
	g_travelerArrives = false;
	if (g_inGame) {
		RecordGear(context, nullptr);
		LeaveGame(context, nullptr);
	}
}

void __cdecl TravelerStepsOut(const D2RL::PluginContext* context, void*) noexcept {
	const Lock lock(g_mutex);
	if (g_inGame && g_travelerArrives) {
		RunConsole(context, "cheat mspawn ddtraveler");  // 045_traveler.py: hooded, stands still, no dialogue
		Later(context, 3000, TravelerGoesToFire);
	}
}

// The hero playing now: while a game runs the other heroes are set aside, so the save folder
// holds only this one (and a hero just made).
auto CurrentHero() -> std::string {
	if (!g_currentHero.empty()) {
		return g_currentHero;
	}
	const auto names = Camp::HeroNames(g_live);
	return names.size() == 1 ? names.front() : PlayedHero();
}

// ---- Camp heroes by the town fire ---------------------------------------------------------
// The save's other heroes rest around the Rogue Encampment's fire as figures (046_camp_heroes.py:
// a monster row per class and armor weight that wears the class's own body). They only stand
// there; the game drops them when the hero leaves the game. Placed once per game, after the
// position search found the fire.

bool g_campHeroesPlaced = false;
std::vector<std::string> g_campHeroQueue;  // console commands still to run, oldest first

// Spots around the fire's middle, in sub-tiles: a ring with room for all 7 other classes.
constexpr int CampRing[][2] = { { 9, 0 }, { 0, 9 }, { -9, 0 }, { 0, -9 }, { 7, 7 }, { -7, -7 }, { 7, -7 }, { -7, 7 } };

// "lit", "med" or "hvy": how heavy the body armor the hero last wore is (camp_data.txt).
auto ArmorWeight(const std::string& gear) -> const char* {
	static const char* const names[] = { "lit", "med", "hvy" };
	std::istringstream words(gear);
	std::string word;
	while (words >> word) {
		const size_t colon = word.find(':');
		const ItemLook* look = colon == std::string::npos ? nullptr : LookOf(word.substr(colon + 1));
		if (look != nullptr && look->kind == 'b') {
			const int torso = std::atoi(look->value.c_str());
			return names[std::clamp(torso, 0, 2)];
		}
	}
	return names[0];
}

void __cdecl RunCampHeroQueue(const D2RL::PluginContext* context, void*) noexcept {
	const Lock lock(g_mutex);
	if (g_campHeroQueue.empty() || !g_inGame || g_levelId != RogueEncampment) {
		g_campHeroQueue.clear();
		return;
	}
	RunConsole(context, g_campHeroQueue.front().c_str());
	g_campHeroQueue.erase(g_campHeroQueue.begin());
	if (!g_campHeroQueue.empty()) {
		Later(context, 150, RunCampHeroQueue);
	}
}

void PlaceCampHeroes(const D2RL::PluginContext* context) {
	if (g_campHeroesPlaced || !g_inGame || g_levelId != RogueEncampment) {
		return;
	}
	const GameMemory::Pos fire = GameMemory::Fire();
	if (!fire.ok) {
		return;
	}
	g_campHeroesPlaced = true;
	const std::string playing = CurrentHero();
	int n = 0;
	std::string names;
	for (const auto& hero : g_camp.heroes) {
		if (hero.name == playing || hero.cls < 0 || n >= static_cast<int>(std::size(CampRing))) {
			continue;
		}
		std::string cls = Camp::ClassNames[hero.cls];
		std::transform(cls.begin(), cls.end(), cls.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		g_campHeroQueue.push_back("location " + std::to_string(fire.x + CampRing[n][0]) + "," + std::to_string(fire.y + CampRing[n][1]));
		g_campHeroQueue.push_back("cheat mspawn ddhero_" + cls + "_" + ArmorWeight(hero.gear));
		names += (names.empty() ? "" : ", ") + hero.name;
		++n;
	}
	if (n == 0) {
		return;
	}
	g_campHeroQueue.push_back("location player");
	Log(context, "Camp: %d other hero(es) rest by the fire: %s.", n, names.c_str());
	Later(context, 50, RunCampHeroQueue);
}

void __cdecl LogMemoryScan(const D2RL::PluginContext* context, void*) noexcept {
	if (GameMemory::Scanning()) {
		Later(context, 1000, LogMemoryScan);
		return;
	}
	Log(context, "Camp: %s", GameMemory::Report().c_str());
	const Lock lock(g_mutex);
	PlaceCampHeroes(context);
}

// In the Rogue Encampment: find the hero and the fire in memory (read only, gamememory.h).
void __cdecl ScanTown(const D2RL::PluginContext* context, void*) noexcept {
	const Lock lock(g_mutex);
	if (!g_inGame || g_levelId != RogueEncampment) {
		return;
	}
	if (!GameMemory::BuildMatches()) {
		Log(context, "Camp: game build %s is not the frozen build; positions are not read.", GameMemory::BuildFound().c_str());
		return;
	}
	GameMemory::StartScan(CurrentHero());
	Later(context, 1000, LogMemoryScan);
}

// The fire and the town's stash chest both open the stash window. The hero walks up to what
// was clicked before it opens, so whichever of the two the hero stands nearer was used. The
// chest is only about 14 sub-tiles from the fire's middle, so a plain distance cutoff is not
// enough (0.11.42 counted the chest as the fire). If the chest wasn't found, only a hero right
// at the pit's edge (the fire is used from about 6 sub-tiles) counts as the fire.
// Unknown positions: the stash.
constexpr long long FireReach = 9;

auto AtFire(const D2RL::PluginContext* context) -> bool {
	const GameMemory::Pos hero = GameMemory::Hero();
	const GameMemory::Pos fire = GameMemory::Fire();
	if (!hero.ok || !fire.ok) {
		Log(context, "Camp: stash window in town, positions unknown (hero %s, fire %s): treated as the stash.", hero.ok ? "ok" : "-",
			fire.ok ? "ok" : "-");
		if (!GameMemory::Scanning()) {
			GameMemory::StartScan(CurrentHero());
		}
		return false;
	}
	const GameMemory::Pos stash = GameMemory::Stash();
	const long long fx = hero.x - fire.x, fy = hero.y - fire.y;
	const long long toFire = fx * fx + fy * fy;
	bool atFire = false;
	if (stash.ok) {
		const long long sx = hero.x - stash.x, sy = hero.y - stash.y;
		const long long toStash = sx * sx + sy * sy;
		atFire = toFire < toStash;
		Log(context, "Camp: stash window in town, hero at %d,%d: %.1f sub-tiles from the fire (%d,%d), %.1f from the stash chest (%d,%d): %s.",
			hero.x, hero.y, std::sqrt(static_cast<double>(toFire)), fire.x, fire.y, std::sqrt(static_cast<double>(toStash)), stash.x, stash.y,
			atFire ? "the fire" : "the stash");
	} else {
		atFire = toFire <= FireReach * FireReach;
		Log(context, "Camp: stash window in town, hero at %d,%d, fire at %d,%d (%.1f sub-tiles), stash chest not found: %s.", hero.x, hero.y,
			fire.x, fire.y, std::sqrt(static_cast<double>(toFire)), atFire ? "the fire" : "the stash");
	}
	return atFire;
}

// Where the portal opens: across the fire from the hero, about 4 sub-tiles past the fire's
// middle. Without positions, at the hero's feet.
void PlacePortal(const D2RL::PluginContext* context) {
	const GameMemory::Pos hero = GameMemory::Hero();
	const GameMemory::Pos fire = GameMemory::Fire();
	if (!hero.ok || !fire.ok) {
		Log(context, "Camp: positions unknown (hero %s, fire %s); the portal opens where the hero stands.", hero.ok ? "ok" : "-", fire.ok ? "ok" : "-");
		RunConsole(context, "location player");
		return;
	}
	double dx = fire.x - hero.x, dy = fire.y - hero.y;
	const double length = std::sqrt(dx * dx + dy * dy);
	if (length < 0.5) {
		dx = 1.0, dy = 1.0;
	} else {
		dx /= length, dy /= length;
	}
	const int x = fire.x + static_cast<int>(std::lround(dx * 4.0));
	const int y = fire.y + static_cast<int>(std::lround(dy * 4.0));
	char command[64];
	std::snprintf(command, sizeof(command), "location %d,%d", x, y);
	Log(context, "Camp: hero at %d,%d, fire at %d,%d; portal at %d,%d.", hero.x, hero.y, fire.x, fire.y, x, y);
	RunConsole(context, command);
}

void __cdecl TravelerPortal(const D2RL::PluginContext* context, void*) noexcept {
	const Lock lock(g_mutex);
	if (!g_inGame || !g_travelerArrives) {
		return;
	}
	PlacePortal(context);
	RunConsole(context, "cheat ospawn 189");  // objects.txt row 189: CainPortal
	Later(context, 1200, TravelerStepsOut);
}

// ---- Console ------------------------------------------------------------------------------

// dd-where: the positions the plugin reads, to compare with "location player" (test).
auto WhereCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	const GameMemory::Pos hero = GameMemory::Hero();
	const GameMemory::Pos fire = GameMemory::Fire();
	char line[160];
	std::snprintf(line, sizeof(line), "Hero %d,%d (%s). Fire %d,%d (%s).", hero.x, hero.y, hero.ok ? "read" : "not found", fire.x, fire.y, fire.ok ? "read" : "not found");
	command->plugin->WriteConsoleMessage(line);
	command->plugin->WriteConsoleMessage(GameMemory::Report().c_str());
	const GameMemory::Stat mf = GameMemory::HeroMagicFind();
	char mfLine[160];
	std::snprintf(mfLine, sizeof(mfLine), "Magic find %d%% (%s).", mf.value, mf.ok ? mf.where.c_str() : ("not read: " + mf.where).c_str());
	command->plugin->WriteConsoleMessage(mfLine);
	Log(command->plugin, "Camp: dd-where: %s %s %s", line, mfLine, GameMemory::Report().c_str());
	if (std::strcmp(command->args == nullptr ? "" : command->args, "scan") == 0) {
		GameMemory::StartScan(CurrentHero());
		command->plugin->WriteConsoleMessage("Scanning again; type dd-where in a few seconds.");
	}
	return D2RL::ConsoleCommandResult::Handled;
}

// dd-units: what the live game-state reader sees (gamestate.h). With no words: how many units of
// each kind are loaded, the hero, and the nearest objects. "dd-units <kind> [row]" lists the
// nearest units of one kind (players, monsters, objects, missiles, items, tiles), "dd-units find"
// looks for the unit list again. Everything also goes to the plugin log.
auto UnitsCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	try {
		const auto say = [&](const std::string& line) {
			command->plugin->WriteConsoleMessage(line.c_str());
			Log(command->plugin, "Units: %s", line.c_str());
		};
		std::string word, rowText;
		{
			std::istringstream args(command->args == nullptr ? "" : command->args);
			args >> word >> rowText;
		}
		if (word == "find") {
			GameState::Find();
		}
		if (!GameState::Ready()) {
			say(GameState::Status());
			return D2RL::ConsoleCommandResult::Handled;
		}
		std::string playing;
		{
			const Lock lock(g_mutex);
			playing = CurrentHero();
		}
		GameState::Unit hero = GameState::Player(playing);
		if (!hero) {
			hero = GameState::Player();
		}
		const auto describe = [&](const GameState::Unit& u) {
			char line[200];
			const long long dx = u.x - hero.x, dy = u.y - hero.y;
			std::snprintf(line, sizeof(line), "%s row %u #%u mode %u at %d,%d (%.0f away)%s", GameState::KindName(u.kind), u.row, u.id, u.mode,
				u.x, u.y, std::sqrt(static_cast<double>(dx * dx + dy * dy)), u.placed ? "" : " not placed");
			std::string text = line;
			if (u.kind == GameState::Kind::Player) {
				text += " " + u.name;
			}
			if (u.kind == GameState::Kind::Monster || u.kind == GameState::Kind::Player) {
				text += " level " + std::to_string(GameState::CharacterLevel(u)) + " life " + std::to_string(GameState::Life(u)) + (u.Alive() ? "" : " dead");
			}
			return text;
		};
		static constexpr const char* Words[] = { "players", "monsters", "objects", "missiles", "items", "tiles" };
		int kind = -1;
		for (int k = 0; k < 6; ++k) {
			if (word == Words[k] || word + "s" == Words[k]) {
				kind = k;
			}
		}
		if (kind < 0) {
			say(GameState::Status());
			std::string counts;
			for (int k = 0; k < 6; ++k) {
				counts += (k ? ", " : "") + std::to_string(GameState::Units(static_cast<GameState::Kind>(k)).size()) + " " + Words[k];
			}
			say("Loaded now: " + counts + ".");
			say(hero ? "Hero: " + describe(hero) + ", in area " + std::to_string(GameState::Level(hero)) + " (levels.txt)" : std::string("Hero: not found."));
			if (hero) {
				Log(command->plugin, "Units: hero %s", GameState::StatsReport(hero).c_str());
			}
			const GameState::Unit fire = GameState::Nearest(GameState::Kind::Object, 39, hero.x, hero.y);
			say(fire ? "Nearest town fire (objects row 39): " + describe(fire) : std::string("No town fire (objects row 39) loaded."));
			word = "objects", kind = 2;
		}
		auto units = rowText.empty() ? GameState::Units(static_cast<GameState::Kind>(kind))
		                             : GameState::Units(static_cast<GameState::Kind>(kind), static_cast<uint32_t>(std::strtoul(rowText.c_str(), nullptr, 10)));
		std::sort(units.begin(), units.end(), [&](const GameState::Unit& a, const GameState::Unit& b) {
			const long long da = (a.x - hero.x) * 1LL * (a.x - hero.x) + (a.y - hero.y) * 1LL * (a.y - hero.y);
			const long long db = (b.x - hero.x) * 1LL * (b.x - hero.x) + (b.y - hero.y) * 1LL * (b.y - hero.y);
			return a.placed != b.placed ? a.placed : da < db;
		});
		say(std::to_string(units.size()) + " " + Words[kind] + (rowText.empty() ? "" : " of row " + rowText) + "; nearest first:");
		for (std::size_t i = 0; i < units.size() && i < 10; ++i) {
			say("  " + describe(units[i]));
		}
	} catch (...) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	return D2RL::ConsoleCommandResult::Handled;
}

// dd-state: what the game-state API (gameapi.h) reads now, to check its values against the game.
// "dd-state" hero, world, nearest monsters, objects and items; "dd-state stats" every hero stat;
// "dd-state watch" logs changes (Game::Poll) every 250 ms until typed again; "dd-state tables"
// lists every unit-list-shaped address in the game's code (research: where missiles live);
// "dd-state skills" the hero's skills; "dd-state items" what the hero wears and carries;
// "dd-state players" every player, corpse and the mercenary; "dd-state counters" the run and
// stop counters ("dd-state counters reset" starts them again).

// The game-state API's one tick (gameapi.h, "The shared event feed"): every 250 ms on the UI
// thread, from the moment the plugin loads. It feeds every subscriber and the run counters.
void __cdecl TrackTick(const D2RL::PluginContext* context, void*) noexcept {
	try {
		Game::Tick();
	} catch (...) {
	}
	Later(context, 250, TrackTick);
}

// "dd-state watch": a subscriber that logs each event (not units coming and going: too many).
std::atomic<int> g_watchId { 0 };

void WatchEvent(const Game::Event& e, void* userData) {
	if (e.type == Game::EventType::UnitAppeared || e.type == Game::EventType::UnitGone) {
		return;
	}
	Log(static_cast<const D2RL::PluginContext*>(userData), "State: %s: %s row %u #%u at %d,%d (%d -> %d)", Game::EventName(e.type),
		GameState::KindName(e.unit.kind), e.unit.row, e.unit.id, e.unit.x, e.unit.y, e.before, e.after);
}

auto StateCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	try {
		const auto say = [&](const std::string& text) {
			std::size_t start = 0;
			while (start <= text.size()) {
				const std::size_t end = (std::min)(text.find('\n', start), text.size());
				const std::string line = text.substr(start, end - start);
				command->plugin->WriteConsoleMessage(line.c_str());
				Log(command->plugin, "State: %s", line.c_str());
				start = end + 1;
			}
		};
		const std::string args = command->args == nullptr ? "" : command->args;
		if (args == "tables") {
			say(GameState::TablesReport());
			return D2RL::ConsoleCommandResult::Handled;
		}
		if (args == "watch") {
			if (const int id = g_watchId.exchange(0); id != 0) {
				Game::Unsubscribe(id);
				say("Stopped watching.");
			} else {
				g_watchId = Game::Subscribe(WatchEvent, const_cast<D2RL::PluginContext*>(command->plugin));
				say("Watching for changes (hero died, object used, monster died, area, hits); see the log. 'dd-state watch' stops.");
			}
			return D2RL::ConsoleCommandResult::Handled;
		}
		if (args == "counters" || args == "counters reset") {
			if (args == "counters reset") {
				Game::ResetCounters();
				say("Counters reset.");
			}
			const Game::CounterSet c = Game::Counters();
			const auto line = [&](const char* name, const Game::Tally& t) {
				char text[320];
				std::snprintf(text, sizeof(text),
					"%s: %.0f s; kills %d (normal %d, champion %d, unique %d, boss %d); deaths hero %d, merc %d; gold +%lld; items found %d; "
					"levels +%d; shrines %d; chests %d.",
					name, t.seconds, t.kills, t.killsNormal, t.killsChampion, t.killsUnique, t.killsBoss, t.heroDeaths, t.mercDeaths, t.goldGained,
					t.itemsFound, t.levelsGained, t.shrinesUsed, t.chestsOpened);
				say(text);
			};
			line("Run", c.run);
			line(("Stop (area " + std::to_string(c.stopArea) + ")").c_str(), c.stop);
			return D2RL::ConsoleCommandResult::Handled;
		}
		if (!GameState::Ready()) {
			say(GameState::Status());
			return D2RL::ConsoleCommandResult::Handled;
		}
		const Game::HeroDetails h = Game::HeroInfo();
		if (!h.unit) {
			say("No hero found.");
			return D2RL::ConsoleCommandResult::Handled;
		}
		if (args == "states") {
			say(Game::StatesReport(h.unit));
			const auto states = Game::States(h.unit);
			say(std::to_string(states.size()) + " states on the hero:");
			for (const Game::StateDetails& d : states) {
				std::string line = "  state " + std::to_string(d.state) + " from skill " + std::to_string(d.skill) + " level " + std::to_string(d.skillLevel) +
				                   ", ends at frame " + std::to_string(d.endFrame) + ", on for " + std::to_string(static_cast<int>(d.activeSeconds)) + " s; stats";
				for (const GameState::StatEntry& e : d.stats) {
					line += " " + std::to_string(e.id) + "=" + std::to_string(e.value);
				}
				say(line);
			}
			return D2RL::ConsoleCommandResult::Handled;
		}
		if (args == "skills") {
			const Game::SkillSet set = Game::HeroSkills();
			std::string line = set.found ? "Skills: left " + std::to_string(set.left) + ", right " + std::to_string(set.right) + "; skill/points:"
			                             : std::string("Skills: skill list not found.");
			for (const Game::SkillDetails& s : set.skills) {
				line += " " + std::to_string(s.id) + "/" + std::to_string(s.baseLevel);
			}
			say(line);
			say(Game::SkillsReport());
			return D2RL::ConsoleCommandResult::Handled;
		}
		if (args == "items") {
			static const char* const places[] = { "stored", "equipped", "belt", "ground", "cursor", "dropping", "socketed" };
			const auto items = Game::HeroItems();
			say(std::to_string(items.size()) + " items the hero owns:");
			for (const Game::ItemDetails& i : items) {
				const int place = static_cast<int>(i.place);
				char text[200];
				std::snprintf(text, sizeof(text), "  item row %u #%u %s, slot %d, page %d, quality %d, ilvl %d%s%s%s", i.unit.row, i.unit.id,
					place >= 0 && place <= 6 ? places[place] : "unknown", i.bodySlot, i.page, i.quality, i.itemLevel, i.Identified() ? "" : ", unidentified",
					i.Socketed() ? ", socketed" : "", i.Ethereal() ? ", ethereal" : "");
				say(text);
			}
			return D2RL::ConsoleCommandResult::Handled;
		}
		if (args == "players") {
			for (const Game::PlayerDetails& p : Game::Players()) {
				char text[200];
				std::snprintf(text, sizeof(text), "Player %s #%u, class %d, level %d, at %d,%d, mode %u (%s)%s%s", p.name.c_str(), p.unit.id, p.classId, p.level,
					p.unit.x, p.unit.y, p.unit.mode, Game::PlayerModeName(p.unit.mode), p.hero ? ", the hero" : "", p.corpse ? ", corpse" : "");
				say(text);
			}
			if (const auto merc = Game::Mercenary()) {
				char text[160];
				std::snprintf(text, sizeof(text), "Mercenary: row %u #%u, life %d/%d, at %d,%d%s", merc->unit.row, merc->unit.id, merc->life, merc->maxLife,
					merc->unit.x, merc->unit.y, merc->alive ? "" : ", DEAD");
				say(text);
			} else {
				say("No mercenary loaded.");
			}
			return D2RL::ConsoleCommandResult::Handled;
		}
		if (args == "stats") {
			std::string line;
			for (const GameState::StatEntry& e : GameState::Stats(h.unit, true)) {
				line += " " + std::to_string(e.id) + (e.param ? "/" + std::to_string(e.param) : std::string()) + "=" + std::to_string(e.value);
				if (line.size() > 180) {
					say("Stats:" + line);
					line.clear();
				}
			}
			say("Stats:" + line);
			return D2RL::ConsoleCommandResult::Handled;
		}
		const Game::WorldDetails w = Game::World();
		char text[320];
		const Game::SkillSet skills = Game::HeroSkills();
		std::snprintf(text, sizeof(text),
			"Hero %s, class %d, level %d, life %d/%d, mana %d/%d, exp %lld, gold %d (stash %d), at %d,%d, mode %u (%s%s)%s; skills left %d right %d.",
			h.name.c_str(), h.classId, h.level, h.life, h.maxLife, h.mana, h.maxMana, h.experience, h.gold, h.stashGold, h.unit.x, h.unit.y, h.unit.mode,
			Game::PlayerModeName(h.unit.mode), Game::InCombatMode(h.unit.mode) ? ", fighting" : "", h.dead ? " DEAD" : "", skills.left, skills.right);
		say(text);
		std::snprintf(text, sizeof(text), "World: area %d, act %d, difficulty %d. Resists fire %d, cold %d, lightning %d, poison %d; magic find %d.", w.area,
			w.act, w.difficulty, Game::Stat(h.unit, Game::StatId::FireResist).value_or(0), Game::Stat(h.unit, Game::StatId::ColdResist).value_or(0),
			Game::Stat(h.unit, Game::StatId::LightningResist).value_or(0), Game::Stat(h.unit, Game::StatId::PoisonResist).value_or(0),
			Game::Stat(h.unit, Game::StatId::MagicFind).value_or(0));
		say(text);
		std::snprintf(text, sizeof(text), "Speed: run/walk %+d%%, attack %+d%%, cast %+d%%, hit recovery %+d%%; gold find %d.",
			Game::Stat(h.unit, Game::StatId::MoveSpeed).value_or(0) + Game::Stat(h.unit, Game::StatId::VelocityPercent).value_or(0),
			Game::Stat(h.unit, Game::StatId::AttackSpeed).value_or(0), Game::Stat(h.unit, Game::StatId::CastSpeed).value_or(0),
			Game::Stat(h.unit, Game::StatId::HitRecovery).value_or(0), Game::Stat(h.unit, Game::StatId::GoldFind).value_or(0));
		say(text);
		const auto monsters = Game::Monsters(60);
		say(std::to_string(monsters.size()) + " live monsters within 60:");
		for (std::size_t i = 0; i < monsters.size() && i < 6; ++i) {
			const auto& m = monsters[i];
			std::string kind = m.superUnique ? "super unique" : m.unique ? "unique" : m.champion ? "champion" : m.minion ? "minion" : "normal";
			std::string ench;
			for (const int e : m.enchantments) {
				ench += (ench.empty() ? " enchant" : ",") + std::to_string(e);
			}
			std::snprintf(text, sizeof(text), "  %s row %u #%u %s, life %d/%d, at %d,%d%s", Game::ClassName(m.cls), m.unit.row, m.unit.id, kind.c_str(), m.life,
				m.maxLife, m.unit.x, m.unit.y, ench.c_str());
			say(text);
		}
		const auto objects = Game::Objects(30);
		std::string line = std::to_string(objects.size()) + " objects within 30:";
		for (std::size_t i = 0; i < objects.size() && i < 8; ++i) {
			line += std::string(" ") + Game::KindName(Game::KindOf(objects[i].row)) + " row " + std::to_string(objects[i].row) + " mode " +
			        std::to_string(objects[i].mode) + ";";
		}
		say(line);
		const auto items = Game::GroundItems(30);
		line = std::to_string(items.size()) + " items on the ground within 30:";
		for (std::size_t i = 0; i < items.size() && i < 8; ++i) {
			line += " row " + std::to_string(items[i].unit.row) + " quality " + std::to_string(items[i].quality) + " ilvl " +
			        std::to_string(items[i].itemLevel) + ";";
		}
		say(line);
	} catch (...) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	return D2RL::ConsoleCommandResult::Handled;
}

// dd-set: test the game-state WRITES (gamewrite.h).
//   dd-set copies                     find every copy of the hero in memory (1-2 s) and list them
//   dd-set stat <id> <value>          set a hero stat the hero already has (e.g. 80 magic find)
//   dd-set hold <id> <value>          the same, written again every 250 ms; "dd-set hold off" stops
//   dd-set state <row> end <frame>    change when a buff/debuff ends
//   dd-set state <row> stat <id> <v>  change what a buff/debuff gives (e.g. Slowed's 67 velocitypercent)
std::mutex                g_copiesLock;
uint32_t                  g_copiesOf = 0;
std::vector<uintptr_t>    g_copies;
std::atomic<bool>         g_holding { false };
std::atomic<int>          g_holdStat { 0 }, g_holdValue { 0 };

auto HeroCopies(const Game::Unit& hero, bool fresh) -> std::vector<uintptr_t> {
	const std::lock_guard<std::mutex> guard(g_copiesLock);
	if (fresh || g_copiesOf != hero.id || g_copies.empty() || g_copies.front() != hero.address) {
		g_copies = GameWrite::Copies(hero);
		g_copiesOf = hero.id;
	}
	return g_copies;
}

void __cdecl HoldTick(const D2RL::PluginContext* context, void*) noexcept {
	if (!g_holding) {
		return;
	}
	try {
		const Game::Unit hero = Game::Hero();
		if (hero) {
			GameWrite::SetStat(HeroCopies(hero, false), static_cast<uint16_t>(g_holdStat.load()), g_holdValue.load());
		}
	} catch (...) {
	}
	Later(context, 250, HoldTick);
}

auto SetCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	try {
		const auto say = [&](const std::string& line) {
			command->plugin->WriteConsoleMessage(line.c_str());
			Log(command->plugin, "Write: %s", line.c_str());
		};
		std::istringstream in(command->args == nullptr ? "" : command->args);
		std::string what;
		in >> what;
		if (what == "hold") {
			std::string a;
			int value = 0;
			in >> a >> value;
			if (a == "off" || a.empty()) {
				g_holding = false;
				say("Stopped holding.");
				return D2RL::ConsoleCommandResult::Handled;
			}
			g_holdStat = std::atoi(a.c_str()), g_holdValue = value;
			if (!g_holding.exchange(true)) {
				Later(command->plugin, 250, HoldTick);
			}
			say("Holding stat " + a + " at " + std::to_string(value) + " every 250 ms ('dd-set hold off' stops).");
			return D2RL::ConsoleCommandResult::Handled;
		}
		if (!GameMemory::BuildMatches() || !GameState::Ready()) {
			say("Writes are off: not the frozen game build, or no game loaded.");
			return D2RL::ConsoleCommandResult::Handled;
		}
		const Game::Unit hero = Game::Hero();
		if (!hero) {
			say("No hero found.");
			return D2RL::ConsoleCommandResult::Handled;
		}
		if (what == "copies") {
			say(GameWrite::CopiesReport(hero, HeroCopies(hero, true)));
			return D2RL::ConsoleCommandResult::Handled;
		}
		if (what == "stat") {
			int id = -1, value = 0;
			if (!(in >> id >> value) || id < 0) {
				say("Usage: dd-set stat <itemstatcost row> <value>");
				return D2RL::ConsoleCommandResult::Handled;
			}
			const int n = GameWrite::SetStat(HeroCopies(hero, false), static_cast<uint16_t>(id), value);
			say("Stat " + std::to_string(id) + " set to " + std::to_string(value) + " in " + std::to_string(n) + " places" +
			    (n == 0 ? " (the hero has no such stat now; writes can only change existing ones)." : "."));
			return D2RL::ConsoleCommandResult::Handled;
		}
		if (what == "state") {
			int row = -1;
			std::string field;
			in >> row >> field;
			int n = 0;
			if (field == "end") {
				long long frame = 0;
				in >> frame;
				n = GameWrite::SetStateEnd(HeroCopies(hero, false), row, static_cast<uint32_t>(frame));
			} else if (field == "stat") {
				int id = -1, value = 0;
				in >> id >> value;
				n = GameWrite::SetStateStat(HeroCopies(hero, false), row, static_cast<uint16_t>(id), value);
			} else {
				say("Usage: dd-set state <states.txt row> end <frame> | stat <id> <value>");
				return D2RL::ConsoleCommandResult::Handled;
			}
			say("State " + std::to_string(row) + ": changed " + std::to_string(n) + " places.");
			return D2RL::ConsoleCommandResult::Handled;
		}
		say("Usage: dd-set copies | stat <id> <value> | hold <id> <value> | hold off | state <row> end <frame> | state <row> stat <id> <value>");
	} catch (...) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	return D2RL::ConsoleCommandResult::Handled;
}

auto CampCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	try {
		const Lock lock(g_mutex);
		command->plugin->WriteConsoleMessage(Describe().c_str());
		if (g_inGame && IsTown(g_levelId)) {
			ShowCamp(command->plugin, nullptr);
		}
	} catch (...) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	return D2RL::ConsoleCommandResult::Handled;
}

auto SlotCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	try {
		const Lock lock(g_mutex);
		const int slot = command->args != nullptr ? std::atoi(command->args) : 0;
		if (slot < 1 || slot > Camp::SlotCount) {
			command->plugin->WriteConsoleMessage("Usage: dd-slot <1-3> (at the main menu).");
			return D2RL::ConsoleCommandResult::InvalidArguments;
		}
		command->plugin->WriteConsoleMessage(PickSlot(command->plugin, slot).c_str());
	} catch (...) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	return D2RL::ConsoleCommandResult::Handled;
}

auto TravelerCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	try {
		const Lock lock(g_mutex);
		if (!g_inGame || !IsTown(g_levelId)) {
			command->plugin->WriteConsoleMessage("Stand in town first.");
			return D2RL::ConsoleCommandResult::Failed;
		}
		if (g_camp.LockedClasses().empty()) {
			command->plugin->WriteConsoleMessage("Every class is already in the camp.");
			return D2RL::ConsoleCommandResult::Failed;
		}
		ShowTraveler(command->plugin, nullptr);
	} catch (...) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	return D2RL::ConsoleCommandResult::Handled;
}

// ---- Setup --------------------------------------------------------------------------------

auto FindSaveFolder() -> fs::path {
	PWSTR saved = nullptr;
	fs::path out;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &saved)) && saved != nullptr) {
		out = fs::path(saved) / L"Diablo II Resurrected" / L"mods" / L"darkestdiablo";  // modinfo.json savepath
	}
	CoTaskMemFree(saved);
	return out;
}

// The save slot screen, with who is by each fire written in. Its template is made by
// tools/camp_layouts.py; the loader reads the layout when the panel opens.

// Takes a widget (with everything in it) out of a layout made by tools/camp_layouts.py, where every
// widget starts with its type and name.
void RemoveWidget(std::string& json, const std::string& name) {
	const size_t at = json.find("\", \"name\": \"" + name + "\"");
	const size_t start = at == std::string::npos ? at : json.rfind("{\"type\"", at);
	if (start == std::string::npos) {
		return;
	}
	int depth = 0;
	bool quoted = false;
	for (size_t i = start; i < json.size(); ++i) {
		const char c = json[i];
		if (quoted) {
			if (c == '\\') {
				++i;
			} else if (c == '"') {
				quoted = false;
			}
		} else if (c == '"') {
			quoted = true;
		} else if (c == '{') {
			++depth;
		} else if (c == '}' && --depth == 0) {
			size_t from = start, to = i + 1;
			if (const size_t next = json.find_first_not_of(" \n\t", to); next != std::string::npos && json[next] == ',') {
				to = next + 1;
			} else if (const size_t before = json.find_last_not_of(" \n\t", from - 1); before != std::string::npos && json[before] == ',') {
				from = before;
			}
			json.erase(from, to - from);
			return;
		}
	}
}

auto SlotsJson() -> std::string {
	std::string json = Camp::ReadFile(g_dataDir / "SaveSlots.layout.json");
	if (json.empty()) {
		return {};
	}
	const bool asking = g_confirmSlot != 0 || g_slotNotice;
	const auto keep = [&](const std::string& name, bool shown) {
		if (!shown) {
			RemoveWidget(json, name);
		}
	};
	for (int slot = 1; slot <= Camp::SlotCount; ++slot) {
		const std::string n = std::to_string(slot);
		const bool started = slot == g_slot ? g_camp.started : Camp::LoadState(g_live, slot).started;
		keep("Slot" + n, !asking);
		keep("Delve" + n, !asking);
		keep("Slot" + n + "Last", !asking && started && slot == g_slot);
		keep("Slot" + n + "Delete", !asking && started);
		keep("Confirm" + n + "A", !g_slotNotice && g_confirmSlot == slot && g_confirmStep == 1);
		keep("Confirm" + n + "B", !g_slotNotice && g_confirmSlot == slot && g_confirmStep == 2);
	}
	keep("Exit", g_confirmSlot == 0);
	keep("NoticeBack", g_slotNotice && Camp::ReadPending(g_live).deletes.empty());
	keep("AskYes", !g_slotNotice && g_confirmStep == 1);
	keep("AskNo", !g_slotNotice && g_confirmStep == 1);
	keep("SureNo", !g_slotNotice && g_confirmStep == 2);
	keep("SureYes", !g_slotNotice && g_confirmStep == 2);
	keep("RestartNotice", g_slotNotice);
	for (int slot = 1; slot <= Camp::SlotCount; ++slot) {
		const Camp::State camp = slot == g_slot ? g_camp : Camp::LoadState(g_live, slot);
		for (const auto& [mark, value] : Camp::DelveText(camp)) {
			const std::string full = "@@S" + std::to_string(slot) + mark + "@@";
			for (size_t at = json.find(full); at != std::string::npos; at = json.find(full, at + value.size())) {
				json.replace(at, full.size(), value);
			}
		}
	}
	// Over the fire the screen is darkened, not hidden; over the game's own menu it is black.
	const std::string backdrop = "\"@@BACKDROP@@\"";
	if (const size_t at = json.find(backdrop); at != std::string::npos) {
		json.replace(at, backdrop.size(), g_atFire || g_emptyFire ? "[0.0,0.0,0.0,0.45]" : "[0.0,0.0,0.0,1.0]");
	}
	const std::string rootName = "\"darkestdiablo/SaveSlots\"";
	if (const size_t at = json.find(rootName); at != std::string::npos) {
		json.replace(at, rootName.size(), std::string("\"") + PanelNameMark + "\"");
	}
	if (const size_t end = json.rfind("]}"); end != std::string::npos) {
		json.insert(end, "," + VersionWidgetJson());
	}
	return json;
}

// Registers a panel whose layout is a file of the mod (made by tools/camp_layouts.py). The file
// is also handed to the loader from disk, in case it only looks at plugin resources.
auto RegisterPanel(const D2RL::PluginContext* context, const D2RL::ResourceService* resources, const char* localId) -> D2RL::Panels::RegistrationHandle {
	const std::string relative = std::string("data/global/ui/layouts/darkestdiablo/") + localId + "hd.json";
	if (resources != nullptr && context->modDirectory != nullptr) {
		const fs::path mod(context->modDirectory);
		for (const fs::path& base : { mod / "darkestdiablo.mpq", mod }) {
			const std::string bytes = Camp::ReadFile(base / relative);
			if (!bytes.empty()) {
				const D2RL::Resources::ResourceRegistration resource {
					.structSize = D2RL::Resources::ResourceRegistrationSize,
					.path       = relative.c_str(),
					.bytes      = bytes.data(),
					.byteCount  = static_cast<uint32_t>(bytes.size()),
				};
				D2RL::Resources::RegistrationHandle handle = D2RL::Resources::InvalidHandle;
				resources->registerResource(context, &resource, &handle);
				break;
			}
		}
	}
	const D2RL::Panels::PanelRegistration registration {
		.structSize = D2RL::Panels::PanelRegistrationSize,
		.flags      = D2RL::Panels::PanelFlags::CloseOnEscape,
		.localId    = localId,
	};
	D2RL::Panels::RegistrationHandle panel = D2RL::Panels::InvalidHandle;
	if (g_panels->registerPanel(context, &registration, &panel) != D2RL::Panels::Result::Success) {
		Log(context, "Camp: could not register the %s panel.", localId);
		return D2RL::Panels::InvalidHandle;
	}
	return panel;
}

auto StartsWith(const char* text, const char* prefix) noexcept -> bool {
	return std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

// A button of ours was clicked: PanelManager:OpenPanel:darkestdiablo/<action>. Returns false for
// actions that belong to another part of the plugin (the route map's travel buttons).
// The player said yes twice: the save is emptied (its files go to camp/trash, see camp.h) and
// the slot screen comes back with the slot shown as empty.
void DeleteSave(const D2RL::PluginContext* context, int slot) {
	if (g_inGame) {
		return;
	}
	Camp::Unbench(g_live, g_slot);
	const std::string stamp = std::to_string(static_cast<long long>(std::time(nullptr)));
	const bool joined = Camp::StashJoined(g_live);
	if (joined) {  // its pages are kept beside the trash
		StashFile::CopyPages(g_live, slot, g_live / "camp" / "trash" / ("slot" + std::to_string(slot) + "-" + stamp + "-stash.d2i"));
	}
	const bool modern = Camp::ModernStashJoined(g_live);
	if (modern) {
		StashFile::CopyPages(g_live, slot, g_live / "camp" / "trash" / ("slot" + std::to_string(slot) + "-" + stamp + "-stash-modern.d2i"), true);
	}
	if (slot == g_stashSlot && !joined) {  // the game holds this save's stash: it would follow into a new save here
		g_confirmSlot = 0;
		g_confirmStep = 0;
		Camp::Pending change;
		change.deletes.emplace_back(slot, stamp);
		RestartWith(context, change);
		return;
	}
	std::string error;
	if (Camp::DeleteSlot(g_live, slot, stamp, error)) {
		Log(context, "Camp: save %d was deleted (its files are kept in camp/trash).", slot);
		if (modern) {
			ModernStash::ForgetGold(g_live, slot);
			auto wipes = StashFile::Wipes(g_live, true);
			wipes.insert(slot);
			if (g_stashSlot != 0 || !StashFile::BlankPages(g_live, slot, true)) {
				StashFile::SetWipes(g_live, wipes, true);
			}
		}
		if (joined) {
			StashFile::ForgetJoinedGold(g_live, slot);
			// While the game holds the stash, its pages are emptied when they are next opened (stash.cpp).
			auto wipes = StashFile::Wipes(g_live);
			wipes.insert(slot);
			if (g_stashSlot != 0 || !StashFile::BlankPages(g_live, slot)) {
				StashFile::SetWipes(g_live, wipes);
			}
		}
	} else {
		Log(context, "Camp: could not delete save %d: %s", slot, error.c_str());
	}
	g_confirmSlot = 0;
	g_confirmStep = 0;
	if (slot == g_slot) {
		g_camp = Camp::LoadState(g_live, g_slot);
		g_pendingClass = -1;
		g_pickedClass = -1;
		g_chosenHero = -1;
		g_slotChosen = false;
	}
	g_slotsOpen = false;
	PoolClose(context, g_slots);
	if (slot == g_slot && g_atFire) {
		RefreshFire(context);
	}
	Later(context, 300, ShowSlots);
}

auto OnAction(const D2RL::PluginContext* context, const char* action) -> bool {
	if (std::strcmp(action, "slot-delete-cancel") == 0) {
		Log(context, "Camp: Keep it pressed (question %d about save %d).", g_confirmStep, g_confirmSlot);
		g_confirmSlot = 0;
		g_confirmStep = 0;
		ShowSlotWidgets(context);
	} else if (std::strcmp(action, "slot-delete-yes") == 0) {
		if (g_confirmSlot != 0 && g_confirmStep == 2) {
			DeleteSave(context, g_confirmSlot);
		}
	} else if (std::strcmp(action, "slot-delete-sure") == 0) {
		if (g_confirmSlot != 0 && g_confirmStep == 1) {
			g_confirmStep = 2;
			Log(context, "Camp: asking once more before deleting save %d.", g_confirmSlot);
			ShowSlotWidgets(context);
		}
	} else if (std::strcmp(action, "slot-notice-back") == 0) {
		// Back from "start the game again": forget the save that was picked, unless a delete waits too.
		Camp::Pending pending = Camp::ReadPending(g_live);
		if (g_slotNotice && pending.deletes.empty()) {
			std::error_code ec;
			fs::remove(Camp::PendingFile(g_live), ec);
			g_slotNotice = false;
			g_slotChosen = false;
			Log(context, "Camp: Back pressed at the restart notice; no save change waits%s.", ec ? " (camp/next.txt could not be removed)" : "");
			ShowSlotWidgets(context);
		}
	} else if ((g_confirmSlot != 0 || g_slotNotice) && StartsWith(action, "slot-")) {
		// a hidden button under the question still takes clicks: ignore it
	} else if (StartsWith(action, "slot-delete-")) {
		const int slot = std::atoi(action + 12);
		const bool started = slot == g_slot ? g_camp.started : slot >= 1 && slot <= Camp::SlotCount && Camp::LoadState(g_live, slot).started;
		if (slot >= 1 && slot <= Camp::SlotCount && started) {
			g_confirmSlot = slot;
			g_confirmStep = 1;
			ShowSlotWidgets(context);
			Log(context, "Camp: asking before deleting save %d.", slot);
		}
	} else if (StartsWith(action, "slot-")) {
		PickSlot(context, std::atoi(action + 5));
	} else if (StartsWith(action, "recruit-")) {
		// The traveler's buttons say which place was picked (recruit-place-<n>); the class
		// offered there is in the camp's offer. A hidden button still takes clicks, so the
		// panel can't have one button per class.
		int cls = -1;
		if (StartsWith(action, "recruit-place-")) {
			const int place = std::atoi(action + 14);
			if (place >= 0 && place < static_cast<int>(g_camp.offer.size())) {
				cls = g_camp.offer[place];
			}
		} else {
			cls = Camp::ClassFromName(action + 8);
		}
		Close(context, g_travelerPanel);
		if (cls >= 0 && Camp::Recruit(g_camp, cls)) {
			Camp::SaveState(g_live, g_slot, g_camp);
			Log(context, "Camp: the %s joins the camp as a recruit. %s", Camp::ClassNames[cls], Describe().c_str());
		} else {
			Log(context, "Camp: %s was not on offer. %s", action, Describe().c_str());
		}
	} else if (std::strcmp(action, "traveler-accept") == 0) {
		// The classes not taken leave; the Campfire reopens with the camp's heroes and the new
		// class, ready to name it.
		const int cls = g_selectedClass;
		if (g_travelerMode && cls >= 0 && Camp::Recruit(g_camp, cls)) {
			Camp::SaveState(g_live, g_slot, g_camp);
			g_travelerMode = false;
			g_justRecruited = cls;
			Log(context, "Camp: the %s joins the camp as a recruit. %s", Camp::ClassNames[cls], Describe().c_str());
			RefreshFire(context);
		} else {
			Log(context, "Camp: Accept pressed with nothing chosen.");
		}
	} else if (std::strcmp(action, "traveler-not-now") == 0) {
		// The traveler keeps waiting (the offer stays) and is back next game; the camp shows as usual.
		g_travelerLater = true;
		g_travelerMode = false;
		Log(context, "Camp: the traveler waits for later.");
		RefreshFire(context);
	} else if (std::strcmp(action, "traveler-later") == 0) {
		Close(context, g_travelerPanel);
		g_camp.offer.clear();
		Camp::SaveState(g_live, g_slot, g_camp);
		Log(context, "Camp: the traveler moves on.");
	} else if (std::strcmp(action, "open-campfire") == 0) {
		ShowCamp(context, nullptr);
	} else if (StartsWith(action, "play-")) {  // a name plate: pick that hero
		g_chosenHero = Camp::ClassFromName(action + 5);
		g_createMode = false;
		ShowDeleteMode(context);
	} else if (Ascension::OnAction(context, action)) {
		return true;
	} else if (std::strcmp(action, "ascend") == 0) {
		if (g_atFire && !g_inGame && !g_deleteMode && !g_createMode && g_camp.HeroOf(g_chosenHero) != nullptr) {
			g_ascendAsk = g_chosenHero;
			RefreshFire(context);
		}
	} else if (std::strcmp(action, "ascend-cancel") == 0) {
		g_ascendAsk = -1;
		RefreshFire(context);
	} else if (std::strcmp(action, "ascend-accept") == 0) {
		const int cls = g_ascendAsk;
		g_ascendAsk = -1;
		if (const Camp::Hero* hero = g_camp.HeroOf(cls); hero != nullptr && !g_inGame) {
			Ascension::Plan(hero->name, ActOfHero(hero->name));
			PlayHero(context, cls);
		}
	} else if (std::strcmp(action, "enter") == 0) {
		Ascension::Plan("", 0);
		PlayHero(context, g_chosenHero);
	} else if (StartsWith(action, "bury-")) {
		BuryHero(context, Camp::ClassFromName(action + 5));
	} else if (std::strcmp(action, "delete-mode") == 0) {
		g_deleteMode = !g_deleteMode;
		g_createMode = g_camp.heroes.empty();
		ShowDeleteMode(context);
	} else if (std::strcmp(action, "to-campfire") == 0) {
		RecordGear(context, nullptr);
		Close(context, g_campPanel);
		if (g_threads != nullptr) {
			g_threads->runOnUiThread(context, LeaveGame, nullptr);
		}
	} else {
		return false;
	}
	return true;
}

}  // namespace

auto Load(const D2RL::PluginContext* context) noexcept -> bool {
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
			if (g_lootData.empty() && fs::exists(place / "loot_data.txt", ec)) {
				g_lootData = Camp::ReadFile(place / "loot_data.txt");
			}
		}
		std::istringstream in(g_lootData);
		g_shapes.Read(in);
	} catch (...) {
	}
	OpenStateLog();  // darkestdiablo-state.log: dd-state results survive restarts
	try {
		const Lock lock(g_mutex);
		g_live = FindSaveFolder();
		if (g_live.empty()) {
			context->LogError("Camp: could not find the Saved Games folder. The Campfire is off.");
			return true;
		}
		std::error_code ec;
		fs::create_directories(g_live / "camp", ec);
		ReadOptions();
		// A save change noted in the last session (restart for another save): made now, before the
		// game reads the save folder. The bench belongs to the old save, so it comes back first.
		if (fs::exists(Camp::PendingFile(g_live), ec)) {
			const Camp::Pending pending = Camp::ReadPending(g_live);
			Camp::Unbench(g_live, Camp::ActiveSlot(g_live));
			for (const auto& line : Camp::ApplyPending(g_live)) {
				Log(context, "Camp: at start, %s", line.c_str());
			}
			g_openedAtStart = pending.open != 0 && Camp::ActiveSlot(g_live) == pending.open;
		}
		// The Warlock rules' stash file, the same way (modernstash.h). `laidOut`: asked before the other is joined.
		{
			const std::string stamp = std::to_string(static_cast<long long>(std::time(nullptr)));
			const bool laidOut = Camp::StashJoined(g_live);
			std::string said;
			const bool done = ModernStash::Join(g_live, stamp, laidOut, said);
			if (!said.empty()) {
				Log(context, "Camp: Warlock stash: %s", said.c_str());
			}
			if (done) {
				auto wipes = StashFile::Wipes(g_live, true);
				for (const int slot : std::set<int>(wipes)) {
					if (StashFile::BlankPages(g_live, slot, true)) {
						wipes.erase(slot);
					}
				}
				StashFile::SetWipes(g_live, wipes, true);
			}
			ModernStash::EmptyTabs(g_live, stamp, said);
			if (!said.empty()) {
				Log(context, "Camp: Warlock stash: %s", said.c_str());
			}
		}
		// All three stashes in one file (stashfile.h), and the pages of saves deleted last session.
		if (Camp::StashJoined(g_live)) {
			auto wipes = StashFile::Wipes(g_live);
			for (const int slot : std::set<int>(wipes)) {
				if (StashFile::BlankPages(g_live, slot)) {
					wipes.erase(slot);
					Log(context, "Camp: emptied the stash pages of the deleted save %d.", slot);
				}
			}
			StashFile::SetWipes(g_live, wipes);
		} else {
			std::string said;
			if (StashFile::Join(g_live, std::to_string(static_cast<long long>(std::time(nullptr))), said)) {
				Log(context, "Camp: %s", said.c_str());
			} else {
				Log(context, "Camp: the stashes stay separate, so changing saves needs a restart: %s.", said.c_str());
			}
		}
		g_slot = Camp::ActiveSlot(g_live);
		g_camp = Camp::LoadState(g_live, g_slot);
		if (const int left = Camp::Unbench(g_live, g_slot); left > 0) {
			Log(context, "Camp: %d benched file(s) could not come back; see camp/slot%d/bench.", left, g_slot);
		}
		Reconcile(context, false);
		Log(context, "Camp: %s", Describe().c_str());
		ReadCampData(context);
		if (context->QueryService(&g_inventory) != D2RL::ServiceQueryResult::Success || !D2RL::HasInventoryServiceField(g_inventory, D2RL::InventoryServiceRequiredSize)) {
			g_inventory = nullptr;
		}
		if (context->QueryService(&g_items) != D2RL::ServiceQueryResult::Success || g_items == nullptr || g_items->getItemInfo == nullptr) {
			g_items = nullptr;
		}
		if (g_inventory == nullptr || g_items == nullptr) {
			context->LogWarn("Camp: item services missing; heroes at the Campfire show no gear.");
		}

		const D2RL::ResourceService* resources = nullptr;
		if (context->QueryService(&resources) != D2RL::ServiceQueryResult::Success || !D2RL::HasResourceServiceField(resources, D2RL::ResourceServiceRequiredSize)) {
			resources = nullptr;
		}
		if (context->QueryService(&g_threads) != D2RL::ServiceQueryResult::Success || !D2RL::HasThreadServiceField(g_threads, D2RL::ThreadServiceRequiredSize)) {
			g_threads = nullptr;
		}
		static std::atomic<bool> tracking { false };
		if (g_threads != nullptr && !tracking.exchange(true)) {
			Later(context, 250, TrackTick);  // the game-state API's event feed and counters (gameapi.h)
		}
		if (context->QueryService(&g_widgets) != D2RL::ServiceQueryResult::Success || !D2RL::HasWidgetServiceField(g_widgets, D2RL::WidgetServiceRequiredSize)) {
			g_widgets = nullptr;
			context->LogWarn("Camp: widget service missing; classes can't be hidden on the hero screen.");
		}
		if (context->QueryService(&g_panels) == D2RL::ServiceQueryResult::Success && D2RL::HasPanelServiceField(g_panels, D2RL::PanelServiceRequiredSize)) {
			g_resources = resources;
			PoolRegister(context, g_slots, D2RL::Panels::PanelFlags::None);
			g_travelerPanel = RegisterPanel(context, resources, TravelerPanel);
			g_campPanel     = RegisterPanel(context, resources, CampPanel);
			g_resources = resources;
			if (g_fireOn) {
				PoolRegister(context, g_fire, D2RL::Panels::PanelFlags::None);
				PoolRegister(context, g_top, D2RL::Panels::PanelFlags::None);
			}
			if (g_fireOn && RegisterFireLayout(context)) {
				const D2RL::Resources::ResourceRegistration curtainLayout {
					.structSize = D2RL::Resources::ResourceRegistrationSize,
					.path       = CurtainLayout,
					.bytes      = CurtainJson,
					.byteCount  = static_cast<uint32_t>(sizeof(CurtainJson) - 1),
				};
				D2RL::Resources::RegistrationHandle curtainHandle = D2RL::Resources::InvalidHandle;
				const D2RL::Panels::PanelRegistration curtain {
					.structSize = D2RL::Panels::PanelRegistrationSize,
					.flags      = D2RL::Panels::PanelFlags::None,
					.localId    = CurtainPanel,
				};
				if (resources->registerResource(context, &curtainLayout, &curtainHandle) != D2RL::Resources::Result::Success ||
					g_panels->registerPanel(context, &curtain, &g_curtainPanel) != D2RL::Panels::Result::Success) {
					g_curtainPanel = D2RL::Panels::InvalidHandle;
				}
			}
		} else {
			g_panels = nullptr;
			context->LogWarn("Camp: panel service missing; no slot screen, traveler or roster panel.");
		}
		if (g_openedAtStart) {  // the save was picked before the restart: straight to its Campfire
			g_slotChosen = true;
			g_wantFire = g_fireOn && g_fireReady;
		}
	} catch (const std::exception& error) {
		Log(context, "Camp: setup failed (%s). The Campfire is off.", error.what());
		g_live.clear();
		return true;
	}
	return context->RegisterConsoleCommand("dd-camp", CampCommand, "Show the camp of the loaded save; in town, open the Campfire panel.") &&
		context->RegisterConsoleCommand("dd-slot", SlotCommand, "Load save slot 1, 2 or 3 (main menu only).") &&
		context->RegisterConsoleCommand("dd-traveler", TravelerCommand, "Make a traveler turn up now (in town; test).") &&
		context->RegisterConsoleCommand("dd-where", WhereCommand, "Show the hero and fire positions the plugin reads; 'dd-where scan' looks again (test).") &&
		context->RegisterConsoleCommand("dd-set", SetCommand, "Game-state WRITES test: dd-set copies | stat <id> <value> | hold <id> <value> | state <row> end|stat ...") &&
		context->RegisterConsoleCommand("dd-state", StateCommand, "Game-state API test: hero, world, monsters, objects, items; 'dd-state stats', 'states', 'skills', 'items', 'players', 'counters', 'watch', 'tables'.") &&
		context->RegisterConsoleCommand("dd-units", UnitsCommand, "List the units the game has loaded: 'dd-units', 'dd-units objects 39', 'dd-units monsters', 'dd-units find' (test).");
}

void AscensionBegan(const std::string& hero) noexcept {
	try {
		const Lock lock(g_mutex);
		Legacy::WriteFight(g_live, g_slot, Legacy::Fight::Fighting, hero);
	} catch (...) {
	}
}

void AscensionWon(const std::string& hero) noexcept {
	try {
		const Lock lock(g_mutex);
		Legacy::WriteFight(g_live, g_slot, Legacy::Fight::Won, hero);
	} catch (...) {
	}
}

void LeaveForCampfire(const D2RL::PluginContext* context) noexcept {
	if (g_threads != nullptr) {
		g_threads->runOnUiThread(context, LeaveGame, nullptr);
	}
}

auto LootData() noexcept -> const std::string& {
	return g_lootData;
}

auto RouteProgressFile() noexcept -> std::filesystem::path {
	try {
		const Lock lock(g_mutex);
		if (g_live.empty()) {
			return {};
		}
		// g_currentHero can be empty (a hero not started from the Campfire, or one whose save the
		// camp hadn't seen yet); then the save folder tells who plays, as for the town scan.
		// Progress under "_unknown" was shared by every such hero and got lost for its owner.
		const std::string hero = CurrentHero();
		return Route::ProgressFile(g_live, g_slot, hero.empty() ? "_unknown" : hero);
	} catch (...) {
		return {};
	}
}

auto OnUiMessage(const D2RL::PluginContext* context, const char* target, const char* command, const char* text) noexcept -> bool {
	try {
		const Lock lock(g_mutex);
		if (g_live.empty()) {
			return false;
		}
		const bool panelManager = std::strcmp(target, "PanelManager") == 0;
		if (panelManager && std::strcmp(command, "OpenPanel") == 0 && StartsWith(text, ActionPrefix)) {
			return OnAction(context, text + sizeof(ActionPrefix) - 1);
		}
		if (panelManager && std::strcmp(command, "ClosePanel") == 0 && StartsWith(text, "darkestdiablo/SaveSlots")) {
			g_slotsOpen = false;
			return false;
		}
		if (std::strcmp(target, "FrontEndNavigation") == 0) {
			if (std::strcmp(command, "ToFrontEnd") == 0) {  // the main menu is about to load: after launch, a game or the hero screen
				const bool afterGame = g_inGame;
				g_inGame = false;
				g_levelId = -1;
				g_selectedClass = -1;
				if (g_pendingPlay.empty()) {
					if (afterGame) {
						Camp::Unbench(g_live, g_slot);
						g_wantFire = g_fireOn && g_fireReady;
					}
					Reconcile(context, afterGame);
				}
				if (const int synced = Camp::SyncBindings(g_live); synced > 0) {
					Log(context, "Camp: key bindings shared with %d hero file(s).", synced);
				}
				Log(context, "Camp: at the menu. %s", Describe().c_str());
			} else if (std::strcmp(command, "ToCharacterCreate") == 0 && !(g_fireOn && g_fireReady) && Camp::Creatable(g_camp, g_rng).empty()) {
				Log(context, "Camp: nobody is waiting to join; the hero screen stays closed.");
				return true;
			}
			if (std::strcmp(command, "ToCharacterSelect") == 0 && g_emptyFire) {
				return true;  // Back over the empty fire: the save slots stay
			}
			if (std::strcmp(command, "ToCharacterSelect") == 0 && g_atFire && g_pendingPlay.empty() && g_slotPicker && g_threads != nullptr) {
				// Change save (or Escape) at the Campfire: the heroes leave the fire and the save
				// slots are laid over the empty fire (Sam), as at launch.
				g_threads->runOnUiThread(context, LeaveFireForSlots, nullptr);
				return true;
			}
			if (std::strcmp(command, "ToExit") == 0 && (g_atFire || g_emptyFire) && g_threads != nullptr) {
				ExitFromFire(context);
				return true;
			}
			if (std::strcmp(command, "ToCharacterCreate") != 0) {
				g_atFire = false;
				g_emptyFire = false;
				PoolClose(context, g_fire);
			}
			if (std::strcmp(command, "ToGame") == 0) {
				Close(context, g_curtainPanel);
			}
			return false;
		}
		if (panelManager && std::strcmp(text, "MainMenuPanel") == 0 && std::strcmp(command, "OpenExclusivePanel") == 0) {
			g_awaitingMenu = false;  // the menu is loading (again): it reads the live folder now
			if (g_threads != nullptr && (g_wantFire || g_exiting || !g_pendingPlay.empty())) {
				g_threads->runOnUiThread(context, OpenCurtain, nullptr);  // the menu is only passed through
			} else if (!g_slotChosen && g_slotPicker && g_threads != nullptr) {
				// before the menu can be seen: black, then the slots over the empty fire (or alone)
				g_threads->runOnUiThread(context, g_fireOn && g_fireReady ? OpenCurtain : ShowSlots, nullptr);
			}
			return false;
		}
		if (panelManager && std::strcmp(text, "CharacterSelectPanel") == 0 && std::strcmp(command, "OpenPanel") == 0) {
			if (g_exiting) {
				Later(context, 300, PressExit);
			} else if (!g_pendingPlay.empty()) {  // a hero was clicked at the Campfire: the list holds it alone
				Later(context, 400, PressPlay);
			} else if (!g_slotChosen && g_slotPicker && g_threads != nullptr) {
				g_threads->runOnUiThread(context, g_fireOn && g_fireReady ? GoToEmptyFire : ShowSlots, nullptr);
			} else if (g_wantFire && g_threads != nullptr) {
				g_slotChosen = true;
				g_wantFire = false;
				g_threads->runOnUiThread(context, GoToFire, nullptr);
			} else {
				g_slotChosen = true;
			}
			return false;
		}
		if (panelManager && std::strcmp(text, "CharacterCreatePanel") == 0 && std::strcmp(command, "OpenExclusivePanel") == 0) {
			g_selectedClass = -1;
			if (g_threads != nullptr) {
				g_threads->runOnUiThread(context, g_emptyFire ? ShowEmptyFire : OpenFire, nullptr);
			}
			return false;
		}
		// In the Rogue Encampment the fire opens the stash window (040_campfire.py), which is the
		// only sign of it the plugin gets. The town's own stash chest opens it too, so the hero's
		// distance to the fire decides: at the fire, using it saves and leaves the game for the
		// Campfire scene, like Save and Exit; anywhere else the stash opens as usual.
		if (panelManager && std::strcmp(text, "BankExpansionLayout") == 0 && std::strcmp(command, "OpenPanel") == 0 && g_inGame &&
		    g_levelId == RogueEncampment && g_threads != nullptr && (g_travelerArrives || AtFire(context))) {
			// The fire is a stash chest to the game; the stash window itself stays shut.
			if (g_travelerArrives) {
				return true;
			}
			if (TravelerWaits()) {
				g_travelerArrives = true;
				g_threads->runOnUiThread(context, TravelerPortal, nullptr);
				Log(context, "Camp: the fire was used with a traveler waiting: portal, traveler, then the Campfire.");
				return true;
			}
			RecordGear(context, nullptr);
			g_threads->runOnUiThread(context, LeaveGame, nullptr);
			Log(context, "Camp: the fire was used: saving and going to the Campfire.");
			return true;
		}
		if (std::strcmp(target, "PausePanelMessage") == 0 && std::strcmp(command, "ExitGame") == 0) {
			RecordGear(context, nullptr);
			return false;
		}
		if (panelManager && std::strcmp(text, "YouDiedModal") == 0 && std::strcmp(command, "OpenPanel") == 0 && g_inGame) {
			g_died = true;
			Log(context, "Camp: the hero died.");
			return false;
		}
		if (std::strcmp(target, "CharacterCreate") == 0) {
			const int cls = Camp::ClassFromName(text);
			// The game keeps sending Hover while a figure animates, so the plugin always knows
			// which hero a click is meant for (see WatchFireClicks).
			if (std::strcmp(command, "Hover") == 0) {
				g_hoveredHero = cls;
			} else if (std::strcmp(command, "Unhover") == 0 && cls == g_hoveredHero) {
				g_hoveredHero = -1;
			}
			if (cls >= 0 && g_camp.HeroOf(cls) != nullptr) {  // a hero of ours: a click picks (or buries) it
				if (std::strcmp(command, "Select") == 0) {
					if (g_deleteMode) {
						BuryHero(context, cls);
						return true;
					}
					g_chosenHero = cls;
					g_createMode = false;
					g_selectedClass = -1;
					ShowDeleteMode(context);
					Later(context, 100, ShowFireState);
				} else if (std::strcmp(command, "Unselect") == 0 && cls == g_chosenHero) {
					g_chosenHero = -1;
					ShowDeleteMode(context);
				}
				return false;  // let the figure react
			}
			const auto allowed = Allowed();
			if (std::strcmp(command, "Select") == 0 || std::strcmp(command, "Hover") == 0) {
				if (!Camp::State::Has(allowed, cls)) {
					return true;
				}
				if (std::strcmp(command, "Select") == 0) {
					g_selectedClass = cls;
					g_pickedClass = cls;
					g_chosenHero = -1;
					ShowDeleteMode(context);
					Later(context, 100, ShowFireState);
				}
			} else if (std::strcmp(command, "Unselect") == 0 && cls == g_selectedClass) {
				g_selectedClass = -1;
			} else if (std::strcmp(command, "Create") == 0) {
				if (g_travelerMode) {  // the traveler's choice comes first (Accept)
					return true;
				}
				if (g_atFire && !g_createMode) {  // first press: bring out the name box
					if (!allowed.empty()) {
						g_createMode = true;
						g_chosenHero = -1;
						ShowDeleteMode(context);
					}
					return true;
				}
				if (!Camp::State::Has(allowed, g_selectedClass)) {
					Log(context, "Camp: Create refused; that class can't join right now.");
					return true;
				}
				g_pendingClass = g_selectedClass;
				Log(context, "Camp: making a %s.", Camp::ClassNames[g_pendingClass]);
			}
		}
	} catch (...) {
	}
	return false;
}

void NoteDifficulty(const D2RL::PluginContext* context, uint32_t difficulty) noexcept {
	try {
		const Lock lock(g_mutex);
		const int d = static_cast<int>(difficulty);
		if (g_live.empty() || !g_inGame || d <= g_camp.difficulty || d > 2) {
			return;
		}
		g_camp.difficulty = d;
		Log(context, "Camp: save %d now plays on %s.", g_slot, Camp::DifficultyNames[d]);
		if (!Camp::SaveState(g_live, g_slot, g_camp)) {
			context->LogWarn("Camp: could not write camp.txt.");
		}
	} catch (...) {
	}
}

void OnGameplayEvent(const D2RL::PluginContext* context, const D2RL::Lifecycle::GameplayEvent* event) noexcept {
	if (event->difficulty > 0 && event->kind != D2RL::Lifecycle::GameplayEventKind::GameJoined) {
		NoteDifficulty(context, event->difficulty);  // fields an event doesn't use are 0, so only a higher one counts
	}
	if (event->playerId != 0) {
		Game::SetLocalPlayerId(event->playerId);  // makes Game::Hero() exact (gameapi.h)
	}
	try {
		const Lock lock(g_mutex);
		if (g_live.empty()) {
			return;
		}
		using Kind = D2RL::Lifecycle::GameplayEventKind;
		switch (event->kind) {
		case Kind::GameJoined:
			Game::ResetCounters();  // a game with a hero starts the run's counters (gameapi.h)
			g_inGame = true;
			g_ascendedNote.clear();
			g_slotChosen = true;
			g_stashSlot = g_slot;  // from now on the game keeps this save's stash in memory
			g_died = false;
			g_reachedLevel = 0;
			g_joinTime = fs::file_time_type::clock::now();
			g_currentHero = g_pendingPlay;  // a hero clicked at the Campfire; a new hero is named below
			g_pendingPlay.clear();
			g_wantFire = false;
			g_gearNow.clear();
			g_atFire = false;
			PoolClose(context, g_fire);
			Close(context, g_curtainPanel);
			break;
		case Kind::LocalPlayerReady: {
			g_inGame = true;
			Camp::Unbench(g_live, g_slot);
			g_travelerLater = false;  // a traveler sent away with "Not now" waits by the fire again
			g_campHeroesPlaced = false;
			const int made = g_pendingClass;
			Reconcile(context, false);  // a hero that was just made joins the camp here
			if (g_currentHero.empty() && made >= 0) {
				if (const Camp::Hero* hero = g_camp.HeroOf(made)) {
					g_currentHero = hero->name;
				}
			}
			Log(context, "Camp: playing %s.", g_currentHero.empty() ? "an unknown hero (route progress is shared)" : g_currentHero.c_str());
			Ascension::OnReady(context, g_currentHero);
			Later(context, 1000, ScanTown);  // when the game starts in the Rogue Encampment
			Later(context, 2000, RecordGear);
			break;
		}
		case Kind::PlayerLevelChanged:
			g_reachedLevel = event->currentValue;
			break;
		case Kind::PlayerResurrected:  // only softcore heroes come back: death is final, so out of the game
			g_died = true;
			Log(context, "Camp: the hero died and came back; sending it out of the game.");
			Later(context, 500, LeaveGame);
			break;
		case Kind::LevelChanged: {
			const int from = event->previousValue;
			g_levelId = event->currentValue;
			Ascension::OnLevelChanged(context, g_levelId);
			Later(context, 1000, RecordGear);
			if (g_levelId == RogueEncampment) {
				Later(context, 1000, ScanTown);  // the town's units are loaded by then
			}
			if (IsTown(g_levelId) && from > 0 && !IsTown(from) && !g_died && !Ascension::Active() && !g_camp.LockedClasses().empty()) {
				const int roll = std::uniform_int_distribution<int>(1, 100)(g_rng);
				Log(context, "Camp: back in town; traveler roll %d (needs %d or less).", roll, g_travelerChance);
				if (roll <= g_travelerChance) {
					Later(context, 1500, ShowTraveler);  // let the town load first
				}
			}
			break;
		}
		case Kind::GameLeft:
			Ascension::OnGameLeft();
			g_travelerArrives = false;
			g_levelId = -1;  // g_inGame stays set until the menu loads, so the camp knows a game just ended
			break;
		default:
			break;
		}
	} catch (...) {
	}
}

}
