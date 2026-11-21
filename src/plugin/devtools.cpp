// Darkest Diablo — the GM window (see devtools.h and docs/gm-tools.md).
//
// How each part works:
// - The switches use the loader's own cheats: "cheat god" turns no-damage, endless stamina and
//   endless mana on or off together, and "cheat invincible" / "cheat stamina" then set the first
//   two on their own. There is no cheat for mana alone, which is why every change sends all three.
// - Town: the console's "goto <town>". The plugin can't make a real portal, so the level the
//   player left is remembered and the Return button goes back to it (to that level's entrance).
// - Items: the plugin's item service, straight into the inventory (the ground if it is full).
// - Events: whatever was registered with RegisterEvent, one button each.
//
// The plugin can show and hide widgets but can't change their text, so every switch is two
// ready-made buttons (off and on) of which the right one is shown.

#include "devtools.h"
#include "gameapi.h"
#include <D2RLPlugin/core_exports.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <thread>

namespace DevTools {

namespace {

constexpr char PanelLocalId[] = "GM";
constexpr char PanelName[]    = "darkestdiablo/GM";
constexpr char LayoutPath[]   = "data/global/ui/layouts/darkestdiablo/GMhd.json";
constexpr char ActionPrefix[] = "darkestdiablo/gm-";  // buttons send PanelManager:OpenPanel:darkestdiablo/gm-<action>
constexpr int  MaxEvents      = 24;

const D2RL::ThreadService*    g_threads   = nullptr;
const D2RL::PanelService*     g_panels    = nullptr;
const D2RL::WidgetService*    g_widgets   = nullptr;
const D2RL::InventoryService* g_inventory = nullptr;
const D2RL::ItemService*      g_items     = nullptr;
D2RL::Panels::RegistrationHandle g_panel = D2RL::Panels::InvalidHandle;

Event g_events[MaxEvents] {};
int   g_eventCount = 0;

// All of this is read and written on the UI thread only.
bool g_inGame      = false;
int  g_levelId     = -1;
int  g_returnLevel = -1;     // where "Return" goes; -1: nowhere yet
bool g_life        = false;
bool g_mana        = false;
bool g_stamina     = false;
bool g_itemError   = false;  // the last item code typed was no good: show the hint in red
std::atomic<uintptr_t> g_session { 1 };  // changes when a game ends, so old timers do nothing

void Log(const D2RL::PluginContext* context, const char* format, ...) noexcept {
	char message[512] {};
	va_list args;
	va_start(args, format);
	std::vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	context->LogInfo(message);
}

// Runs a console command through D2RCore's ExecuteConsoleCommand export (UI thread only).
auto RunConsole(const D2RL::PluginContext* context, const char* command) noexcept -> bool {
	namespace CE = D2RL::CoreExports;
	static CE::ExecuteConsoleCommandFn execute = nullptr;
	if (execute == nullptr) {
		const HMODULE core = GetModuleHandleA(CE::CoreDllName);
		execute = core == nullptr ? nullptr :
			reinterpret_cast<CE::ExecuteConsoleCommandFn>(GetProcAddress(core, CE::ExecuteConsoleCommandInfo.name));
	}
	if (execute == nullptr) {
		context->LogWarn("GM: ExecuteConsoleCommand not found in D2RCore.");
		return false;
	}
	const bool ok = execute(command);
	Log(context, "GM: console %s -> %s", command, ok ? "sent" : "refused");
	return ok;
}

// The town of the act a level belongs to (levels.txt Ids).
auto TownOf(int levelId) noexcept -> int {
	return levelId >= 109 ? 109 : levelId >= 103 ? 103 : levelId >= 75 ? 75 : levelId >= 40 ? 40 : 1;
}

auto IsTown(int levelId) noexcept -> bool {
	return levelId == 1 || levelId == 40 || levelId == 75 || levelId == 103 || levelId == 109;
}

// ---- The window's layout ------------------------------------------------------------------
// Built once at load, from the list of events. Sizes are in the game's layout units (the
// screen is 2160 high). A ModalButton is 513 x 92.

constexpr int WindowWidth = 1660;
constexpr int Margin      = 30;
constexpr int ColumnStep  = 543;
constexpr int RowStep     = 110;
constexpr int EventsTop   = 855;

auto Escape(const char* text) -> std::string {
	std::string out;
	for (const char* c = text; *c != '\0'; ++c) {
		if (*c == '"' || *c == '\\') {
			out += '\\';
		}
		if (static_cast<unsigned char>(*c) >= 0x20) {
			out += *c;
		}
	}
	return out;
}

auto Rect(int x, int y, int width = 0, int height = 0) -> std::string {
	std::string out = "{\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y);
	if (width > 0) {
		out += ",\"width\":" + std::to_string(width) + ",\"height\":" + std::to_string(height);
	}
	return out + "}";
}

auto Text(const char* name, int x, int y, int width, int height, const char* text, const char* color, const char* size) -> std::string {
	return std::string("{\"type\":\"TextBoxWidget\",\"name\":\"") + name + "\",\"fields\":{\"rect\":" + Rect(x, y, width, height) +
		",\"text\":\"" + Escape(text) + "\",\"style\":{\"fontColor\":\"" + color + "\",\"pointSize\":\"" + size +
		"\",\"alignment\":{\"h\":\"left\",\"v\":\"center\"},\"dropShadow\":\"$DefaultDropShadow\"}}}";
}

// A button that tells the plugin "gm-<action>". With no action it is greyed out.
auto Button(const std::string& name, int x, int y, const char* text, const std::string& action, const char* color = "$FontColorWhite", const char* scale = nullptr) -> std::string {
	std::string out = "{\"type\":\"ButtonWidget\",\"name\":\"" + name + "\",\"fields\":{\"rect\":{\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y);
	if (scale != nullptr) {
		out += std::string(",\"scale\":") + scale;
	}
	out += std::string("},\"filename\":\"Panel\\\\Modals\\\\ModalButton\",\"focusIndicatorFilename\":\"Controller/HoverImages/ModalButton_Hover\","
		"\"pressedFrame\":1,\"disabledFrame\":2,\"hoveredFrame\":3,\"textString\":\"") + Escape(text) + "\",\"pointSize\":\"$MediumFontSize\",\"textColor\":\"" + color + "\"";
	if (action.empty()) {
		out += ",\"disabled\":true";
	} else {
		out += std::string(",\"onClickMessage\":\"PanelManager:OpenPanel:") + ActionPrefix + action + "\"";
	}
	return out + "}}";
}

auto Column(int column) noexcept -> int {
	return Margin + column * ColumnStep;
}

auto LayoutJson() -> std::string {
	static constexpr char Gold[]  = "$FontColorGoldYellow";
	static constexpr char White[] = "$FontColorWhite";
	static constexpr char Grey[]  = "$FontColorLightGray";
	const int eventRows = g_eventCount > 0 ? (g_eventCount + 2) / 3 : 1;
	const int height = EventsTop + eventRows * RowStep + 20;
	std::string kids;
	const auto add = [&kids](const std::string& widget) {
		kids += "," + widget;
	};

	// A dark backing that also swallows clicks, so a click on the window doesn't walk the hero.
	kids = "{\"type\":\"RectangleWidget\",\"name\":\"Back\",\"fields\":{\"rect\":" + Rect(0, 0, WindowWidth, height) + ",\"color\":[0.0,0.0,0.0,0.6]},"
		"\"children\":[{\"type\":\"ClickCatcherWidget\",\"name\":\"catcher\",\"fields\":{\"fitToParent\":true}}]}";
	add(Text("Title", Margin, 20, 1200, 74, "GM tools", Gold, "$XExtraLargeFontSize"));
	add(std::string("{\"type\":\"ButtonWidget\",\"name\":\"CloseButton\",\"fields\":{\"rect\":") + Rect(WindowWidth - 96, 18) +
		",\"filename\":\"PANEL\\\\closebtn_4x\",\"hoveredFrame\":3,\"pressedFrame\":1,\"tooltipString\":\"@d2r:strClose\","
		"\"sound\":\"cursor_close_window_hd\",\"onClickMessage\":\"PanelManager:ClosePanel:" + PanelName + "\"}}");

	add(Text("CheatsLabel", Margin, 110, 1600, 50, "Cheats", Gold, "$LargeFontSize"));
	add(Button("LifeOff", Column(0), 165, "Unlimited life: off", "life"));
	add(Button("LifeOn", Column(0), 165, "Unlimited life: ON", "life", Gold));
	add(Button("ManaOff", Column(1), 165, "Unlimited mana: off", "mana"));
	add(Button("ManaOn", Column(1), 165, "Unlimited mana: ON", "mana", Gold));
	add(Button("StaminaOff", Column(2), 165, "Unlimited stamina: off", "stamina"));
	add(Button("StaminaOn", Column(2), 165, "Unlimited stamina: ON", "stamina", Gold));

	add(Text("TravelLabel", Margin, 285, 1600, 50, "Travel", Gold, "$LargeFontSize"));
	add(Button("Town", Column(0), 340, "Go to town", "town"));
	add(Button("ReturnOn", Column(1), 340, "Return to where I was", "return"));
	add(Button("ReturnOff", Column(1), 340, "Return to where I was", "", Grey));

	add(Text("ItemsLabel", Margin, 460, 1600, 50, "Items", Gold, "$LargeFontSize"));
	add("{\"type\":\"RectangleWidget\",\"name\":\"ItemCodeBack\",\"fields\":{\"rect\":" + Rect(Margin, 515, 520, 64) + ",\"color\":[0.12,0.10,0.08,1.0]}}");
	add("{\"type\":\"InputTextBoxWidget\",\"name\":\"ItemCode\",\"fields\":{\"rect\":" + Rect(Margin + 14, 522, 492, 50) +
		",\"maxStringLength\":4,\"canInsertNewlines\":false,\"backgroundColor\":\"$FontColorTransparent\",\"fontType\":\"12ptF\","
		"\"fontStyle\":{\"fontFace\":\"BlizzardGlobal\",\"fontColor\":\"$FontColorWhite\",\"pointSize\":\"$MediumFontSize\"},"
		"\"hint\":{\"text\":\"Item code\",\"style\":\"$StyleModalDialogDescription\"}}}");
	add(Text("ItemHint", Margin + 550, 515, 1050, 64, "Type an item's code (rin = ring, amu = amulet, lsd = long sword), then pick a quality.", Grey, "$MediumFontSize"));
	add(Text("ItemError", Margin + 550, 515, 1050, 64, "Type the item's 3 or 4 character code in the box first, like rin.", "$FontColorRed", "$MediumFontSize"));
	static constexpr const char* Qualities[] = { "Normal", "Magic", "Rare", "Set", "Unique" };
	for (int q = 0; q < 5; ++q) {
		std::string action = std::string("item-") + Qualities[q];
		for (char& c : action) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		add(Button(std::string("Item") + Qualities[q], Margin + q * 323, 600, Qualities[q], action, White, "0.6"));
	}
	add(Button("Gold", Column(0), 680, "Gold (up to 50,000)", "gold"));
	add(Button("Potions", Column(1), 680, "4 full rejuvenations", "potions"));
	add(Button("Identify", Column(2), 680, "Identify everything", "identify"));

	add(Text("EventsLabel", Margin, 800, 1600, 50, "Events", Gold, "$LargeFontSize"));
	if (g_eventCount == 0) {
		add(Text("NoEvents", Margin, EventsTop, 1600, 60, "No events yet.", Grey, "$MediumFontSize"));
	}
	for (int i = 0; i < g_eventCount; ++i) {
		add(Button("Event" + std::to_string(i), Column(i % 3), EventsTop + (i / 3) * RowStep, g_events[i].label, "event-" + std::to_string(i)));
	}

	return std::string("{\"type\":\"Panel\",\"name\":\"") + PanelName + "\",\"fields\":{\"anchor\":{\"x\":0.5,\"y\":0.5},\"priority\":9200},\"children\":["
		"{\"type\":\"NineTileImageWidget\",\"name\":\"Frame\",\"fields\":{\"filename\":\"PANEL/TooltipBorder/TooltipBorder\",\"backgroundColor\":[0.0,0.0,0.0,0.85],"
		"\"tileScale\":1.0,\"rect\":" + Rect(-WindowWidth / 2, -height / 2, WindowWidth, height) + "},\"children\":[" + kids + "]}]}";
}

// ---- Showing the window (UI thread) -------------------------------------------------------

void SetVisible(const D2RL::PluginContext* context, const char* widget, bool visible) noexcept {
	if (g_widgets == nullptr) {
		return;
	}
	D2RL::Widgets::WidgetHandle panel = D2RL::Widgets::InvalidHandle;
	D2RL::Widgets::WidgetHandle frame = D2RL::Widgets::InvalidHandle;
	D2RL::Widgets::WidgetHandle handle = D2RL::Widgets::InvalidHandle;
	if (g_widgets->findPanel(context, PanelName, &panel) == D2RL::Widgets::Result::Success &&
		g_widgets->findWidget(context, panel, "Frame", &frame) == D2RL::Widgets::Result::Success &&
		g_widgets->findWidget(context, frame, widget, &handle) == D2RL::Widgets::Result::Success) {
		g_widgets->setWidgetVisible(context, handle, visible);
	}
}

// Shows the right one of each pair of ready-made widgets. Does nothing while the window is closed.
void __cdecl ShowState(const D2RL::PluginContext* context, void*) noexcept {
	SetVisible(context, "LifeOn", g_life);
	SetVisible(context, "LifeOff", !g_life);
	SetVisible(context, "ManaOn", g_mana);
	SetVisible(context, "ManaOff", !g_mana);
	SetVisible(context, "StaminaOn", g_stamina);
	SetVisible(context, "StaminaOff", !g_stamina);
	const bool canReturn = g_returnLevel > 0 && g_returnLevel != g_levelId;
	SetVisible(context, "ReturnOn", canReturn);
	SetVisible(context, "ReturnOff", !canReturn);
	SetVisible(context, "ItemError", g_itemError);
	SetVisible(context, "ItemHint", !g_itemError);
}

void Refresh(const D2RL::PluginContext* context) noexcept {
	ShowState(context, nullptr);
	if (g_threads != nullptr) {
		g_threads->runOnUiThread(context, ShowState, nullptr);  // once more when the panel has been built
	}
}

void CloseWindow(const D2RL::PluginContext* context) noexcept {
	if (g_panels != nullptr && g_panel != D2RL::Panels::InvalidHandle) {
		g_panels->closePanel(context, g_panel);
	}
}

// Opens the window, or closes it if it is open. Returns what to tell the player, or "" if fine.
auto ToggleWindow(const D2RL::PluginContext* context) noexcept -> const char* {
	if (g_panels == nullptr || g_panel == D2RL::Panels::InvalidHandle) {
		return "The GM window is not available (see the plugin log).";
	}
	if (!g_inGame) {
		return "Enter a game first.";
	}
	g_itemError = false;
	const auto result = g_panels->togglePanel(context, g_panel);
	if (result != D2RL::Panels::Result::Success) {
		Log(context, "GM: could not open the window (code %d).", static_cast<int>(result));
		return "The GM window could not be opened (see the plugin log).";
	}
	Refresh(context);
	return "";
}

void __cdecl ToggleFromKey(const D2RL::PluginContext* context, void*) noexcept {
	ToggleWindow(context);
}

// ---- Cheats -------------------------------------------------------------------------------

// Sends the three switches to the game. "cheat god" sets all three at once (the only way to
// reach the mana one); the two after it put life and stamina where they should be.
void ApplyCheats(const D2RL::PluginContext* context) noexcept {
	RunConsole(context, g_mana ? "cheat god 1" : "cheat god 0");
	if (g_life != g_mana) {
		RunConsole(context, g_life ? "cheat invincible 1" : "cheat invincible 0");
	}
	if (g_stamina != g_mana) {
		RunConsole(context, g_stamina ? "cheat stamina 1" : "cheat stamina 0");
	}
	Log(context, "GM: unlimited life %s, mana %s, stamina %s.", g_life ? "on" : "off", g_mana ? "on" : "off", g_stamina ? "on" : "off");
}

void __cdecl ReapplyCheats(const D2RL::PluginContext* context, void* session) noexcept {
	if (g_inGame && reinterpret_cast<uintptr_t>(session) == g_session.load() && (g_life || g_mana || g_stamina)) {
		ApplyCheats(context);
	}
}

// ---- Travel -------------------------------------------------------------------------------

auto GoToTown(const D2RL::PluginContext* context) noexcept -> const char* {
	if (!g_inGame || g_levelId <= 0) {
		return "Enter a game first.";
	}
	if (IsTown(g_levelId)) {
		return "You are in town already.";
	}
	g_returnLevel = g_levelId;
	CloseWindow(context);
	char command[32] {};
	std::snprintf(command, sizeof(command), "goto %d", TownOf(g_levelId));
	RunConsole(context, command);
	return "";
}

auto ReturnFromTown(const D2RL::PluginContext* context) noexcept -> const char* {
	if (!g_inGame || g_returnLevel <= 0 || g_returnLevel == g_levelId) {
		return "There is nowhere to return to yet. Use the town button outside town first.";
	}
	const int level = g_returnLevel;
	g_returnLevel = -1;
	CloseWindow(context);
	char command[32] {};
	std::snprintf(command, sizeof(command), "goto %d", level);
	RunConsole(context, command);
	return "";
}

// ---- Items --------------------------------------------------------------------------------

struct ItemOrder {
	char                  code[5];
	D2RL::Items::Quality  quality;
	int                   count;
};

// Game thread: makes the ordered item(s) in the inventory, or on the ground if it is full.
void __cdecl MakeItems(const D2RL::PluginContext* context, void* data) noexcept {
	const ItemOrder* order = static_cast<ItemOrder*>(data);
	D2RL::PlayerHandle player = D2RL::InvalidPlayerHandle;
	if (g_inventory->getLocalPlayer(context, &player) != D2RL::Inventory::Result::Success) {
		context->LogWarn("GM: could not find the player for the item.");
		delete order;
		return;
	}
	char c[4] = { ' ', ' ', ' ', ' ' };
	for (int i = 0; i < 4 && order->code[i] != '\0'; ++i) {
		c[i] = order->code[i];
	}
	for (int n = 0; n < order->count; ++n) {
		D2RL::Items::ItemCreateSpec spec {
			.structSize      = D2RL::Items::ItemCreateSpecSize,
			.flags           = order->quality == D2RL::Items::Quality::Unique ? D2RL::Items::ItemCreateFlagBit(D2RL::Items::ItemCreateFlag::AllowDuplicateUnique) : 0U,
			.code            = D2RL::Items::MakeItemCode(c[0], c[1], c[2], c[3]),
			.quality         = order->quality,
			.qualityRecordId = D2RL::Items::RandomQualityRecord,
			.itemLevel       = 99,
			.seedMode        = D2RL::Items::SeedMode::Random,
			.quantity        = D2RL::Items::DefaultValue,
			.durability      = D2RL::Items::DefaultValue,
			.stateFlags      = D2RL::Items::ItemStateIdentified,
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
			const auto first = result;
			result = g_items->createItem(context, player, &spec, &item);
			Log(context, "GM: item %s (quality %d): inventory refused it (code %d); ground drop %s (code %d).", order->code, static_cast<int>(order->quality),
				static_cast<int>(first), result == D2RL::Items::Result::Success ? "worked" : "failed too", static_cast<int>(result));
		} else {
			Log(context, "GM: item %s (quality %d) put in the inventory.", order->code, static_cast<int>(order->quality));
		}
	}
	delete order;
}

auto QualityFromName(const char* name) noexcept -> D2RL::Items::Quality {
	using Q = D2RL::Items::Quality;
	static constexpr struct { const char* name; Q quality; } Names[] = {
		{ "normal", Q::Normal }, { "magic", Q::Magic }, { "rare", Q::Rare }, { "set", Q::Set }, { "unique", Q::Unique },
		{ "superior", Q::Superior }, { "inferior", Q::Inferior }, { "crafted", Q::Crafted },
	};
	for (const auto& entry : Names) {
		if (_stricmp(name, entry.name) == 0) {
			return entry.quality;
		}
	}
	return Q::Unknown;
}

// Checks the code and queues the item. Returns what to tell the player, or "" if it was queued.
auto SpawnItem(const D2RL::PluginContext* context, const char* code, D2RL::Items::Quality quality, int count) noexcept -> const char* {
	if (!g_inGame || g_threads == nullptr || g_items == nullptr || g_inventory == nullptr) {
		return "Enter a game first.";
	}
	std::string clean;
	for (const char* c = code; *c != '\0'; ++c) {
		if (!std::isspace(static_cast<unsigned char>(*c))) {
			clean += static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
		}
	}
	bool valid = clean.size() >= 3 && clean.size() <= 4;
	for (const char c : clean) {
		valid = valid && std::isalnum(static_cast<unsigned char>(c)) != 0;
	}
	if (!valid) {
		return "An item code has 3 or 4 letters or digits, like rin (ring) or lsd (long sword).";
	}
	auto* order = new (std::nothrow) ItemOrder {};
	if (order == nullptr) {
		return "Out of memory.";
	}
	std::snprintf(order->code, sizeof(order->code), "%s", clean.c_str());
	order->quality = quality;
	order->count = count < 1 ? 1 : count > 40 ? 40 : count;
	if (g_threads->runOnGameThread(context, MakeItems, order) != D2RL::Threads::Result::Success) {
		delete order;
		return "The game is not ready for items right now.";
	}
	return "";
}

// The text typed in the window's item box.
auto TypedItemCode(const D2RL::PluginContext* context) noexcept -> std::string {
	char text[32] {};
	uint32_t needed = 0;
	D2RL::Widgets::WidgetHandle panel = D2RL::Widgets::InvalidHandle;
	D2RL::Widgets::WidgetHandle frame = D2RL::Widgets::InvalidHandle;
	D2RL::Widgets::WidgetHandle box = D2RL::Widgets::InvalidHandle;
	if (g_widgets == nullptr || g_widgets->findPanel(context, PanelName, &panel) != D2RL::Widgets::Result::Success ||
		g_widgets->findWidget(context, panel, "Frame", &frame) != D2RL::Widgets::Result::Success ||
		g_widgets->findWidget(context, frame, "ItemCode", &box) != D2RL::Widgets::Result::Success) {
		context->LogWarn("GM: could not find the item code box.");
		return {};
	}
	const auto result = g_widgets->getInputText(context, box, text, sizeof(text), &needed);
	if (result != D2RL::Widgets::Result::Success) {
		Log(context, "GM: could not read the item code box (code %d).", static_cast<int>(result));
		return {};
	}
	return text;
}

// ---- Events -------------------------------------------------------------------------------

auto RunEvent(const D2RL::PluginContext* context, int index) noexcept -> const char* {
	if (index < 0 || index >= g_eventCount) {
		return "No such event. \"gm events\" lists them.";
	}
	if (!g_inGame) {
		return "Enter a game first.";
	}
	const Event& event = g_events[index];
	CloseWindow(context);  // an event may open a window of its own
	Log(context, "GM: event %s.", event.id);
	if (event.console != nullptr) {
		RunConsole(context, event.console);
	} else {
		event.run(context);
	}
	return "";
}

// ---- Actions from the window's buttons ----------------------------------------------------

void OnAction(const D2RL::PluginContext* context, const char* action) noexcept {
	const char* problem = "";
	if (std::strcmp(action, "life") == 0) {
		g_life = !g_life;
		ApplyCheats(context);
	} else if (std::strcmp(action, "mana") == 0) {
		g_mana = !g_mana;
		ApplyCheats(context);
	} else if (std::strcmp(action, "stamina") == 0) {
		g_stamina = !g_stamina;
		ApplyCheats(context);
	} else if (std::strcmp(action, "town") == 0) {
		problem = GoToTown(context);
	} else if (std::strcmp(action, "return") == 0) {
		problem = ReturnFromTown(context);
	} else if (std::strncmp(action, "item-", 5) == 0) {
		problem = SpawnItem(context, TypedItemCode(context).c_str(), QualityFromName(action + 5), 1);
		g_itemError = problem[0] != '\0';
	} else if (std::strcmp(action, "gold") == 0) {
		// A hero may carry 10,000 gold per character level, and "cheat gold" empties the purse when
		// an amount would pass that (50,000 at once left low-level heroes with 0). So give what
		// still fits, up to 50,000; if the hero can't be read, 5,000, which fits even at level 1.
		int amount = 5000;
		const Game::HeroDetails hero = Game::HeroInfo();
		if (hero.level > 0 && hero.gold >= 0) {
			amount = (std::min)(50000, hero.level * 10000 - hero.gold);
		}
		if (amount > 0) {
			char command[40] {};
			std::snprintf(command, sizeof(command), "cheat gold %d", amount);
			RunConsole(context, command);
		} else {
			problem = "This hero carries all the gold its level allows (10,000 per level).";
		}
	} else if (std::strcmp(action, "potions") == 0) {
		problem = SpawnItem(context, "rvl", D2RL::Items::Quality::Normal, 4);
	} else if (std::strcmp(action, "identify") == 0) {
		RunConsole(context, "cheat idall");
	} else if (std::strncmp(action, "event-", 6) == 0) {
		problem = RunEvent(context, std::atoi(action + 6));
	} else {
		Log(context, "GM: unknown button action %s.", action);
	}
	if (problem[0] != '\0') {
		Log(context, "GM: %s", problem);
		context->WriteConsoleMessage(problem);
	}
	Refresh(context);
}

// ---- Console and key ----------------------------------------------------------------------

constexpr char Usage[] = "gm | gm life|mana|stamina [0|1] | gm town | gm return | gm item <code> [normal|magic|rare|set|unique] [count] | gm events | gm event <id>";

auto GmCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	using R = D2RL::ConsoleCommandResult;
	if (command == nullptr || command->plugin == nullptr) {
		return R::Failed;
	}
	const D2RL::PluginContext* context = command->plugin;
	char args[128] {};
	std::snprintf(args, sizeof(args), "%s", command->args != nullptr ? command->args : "");
	char* words[4] {};
	int count = 0;
	for (char* word = std::strtok(args, " \t"); word != nullptr && count < 4; word = std::strtok(nullptr, " \t")) {
		words[count++] = word;
	}
	const char* problem = "";
	if (count == 0) {
		problem = ToggleWindow(context);
	} else if (_stricmp(words[0], "life") == 0 || _stricmp(words[0], "hp") == 0 || _stricmp(words[0], "mana") == 0 || _stricmp(words[0], "stamina") == 0) {
		bool& flag = _stricmp(words[0], "mana") == 0 ? g_mana : _stricmp(words[0], "stamina") == 0 ? g_stamina : g_life;
		flag = count > 1 ? std::atoi(words[1]) != 0 : !flag;
		if (g_inGame) {
			ApplyCheats(context);
		} else {
			problem = "Saved. It starts when you enter a game.";
		}
		Refresh(context);
	} else if (_stricmp(words[0], "town") == 0) {
		problem = GoToTown(context);
	} else if (_stricmp(words[0], "return") == 0) {
		problem = ReturnFromTown(context);
	} else if (_stricmp(words[0], "item") == 0 && count >= 2) {
		const auto quality = count >= 3 ? QualityFromName(words[2]) : D2RL::Items::Quality::Normal;
		if (quality == D2RL::Items::Quality::Unknown) {
			context->WriteConsoleMessage(Usage);
			return R::InvalidArguments;
		}
		problem = SpawnItem(context, words[1], quality, count >= 4 ? std::atoi(words[3]) : 1);
	} else if (_stricmp(words[0], "events") == 0) {
		for (int i = 0; i < g_eventCount; ++i) {
			char line[160] {};
			std::snprintf(line, sizeof(line), "%s: %s", g_events[i].id, g_events[i].label);
			context->WriteConsoleMessage(line);
		}
		if (g_eventCount == 0) {
			context->WriteConsoleMessage("No events yet.");
		}
	} else if (_stricmp(words[0], "event") == 0 && count >= 2) {
		int index = -1;
		for (int i = 0; i < g_eventCount; ++i) {
			if (_stricmp(words[1], g_events[i].id) == 0) {
				index = i;
			}
		}
		problem = RunEvent(context, index);
	} else {
		context->WriteConsoleMessage(Usage);
		return R::InvalidArguments;
	}
	if (problem[0] != '\0') {
		context->WriteConsoleMessage(problem);
	}
	return R::Handled;
}

auto __cdecl OnKey(const D2RL::PluginContext* context, const D2RL::Input::ActionEvent* event, void*) noexcept -> D2RL::Input::ActionResult {
	if (context == nullptr || event == nullptr || event->kind != D2RL::Input::ActionEventKind::Pressed || g_threads == nullptr) {
		return D2RL::Input::ActionResult::Ignored;
	}
	g_threads->runOnUiThread(context, ToggleFromKey, nullptr);
	return D2RL::Input::ActionResult::Handled;
}

}  // namespace

auto RegisterEvent(const Event& event) noexcept -> bool {
	if (g_eventCount >= MaxEvents || event.id == nullptr || event.label == nullptr || (event.console == nullptr && event.run == nullptr)) {
		return false;
	}
	g_events[g_eventCount++] = event;
	return true;
}

auto Load(const D2RL::PluginContext* context) noexcept -> bool {
	try {
		const D2RL::ResourceService* resources = nullptr;
		if (context->QueryService(&g_threads) != D2RL::ServiceQueryResult::Success || !D2RL::HasThreadServiceField(g_threads, D2RL::ThreadServiceRequiredSize)) {
			g_threads = nullptr;
		}
		if (context->QueryService(&g_inventory) != D2RL::ServiceQueryResult::Success || !D2RL::HasInventoryServiceField(g_inventory, D2RL::InventoryServiceRequiredSize)) {
			g_inventory = nullptr;
		}
		if (context->QueryService(&g_items) != D2RL::ServiceQueryResult::Success || !D2RL::HasItemServiceField(g_items, D2RL::ItemServiceRequiredSize)) {
			g_items = nullptr;
		}
		if (context->QueryService(&g_widgets) != D2RL::ServiceQueryResult::Success || !D2RL::HasWidgetServiceField(g_widgets, D2RL::WidgetServiceRequiredSize)) {
			g_widgets = nullptr;
		}
		if (context->QueryService(&resources) == D2RL::ServiceQueryResult::Success && D2RL::HasResourceServiceField(resources, D2RL::ResourceServiceRequiredSize) &&
			context->QueryService(&g_panels) == D2RL::ServiceQueryResult::Success && D2RL::HasPanelServiceField(g_panels, D2RL::PanelServiceRequiredSize)) {
			const std::string layout = LayoutJson();
			const D2RL::Resources::ResourceRegistration resource {
				.structSize = D2RL::Resources::ResourceRegistrationSize,
				.path       = LayoutPath,
				.bytes      = layout.data(),
				.byteCount  = layout.size(),
			};
			D2RL::Resources::RegistrationHandle resourceHandle = D2RL::Resources::InvalidHandle;
			const D2RL::Panels::PanelRegistration registration {
				.structSize = D2RL::Panels::PanelRegistrationSize,
				.flags      = D2RL::Panels::PanelFlags::CloseOnEscape,
				.localId    = PanelLocalId,
			};
			if (resources->registerResource(context, &resource, &resourceHandle) != D2RL::Resources::Result::Success ||
				g_panels->registerPanel(context, &registration, &g_panel) != D2RL::Panels::Result::Success) {
				context->LogWarn("GM: could not register the window; the console commands still work.");
				g_panel = D2RL::Panels::InvalidHandle;
			}
		} else {
			g_panels = nullptr;
			context->LogWarn("GM: panel services missing; the console commands still work.");
		}

		// The key. Optional: the console command opens the window too.
		const D2RL::InputService* input = nullptr;
		if (context->QueryService(&input) == D2RL::ServiceQueryResult::Success && D2RL::HasInputServiceField(input, D2RL::InputServiceRequiredSize)) {
			const D2RL::Input::ActionRegistration action {
				.structSize       = D2RL::Input::ActionRegistrationSize,
				.logicalId        = "gm-window",
				.displayName      = "GM tools window",
				.category         = "Darkest Diablo",
				.defaultPrimary   = { .key = D2RL::Input::Key::G, .modifier = D2RL::Input::Modifier::Control },
				.defaultSecondary = { .key = D2RL::Input::Key::None },
				.callback         = OnKey,
			};
			D2RL::Input::ActionHandle handle = D2RL::Input::InvalidHandle;
			if (input->registerAction(context, &action, &handle) != D2RL::Input::Result::Success) {
				context->LogWarn("GM: could not add the key for the window.");
			}
		}

		Log(context, "GM: window ready with %d event(s).", g_eventCount);
		// "/gm" as a second name, if the console takes a name with a slash.
		D2RL::ConsoleCommandRegistration command = D2RL::MakeConsoleCommand("gm", GmCommand, "Open the GM tools window (cheats for testing), or run one of its tools.");
		command.usage = Usage;
		command.aliases = "/gm";
		if (context->RegisterConsoleCommand(command)) {
			return true;
		}
		context->LogWarn("GM: the console refused the name /gm; using gm only.");
		command.aliases = nullptr;
		return context->RegisterConsoleCommand(command);
	} catch (...) {
		context->LogWarn("GM: setting up the window failed.");
		return true;
	}
}

auto OnUiMessage(const D2RL::PluginContext* context, const char* target, const char* command, const char* text) noexcept -> bool {
	if (std::strcmp(target, "PanelManager") != 0 || std::strcmp(command, "OpenPanel") != 0 ||
		std::strncmp(text, ActionPrefix, sizeof(ActionPrefix) - 1) != 0) {
		return false;
	}
	try {
		OnAction(context, text + sizeof(ActionPrefix) - 1);
	} catch (...) {
		context->LogWarn("GM: a button action failed.");
	}
	return true;
}

void OnGameplayEvent(const D2RL::PluginContext* context, const D2RL::Lifecycle::GameplayEvent* event) noexcept {
	using Kind = D2RL::Lifecycle::GameplayEventKind;
	if (event->kind == Kind::LevelChanged) {
		g_levelId = event->currentValue;
		g_inGame = true;
		Refresh(context);
	} else if (event->kind == Kind::LocalPlayerReady) {
		g_inGame = true;
		// The switches stay on from game to game; give the new game a moment, then set them again.
		if ((g_life || g_mana || g_stamina) && g_threads != nullptr) {
			const uintptr_t session = g_session.load();
			const auto* threads = g_threads;
			std::thread([context, threads, session] {
				std::this_thread::sleep_for(std::chrono::milliseconds(1500));
				if (session == g_session.load()) {
					threads->runOnUiThread(context, ReapplyCheats, reinterpret_cast<void*>(session));
				}
			}).detach();
		}
	} else if (event->kind == Kind::GameLeft) {
		++g_session;
		g_inGame = false;
		g_levelId = -1;
		g_returnLevel = -1;
	}
}

}
