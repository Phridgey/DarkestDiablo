// Darkest Diablo — the Campfire (see campfire.h, camp.h and docs/campfire.md).
//
// How each part reaches the game:
// - Save slots: a plugin panel over the main menu at launch. Picking another slot moves the save
//   files (camp.h, SwitchSlot) and asks the menu to load again.
// - The roster is the game's own character list. The camp is kept in line with the save files
//   every time the menu opens (camp.h, Reconcile).
// - Recruiting uses the game's hero screen. Classes that can't be made are hidden and their
//   clicks are swallowed.
// - Death: the game's "You died" window or a resurrection marks the hero as fallen. A softcore
//   hero is sent out of the game at once. Back at the menu the files go to the graveyard.
// - The traveler: a chance each time a hero comes back to town; a panel offers up to 3 classes.
// - In town, the stash (and so the bonfire) has a Campfire button that opens the roster panel.
//
// Console (Ctrl + `): dd-camp, dd-slot <1-3>, dd-traveler.

#include "campfire.h"
#include "camp.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <exception>
#include <mutex>
#include <thread>

namespace Campfire {

namespace {

namespace fs = std::filesystem;
using Lock = std::lock_guard<std::recursive_mutex>;

constexpr char PluginId[]      = "darkestdiablo";
constexpr char SlotsPanel[]    = "SaveSlots";
constexpr char TravelerPanel[] = "Traveler";
constexpr char CampPanel[]     = "Campfire";
constexpr char ActionPrefix[]  = "darkestdiablo/";  // buttons send PanelManager:OpenPanel:darkestdiablo/<action>

const D2RL::ThreadService* g_threads = nullptr;
const D2RL::PanelService*  g_panels  = nullptr;
const D2RL::WidgetService* g_widgets = nullptr;
D2RL::Panels::RegistrationHandle g_slotsPanel    = D2RL::Panels::InvalidHandle;
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
bool g_inGame        = false;
int  g_levelId       = -1;
int  g_selectedClass = -1;      // on the hero screen
int  g_pendingClass  = -1;      // Create was pressed for this class; its save has not been seen yet
bool g_died          = false;   // the hero in the current game died
int  g_reachedLevel  = 0;       // character level reported during the current game
fs::file_time_type g_joinTime {};

void Log(const D2RL::PluginContext* context, const char* format, ...) noexcept {
	char message[512] {};
	va_list args;
	va_start(args, format);
	std::vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	context->LogInfo(message);
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
	hints.newClass = g_pendingClass;
	if (afterGame) {
		const std::string played = PlayedHero();
		if (g_died) {
			hints.diedName = played;
		}
		hints.leveledName = played;
		hints.leveledTo = g_reachedLevel;
	}
	const size_t heroesBefore = g_camp.heroes.size();
	for (const auto& line : Camp::Reconcile(g_live, g_slot, g_camp, hints)) {
		Log(context, "Camp: %s", line.c_str());
	}
	if (g_camp.heroes.size() > heroesBefore || !g_inGame) {
		g_pendingClass = -1;
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

// ---- Save slots ---------------------------------------------------------------------------

void __cdecl ShowSlots(const D2RL::PluginContext* context, void*) noexcept {
	const Lock lock(g_mutex);
	if (g_slotChosen || g_inGame) {
		return;
	}
	if (!Open(context, g_slotsPanel, "save slot")) {
		g_slotChosen = true;  // carry on with the slot that is live
		return;
	}
	const std::string panel = FullName(SlotsPanel);
	for (int slot = 1; slot <= Camp::SlotCount; ++slot) {
		SetVisible(context, panel.c_str(), ("Slot" + std::to_string(slot) + "Last").c_str(), slot == g_slot);
	}
	SetVisible(context, panel.c_str(), "RestartNotice", false);
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
	if (Open(context, g_slotsPanel, "save slot")) {
		const std::string panel = FullName(SlotsPanel);
		SetVisible(context, panel.c_str(), "RestartNotice", true);
		for (int slot = 1; slot <= Camp::SlotCount; ++slot) {
			SetVisible(context, panel.c_str(), ("Slot" + std::to_string(slot)).c_str(), false);
			SetVisible(context, panel.c_str(), ("Slot" + std::to_string(slot) + "Summary").c_str(), false);
			SetVisible(context, panel.c_str(), ("Slot" + std::to_string(slot) + "Last").c_str(), false);
		}
	}
}

void __cdecl ReloadMenu(const D2RL::PluginContext* context, void*) noexcept {
	Dispatch(context, "FrontEndNavigation", "ToFrontEnd");
	Later(context, 3000, CheckMenuReloaded);
}

// Makes `slot` live. Only from the main menu. Returns a message for the console.
auto PickSlot(const D2RL::PluginContext* context, int slot) -> std::string {
	if (g_inGame) {
		return "Leave the game first: slots can only change at the main menu.";
	}
	g_slotChosen = true;
	Close(context, g_slotsPanel);
	if (slot == g_slot) {
		return "Save " + std::to_string(slot) + " is already loaded.";
	}
	std::string error;
	if (!Camp::SwitchSlot(g_live, slot, error)) {
		Log(context, "Camp: could not switch to save %d: %s", slot, error.c_str());
		return "Could not switch save: " + error;
	}
	g_slot = slot;
	g_camp = Camp::LoadState(g_live, g_slot);
	g_pendingClass = -1;
	Reconcile(context, false);
	Log(context, "Camp: switched to save %d. %s", slot, Describe().c_str());
	g_awaitingMenu = true;
	if (g_threads != nullptr) {
		g_threads->runOnUiThread(context, ReloadMenu, nullptr);
	}
	return "Switched to save " + std::to_string(slot) + ".";
}

// ---- Hero screen --------------------------------------------------------------------------

// Hides the classes that can't be made right now. Runs a moment after the hero screen opens.
void __cdecl FilterHeroScreen(const D2RL::PluginContext* context, void*) noexcept {
	const Lock lock(g_mutex);
	const auto allowed = Camp::Creatable(g_camp, g_rng);
	int hidden = 0;
	for (int c = 0; c < Camp::ClassCount; ++c) {
		const bool show = Camp::State::Has(allowed, c);
		if (SetVisible(context, "CharacterCreatePanel", Camp::ClassNames[c], show) && !show) {
			++hidden;
		}
	}
	Log(context, "Camp: hero screen shows %d class(es), hid %d.", static_cast<int>(allowed.size()), hidden);
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
	if (g_camp.offer.empty() || !Open(context, g_travelerPanel, "traveler")) {
		return;
	}
	const std::string panel = FullName(TravelerPanel);
	for (int place = 0; place < Camp::OfferSize; ++place) {
		for (int c = 0; c < Camp::ClassCount; ++c) {
			const bool show = place < static_cast<int>(g_camp.offer.size()) && g_camp.offer[place] == c;
			SetVisible(context, panel.c_str(), ("Offer" + std::to_string(place) + Camp::ClassNames[c]).c_str(), show);
		}
	}
	Log(context, "Camp: a traveler joins the camp. %s", Describe().c_str());
}

// ---- Roster panel (in town) ---------------------------------------------------------------

void __cdecl ShowCamp(const D2RL::PluginContext* context, void*) noexcept {
	const Lock lock(g_mutex);
	if (!Open(context, g_campPanel, "campfire")) {
		return;
	}
	const std::string panel = FullName(CampPanel);
	for (int c = 0; c < Camp::ClassCount; ++c) {
		const bool hero = g_camp.HeroOf(c) != nullptr;
		const bool recruit = Camp::State::Has(g_camp.recruits, c);
		const std::string name = Camp::ClassNames[c];
		SetVisible(context, panel.c_str(), ("Hero" + name).c_str(), hero);
		SetVisible(context, panel.c_str(), ("Recruit" + name).c_str(), recruit);
		SetVisible(context, panel.c_str(), ("Locked" + name).c_str(), !hero && !recruit);
	}
}

void __cdecl LeaveGame(const D2RL::PluginContext* context, void*) noexcept {
	Dispatch(context, "PausePanelMessage", "ExitGame");
}

// ---- Console ------------------------------------------------------------------------------

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

// The slot screen's summary lines, as a string file made at launch (the only moment the screen
// is shown). Layout text can't be changed later through the SDK.
void RegisterSlotStrings(const D2RL::PluginContext* context, const D2RL::ResourceService* resources) {
	static constexpr const char* Locales[] = { "enUS", "deDE", "esES", "frFR", "itIT", "koKR", "plPL", "ruRU", "zhCN", "zhTW", "esMX", "jaJP", "ptBR" };
	std::string json = "[";
	for (int slot = 1; slot <= Camp::SlotCount; ++slot) {
		const Camp::State camp = slot == g_slot ? g_camp : Camp::LoadState(g_live, slot);
		const std::string text = Camp::Summary(camp);  // class names, numbers and plain words: nothing to escape
		json += std::string(slot > 1 ? "," : "") + "{\"Key\":\"Slot" + std::to_string(slot) + "Summary\"";
		for (const char* locale : Locales) {
			json += std::string(",\"") + locale + "\":\"" + text + "\"";
		}
		json += "}";
	}
	json += "]";
	const D2RL::Resources::ResourceRegistration resource {
		.structSize = D2RL::Resources::ResourceRegistrationSize,
		.path       = "data/local/lng/strings/d2rloader/darkestdiablo/camp.json",
		.bytes      = json.data(),
		.byteCount  = static_cast<uint32_t>(json.size()),
	};
	D2RL::Resources::RegistrationHandle handle = D2RL::Resources::InvalidHandle;
	if (resources->registerResource(context, &resource, &handle) != D2RL::Resources::Result::Success) {
		context->LogWarn("Camp: could not add the save slot summaries.");
	}
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
auto OnAction(const D2RL::PluginContext* context, const char* action) -> bool {
	if (StartsWith(action, "slot-")) {
		PickSlot(context, std::atoi(action + 5));
	} else if (StartsWith(action, "recruit-")) {
		const int cls = Camp::ClassFromName(action + 8);
		Close(context, g_travelerPanel);
		if (Camp::Recruit(g_camp, cls)) {
			Camp::SaveState(g_live, g_slot, g_camp);
			Log(context, "Camp: the %s joins the camp as a recruit. %s", Camp::ClassNames[cls], Describe().c_str());
		}
	} else if (std::strcmp(action, "traveler-later") == 0) {
		Close(context, g_travelerPanel);
		g_camp.offer.clear();
		Camp::SaveState(g_live, g_slot, g_camp);
		Log(context, "Camp: the traveler moves on.");
	} else if (std::strcmp(action, "open-campfire") == 0) {
		ShowCamp(context, nullptr);
	} else if (std::strcmp(action, "to-campfire") == 0) {
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
		const Lock lock(g_mutex);
		g_live = FindSaveFolder();
		if (g_live.empty()) {
			context->LogError("Camp: could not find the Saved Games folder. The Campfire is off.");
			return true;
		}
		std::error_code ec;
		fs::create_directories(g_live / "camp", ec);
		ReadOptions();
		g_slot = Camp::ActiveSlot(g_live);
		g_camp = Camp::LoadState(g_live, g_slot);
		Reconcile(context, false);
		Log(context, "Camp: %s", Describe().c_str());

		const D2RL::ResourceService* resources = nullptr;
		if (context->QueryService(&resources) != D2RL::ServiceQueryResult::Success || !D2RL::HasResourceServiceField(resources, D2RL::ResourceServiceRequiredSize)) {
			resources = nullptr;
		}
		if (context->QueryService(&g_threads) != D2RL::ServiceQueryResult::Success || !D2RL::HasThreadServiceField(g_threads, D2RL::ThreadServiceRequiredSize)) {
			g_threads = nullptr;
		}
		if (context->QueryService(&g_widgets) != D2RL::ServiceQueryResult::Success || !D2RL::HasWidgetServiceField(g_widgets, D2RL::WidgetServiceRequiredSize)) {
			g_widgets = nullptr;
			context->LogWarn("Camp: widget service missing; classes can't be hidden on the hero screen.");
		}
		if (context->QueryService(&g_panels) == D2RL::ServiceQueryResult::Success && D2RL::HasPanelServiceField(g_panels, D2RL::PanelServiceRequiredSize)) {
			g_slotsPanel    = RegisterPanel(context, resources, SlotsPanel);
			g_travelerPanel = RegisterPanel(context, resources, TravelerPanel);
			g_campPanel     = RegisterPanel(context, resources, CampPanel);
		} else {
			g_panels = nullptr;
			context->LogWarn("Camp: panel service missing; no slot screen, traveler or roster panel.");
		}
		if (resources != nullptr) {
			RegisterSlotStrings(context, resources);
		}
	} catch (const std::exception& error) {
		Log(context, "Camp: setup failed (%s). The Campfire is off.", error.what());
		g_live.clear();
		return true;
	}
	return context->RegisterConsoleCommand("dd-camp", CampCommand, "Show the camp of the loaded save; in town, open the Campfire panel.") &&
		context->RegisterConsoleCommand("dd-slot", SlotCommand, "Load save slot 1, 2 or 3 (main menu only).") &&
		context->RegisterConsoleCommand("dd-traveler", TravelerCommand, "Make a traveler turn up now (in town; test).");
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
		if (std::strcmp(target, "FrontEndNavigation") == 0) {
			if (std::strcmp(command, "ToFrontEnd") == 0) {  // the main menu is about to load: after launch, a game or the hero screen
				const bool afterGame = g_inGame;
				g_inGame = false;
				g_levelId = -1;
				g_selectedClass = -1;
				Reconcile(context, afterGame);
				Log(context, "Camp: at the fire. %s", Describe().c_str());
			} else if (std::strcmp(command, "ToCharacterCreate") == 0 && Camp::Creatable(g_camp, g_rng).empty()) {
				Log(context, "Camp: nobody is waiting to join; the hero screen stays closed.");
				return true;
			}
			return false;
		}
		if (panelManager && std::strcmp(text, "MainMenuPanel") == 0 && std::strcmp(command, "OpenExclusivePanel") == 0) {
			g_awaitingMenu = false;  // the menu is loading (again): it reads the live folder now
			return false;
		}
		if (panelManager && std::strcmp(text, "CharacterSelectPanel") == 0 && std::strcmp(command, "OpenPanel") == 0) {
			if (!g_slotChosen && g_slotPicker && g_threads != nullptr) {
				g_threads->runOnUiThread(context, ShowSlots, nullptr);
			} else {
				g_slotChosen = true;
			}
			return false;
		}
		if (panelManager && std::strcmp(text, "CharacterCreatePanel") == 0 && std::strcmp(command, "OpenExclusivePanel") == 0) {
			g_selectedClass = -1;
			if (g_threads != nullptr) {
				g_threads->runOnUiThread(context, FilterHeroScreen, nullptr);
				Later(context, 500, FilterHeroScreen);  // again once the screen has settled
			}
			return false;
		}
		if (panelManager && std::strcmp(text, "YouDiedModal") == 0 && std::strcmp(command, "OpenPanel") == 0 && g_inGame) {
			g_died = true;
			Log(context, "Camp: the hero died.");
			return false;
		}
		if (std::strcmp(target, "CharacterCreate") == 0) {
			const int cls = Camp::ClassFromName(text);
			const auto allowed = Camp::Creatable(g_camp, g_rng);
			if (std::strcmp(command, "Select") == 0 || std::strcmp(command, "Hover") == 0) {
				if (!Camp::State::Has(allowed, cls)) {
					return true;
				}
				if (std::strcmp(command, "Select") == 0) {
					g_selectedClass = cls;
				}
			} else if (std::strcmp(command, "Unselect") == 0 && cls == g_selectedClass) {
				g_selectedClass = -1;
			} else if (std::strcmp(command, "Create") == 0) {
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

void OnGameplayEvent(const D2RL::PluginContext* context, const D2RL::Lifecycle::GameplayEvent* event) noexcept {
	try {
		const Lock lock(g_mutex);
		if (g_live.empty()) {
			return;
		}
		using Kind = D2RL::Lifecycle::GameplayEventKind;
		switch (event->kind) {
		case Kind::GameJoined:
			g_inGame = true;
			g_slotChosen = true;
			g_died = false;
			g_reachedLevel = 0;
			g_joinTime = fs::file_time_type::clock::now();
			break;
		case Kind::LocalPlayerReady:
			g_inGame = true;
			Reconcile(context, false);  // a hero that was just made joins the camp here
			break;
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
			if (IsTown(g_levelId) && from > 0 && !IsTown(from) && !g_died && !g_camp.LockedClasses().empty()) {
				const int roll = std::uniform_int_distribution<int>(1, 100)(g_rng);
				Log(context, "Camp: back in town; traveler roll %d (needs %d or less).", roll, g_travelerChance);
				if (roll <= g_travelerChance) {
					Later(context, 1500, ShowTraveler);  // let the town load first
				}
			}
			break;
		}
		case Kind::GameLeft:
			g_levelId = -1;  // g_inGame stays set until the menu loads, so the camp knows a game just ended
			break;
		default:
			break;
		}
	} catch (...) {
	}
}

}
