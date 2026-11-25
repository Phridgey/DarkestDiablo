// Darkest Diablo — the caravan's stash in the game: its panels, gold, items and page locks.
// The rules are in stash.h; docs/stash.md says how it plays and what is still to be checked.
//
// - Panels: the caravan window and the Upgrades tab change while the game runs, so each is a row
//   of spare panels like the Campfire's (campfire.cpp, LivePanel).
// - Items: putting one in the Upgrades tab destroys it and counts it in stash.txt; taking one out
//   makes a new one. Gems, runes and potions have nothing of their own to lose.
// - Pages: the arrows stop at the last unlocked page, and once in a while anything that still
//   ended up on a locked page, or in the hero's own (hidden) stash page, is moved to an open one.
//   Nothing is ever destroyed for that: what fits nowhere stays where it is.

#include "stash.h"
#include "stashgame.h"
#include "journal.h"
#include "stashfile.h"
#include "campfire.h"
#include "legacy.h"
#include "gameapi.h"
#include "gamestate.h"
#include <D2RLPlugin/core_exports.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace StashGame {

namespace {

namespace fs = std::filesystem;
using Container = D2RL::Items::ItemContainer;

constexpr char ActionPrefix[] = "darkestdiablo/stash-";  // buttons send PanelManager:OpenPanel:darkestdiablo/stash-<action>
constexpr char BankPanel[]    = "BankExpansionLayout";
constexpr int  SweepMs        = 250;  // was 700: what is put on an ascended hero's page goes back at once
constexpr int  PayPollMs      = 40;    // how often a payment looks whether the gold moved
constexpr int  PayLimitMs     = 1500;

const D2RL::ThreadService*    g_threads   = nullptr;
const D2RL::PanelService*     g_panels    = nullptr;
const D2RL::WidgetService*    g_widgets   = nullptr;
const D2RL::ResourceService*  g_resources = nullptr;
const D2RL::InventoryService* g_inventory = nullptr;
const D2RL::ItemService*      g_items     = nullptr;
bool g_sharedWrite = false;

std::recursive_mutex g_mutex;  // guards the state below
using Lock = std::lock_guard<std::recursive_mutex>;
Stash::State g_state;
fs::path     g_file;           // the stash.txt g_state came from

Legacy::State g_legacy;        // ascended heroes' items (docs/ascend.md); read with g_state
using ItemId = std::pair<uint64_t, uint32_t>;
std::map<int, std::vector<ItemId>> g_known;  // what may lie on each ascended hero's page; empty until the first look of a visit
std::atomic<bool>      g_delivering { false };

// live folder and slot from camp/slot<N>/progress/<hero>.txt
auto LiveOf(const fs::path& progress) -> fs::path { return progress.parent_path().parent_path().parent_path().parent_path(); }
auto SlotOf(const fs::path& progress) -> int { return std::atoi(progress.parent_path().parent_path().filename().string().c_str() + 4); }

std::atomic<bool>      g_bankOpen { false };
std::atomic<uintptr_t> g_visit { 0 };       // counts stash openings, so an old sweeper stops
std::atomic<int>       g_page { 1 };        // the save's page we believe is showing (from 1)
std::atomic<int>       g_base { 0 };        // the save's first page in the stash file (from 0): stashfile.h
std::atomic<int>       g_tabBase { Stash::PageCount };  // the first of its 4 pages for ascended heroes there
std::atomic<bool>      g_wipe { false };    // the save's pages still hold a deleted save's items
std::atomic<bool>      g_autoTab { true };  // open the stash on the shared pages (dd-stash autotab: off)
std::atomic<bool>      g_treeWanted { false }, g_tabWanted { false };
std::atomic<bool>      g_journalWanted { false };  // starts minimized; stays as it is between visits
std::atomic<int>       g_journalRune { -1 }, g_journalPage { 0 };
std::atomic<bool>      g_jumping { false };  // dd-stash jump: the page lock lets these arrows through
std::atomic<bool>      g_refresh { false };
std::atomic<bool>      g_paying { false };

// A payment on its way: what the hero had, and what should be left.
enum class Moving { Pay, Deposit, Take };
struct Payment { Moving kind = Moving::Pay; long long total = 0, purse = 0; int gold = 0, bank = 0, goldAfter = 0, bankAfter = 0; ULONGLONG started = 0; };
Payment g_payment;
std::string g_payNote;  // under the Pay buttons
size_t      g_goldStep = 4;  // Stash::GoldSteps: 10,000

void Log(const D2RL::PluginContext* context, const char* format, ...) noexcept {
	char message[512] {};
	va_list args;
	va_start(args, format);
	std::vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	context->LogInfo(message);
}

void Later(const D2RL::PluginContext* context, int delayMs, D2RL::Threads::Callback callback, void* data = nullptr) noexcept {
	try {
		std::thread([=] {
			std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
			g_threads->runOnUiThread(context, callback, data);
		}).detach();
	} catch (...) {
	}
}

// ---- The save's file ------------------------------------------------------------------------
// The save of the current game, read again when it changes (another save, or a test edit by hand
// needs a restart). Returns false outside a save.
// `progress` is Campfire::RouteProgressFile() (camp/slot<N>/progress/<hero>.txt). Always ask for it
// before locking g_mutex: the Campfire sends UI messages under its own lock, which come back here.
auto Current(const fs::path& progress) -> bool {
	if (progress.empty()) {
		return false;
	}
	const fs::path file = progress.parent_path().parent_path() / "stash.txt";
	if (file != g_file) {
		std::error_code ec;
		g_state = fs::exists(file, ec) ? Stash::Parse(Camp::ReadFile(file)) : Stash::State {};
		g_file = file;
		// camp/slot<N>/stash.txt: which save this is, and so which pages of the stash file are its own.
		const fs::path live = file.parent_path().parent_path().parent_path();
		const int slot = std::clamp(std::atoi(file.parent_path().filename().string().c_str() + 4), 1, Camp::SlotCount);
		const bool joined = Camp::StashJoined(live);
		g_base = joined ? (slot - 1) * StashFile::PagesPerSave : 0;
		g_tabBase = joined ? StashFile::AllPages + (slot - 1) * StashFile::TabsPerSave : Stash::PageCount;
		g_wipe = joined && StashFile::Wipes(live).count(slot) != 0;
		if (joined && !g_state.purseMoved) {  // the gold its own stash file held
			g_state.purse += StashFile::JoinedGold(live, slot);
			g_state.purseMoved = true;
			Camp::WriteFile(g_file, Stash::Serialize(g_state));
		}
		g_legacy = Legacy::Load(LiveOf(progress), SlotOf(progress));
	}
	return true;
}

auto Save(const D2RL::PluginContext* context) -> bool {
	if (!g_file.empty() && Camp::WriteFile(g_file, Stash::Serialize(g_state))) {
		return true;
	}
	context->LogWarn("Stash: could not write stash.txt.");
	return false;
}

auto RunConsole(const D2RL::PluginContext* context, const char* command) noexcept -> bool {
	namespace CE = D2RL::CoreExports;
	const HMODULE core = GetModuleHandleA(CE::CoreDllName);
	const auto execute = core == nullptr ? nullptr : reinterpret_cast<CE::ExecuteConsoleCommandFn>(GetProcAddress(core, CE::ExecuteConsoleCommandInfo.name));
	const bool ok = execute != nullptr && execute(command);
	Log(context, "Stash: console %s -> %s", command, ok ? "sent" : "refused");
	return ok;
}

void Dispatch(const D2RL::PluginContext* context, const char* target, const char* command, const char* text) noexcept {
	if (g_widgets != nullptr) {
		const D2RL::Widgets::UiAction action { .structSize = D2RL::Widgets::UiActionSize, .target = target, .command = command, .text = text };
		const auto result = g_widgets->dispatchUiAction(context, &action);
		Log(context, "Stash: sent %s:%s:%s (code %d).", target, command, text, static_cast<int>(result));
	}
}

// ---- Panels -----------------------------------------------------------------------------------
// The game keeps a panel it has shown and does not read its layout again, so each new content goes
// to the next panel of the row; the one it replaces is closed and the game is asked to forget it
// at once, long before the row comes round to it again.
struct Row {
	const char* base;
	int         count;
	std::vector<D2RL::Panels::RegistrationHandle>    panels {};
	std::vector<D2RL::Resources::RegistrationHandle> layouts {};
	int         current = -1;
	std::string content {};
};
// A plugin may register 64 panels in all; the whole plugin uses 59 now.
Row g_tree { "StashTree", 2 };
Row g_wall { "StashWall", 2 };
Row g_tab  { "StashTab", 2 };
Row g_journal { "StashJournal", 2 };
Row g_pageRow { "StashPage", 2 };
Row g_name { "StashName", 2 };  // whose ascended items the page holds
D2RL::Panels::RegistrationHandle g_side = D2RL::Panels::InvalidHandle;

auto RowName(const Row& row, int i) -> std::string {
	return std::string("darkestdiablo/") + row.base + std::to_string(i);
}

auto IsOpen(const D2RL::PluginContext* context, D2RL::Panels::RegistrationHandle panel) noexcept -> bool {
	D2RL::Panels::PanelInfo info { .structSize = D2RL::Panels::PanelInfoSize };
	return panel != D2RL::Panels::InvalidHandle && g_panels->getPanelInfo(context, panel, &info) == D2RL::Panels::Result::Success &&
		info.presentationState == D2RL::Panels::PresentationState::Open;
}

auto RowOpen(const D2RL::PluginContext* context, const Row& row) noexcept -> bool {
	return row.current >= 0 && IsOpen(context, row.panels[static_cast<size_t>(row.current)]);
}

void CloseRow(const D2RL::PluginContext* context, const Row& row) noexcept {
	if (RowOpen(context, row)) {
		g_panels->closePanel(context, row.panels[static_cast<size_t>(row.current)]);
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

// UI thread. Shows `json` in the row: the panel that holds it already, or the next one.
void ShowRow(const D2RL::PluginContext* context, Row& row, const std::string& json) {
	if (row.panels.empty()) {
		return;
	}
	if (row.current >= 0 && row.content == json) {
		if (!RowOpen(context, row)) {
			g_panels->openPanel(context, row.panels[static_cast<size_t>(row.current)]);
		}
		return;
	}
	const int old = row.current;
	const int i = (old + 1) % static_cast<int>(row.panels.size());
	const std::string name = RowName(row, i);
	auto& layout = row.layouts[static_cast<size_t>(i)];
	if (layout != D2RL::Resources::InvalidHandle) {
		g_resources->unregisterResource(context, layout);
		layout = D2RL::Resources::InvalidHandle;
	}
	if (!RegisterLayout(context, name, json, layout)) {
		Log(context, "Stash: %s could not take its new layout.", name.c_str());
		return;
	}
	const auto result = g_panels->openPanel(context, row.panels[static_cast<size_t>(i)]);
	if (result != D2RL::Panels::Result::Success) {
		Log(context, "Stash: opening %s gave code %d.", name.c_str(), static_cast<int>(result));
	}
	row.current = i;
	row.content = result == D2RL::Panels::Result::Success ? json : std::string();  // failed: the next try uses the next panel
	if (old >= 0) {
		const auto oldPanel = row.panels[static_cast<size_t>(old)];
		if (IsOpen(context, oldPanel)) {
			g_panels->closePanel(context, oldPanel);
		}
		Dispatch(context, "PanelManager", "UnloadPanel", RowName(row, old).c_str());
	}
}

// UI thread: what should be open is open, with the newest content.
void __cdecl Refresh(const D2RL::PluginContext* context, void*) noexcept {
	g_refresh = false;
	if (g_panels == nullptr) {
		return;
	}
	try {
		const fs::path progress = Campfire::RouteProgressFile();
		const Lock lock(g_mutex);
		if (!g_bankOpen.load() || !Current(progress)) {
			for (const Row* row : { &g_tree, &g_wall, &g_tab, &g_journal, &g_pageRow, &g_name }) {
				CloseRow(context, *row);
			}
			if (IsOpen(context, g_side)) {
				g_panels->closePanel(context, g_side);
			}
			return;
		}
		if (g_side != D2RL::Panels::InvalidHandle && !IsOpen(context, g_side)) {
			g_panels->openPanel(context, g_side);
		}
		const auto estate = g_legacy.tabs.find(g_page.load() - Stash::PageCount - 1);
		if (estate != g_legacy.tabs.end() && !g_tabWanted.load() && !g_treeWanted.load()) {
			ShowRow(context, g_name, Stash::NameLayout(estate->second));
		} else {
			CloseRow(context, g_name);
		}
		if (g_tabWanted.load() && g_state.Has("tab")) {
			ShowRow(context, g_tab, Stash::UpgradesLayout(g_state));
		} else {
			CloseRow(context, g_tab);
		}
		if (g_treeWanted.load()) {
			ShowRow(context, g_tree, Stash::TreeLayout(g_state));
			ShowRow(context, g_wall, Stash::WallLayout(g_state, g_payNote));
		} else {
			CloseRow(context, g_tree);
			CloseRow(context, g_wall);
		}
		if (!g_treeWanted.load() && !g_tabWanted.load()) {
			// Ascended heroes' pages count on after the last open one.
			const int open = g_state.legacyDone ? g_state.LastPage() : Stash::PageCount;
			int shown = g_page.load();
			if (shown > Stash::PageCount) {
				shown = open + 1 + static_cast<int>(std::distance(g_legacy.tabs.begin(), g_legacy.tabs.lower_bound(shown - Stash::PageCount - 1)));
			}
			ShowRow(context, g_pageRow, Stash::PageLayout(shown, open + static_cast<int>(g_legacy.tabs.size()), g_state.purse, g_goldStep));
		} else {
			CloseRow(context, g_pageRow);
		}
		if (g_journalWanted.load() && !g_treeWanted.load()) {  // the caravan window lies over its place
			ShowRow(context, g_journal, Journal::Layout(g_journalRune.load(), g_journalPage.load()));
		} else {
			CloseRow(context, g_journal);
		}
	} catch (...) {
		context->LogWarn("Stash: refreshing the panels failed.");
	}
}

void QueueRefresh(const D2RL::PluginContext* context) noexcept {
	if (!g_refresh.exchange(true) && g_threads->runOnUiThread(context, Refresh, nullptr) != D2RL::Threads::Result::Success) {
		g_refresh = false;
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

auto __cdecl Collect(const D2RL::PluginContext*, const D2RL::Items::ItemInfo* item, void* out) noexcept -> D2RL::Inventory::IterationAction {
	try {
		if (item != nullptr) {
			static_cast<std::vector<D2RL::Items::ItemInfo>*>(out)->push_back(*item);
		}
	} catch (...) {
		return D2RL::Inventory::IterationAction::Stop;
	}
	return D2RL::Inventory::IterationAction::Continue;
}

// Game thread. The local player's items in the given containers; false when they can't be read.
auto ItemsIn(const D2RL::PluginContext* context, uint32_t mask, D2RL::PlayerHandle& player, std::vector<D2RL::Items::ItemInfo>& out) -> bool {
	const D2RL::Inventory::ItemFilter filter { .structSize = D2RL::Inventory::ItemFilterSize, .containerMask = mask };
	return g_inventory->getLocalPlayer(context, &player) == D2RL::Inventory::Result::Success &&
		g_inventory->forEachInventoryItem(context, player, &filter, Collect, &out) == D2RL::Inventory::Result::Success;
}

// Game thread, locked. Puts one item in the Upgrades tab if its slot takes it.
auto Store(const D2RL::PluginContext* context, D2RL::PlayerHandle player, const D2RL::Items::ItemInfo& item) -> bool {
	const Stash::Kind* kind = Stash::KindOf(CodeText(item.code));
	const int n = (std::max)(1, item.quantity);
	if (kind == nullptr || g_state.Room(*kind) < n || item.socketedItemCount != 0) {
		return false;
	}
	const auto result = g_items->destroyItem(context, player, item.handle, D2RL::Items::SocketedItemPolicy::RejectIfNotEmpty);
	if (result != D2RL::Items::Result::Success) {
		Log(context, "Stash: storing %s failed (code %d).", kind->code.c_str(), static_cast<int>(result));
		return false;
	}
	g_state.Add(kind->code, n);
	return true;
}

// Game thread. Stores from the bag: one item by its number (`data`), or everything (0), or
// everything because the hero came to town (Porter).
constexpr uintptr_t StoreAll = 0, StorePorter = static_cast<uintptr_t>(-1);

void __cdecl StoreFromBag(const D2RL::PluginContext* context, void* data) noexcept {
	try {
		const fs::path progress = Campfire::RouteProgressFile();
		const Lock lock(g_mutex);
		const uintptr_t what = reinterpret_cast<uintptr_t>(data);
		D2RL::PlayerHandle player = D2RL::InvalidPlayerHandle;
		std::vector<D2RL::Items::ItemInfo> items;
		if (!Current(progress) || !g_state.Has("tab") || (what == StorePorter && !g_state.Has("porter")) ||
			!ItemsIn(context, D2RL::Items::ContainerBit(Container::Inventory), player, items)) {
			return;
		}
		int stored = 0;
		for (const auto& item : items) {
			const Stash::Kind* kind = Stash::KindOf(CodeText(item.code));
			if (kind == nullptr ||
				(what != StoreAll && what != StorePorter && item.runtimeId != static_cast<uint32_t>(what))) {
				continue;
			}
			stored += Store(context, player, item) ? 1 : 0;
		}
		if (stored > 0) {
			Save(context);
			Log(context, "Stash: stored %d item(s) from the bag.", stored);
			QueueRefresh(context);
		}
	} catch (...) {
		context->LogWarn("Stash: storing from the bag failed.");
	}
}

// Game thread. A slot was clicked: an item in the hand is stored if it belongs in the tab, else
// one of the slot's kind is taken out, into the hand (the bag if the hand is full). With StoreOnly
// in `data` nothing is taken out: the click looked a rune up in the journal.
constexpr size_t StoreOnly = 0x10000;

void __cdecl ClickSlot(const D2RL::PluginContext* context, void* data) noexcept {
	try {
		const fs::path progress = Campfire::RouteProgressFile();
		const Lock lock(g_mutex);
		const auto& kinds = Stash::Kinds();
		const size_t index = reinterpret_cast<size_t>(data) & (StoreOnly - 1);
		D2RL::PlayerHandle player = D2RL::InvalidPlayerHandle;
		if (index >= kinds.size() || !Current(progress) || !g_state.Has("tab") || g_inventory->getLocalPlayer(context, &player) != D2RL::Inventory::Result::Success) {
			return;
		}
		D2RL::ItemHandle hand = D2RL::InvalidItemHandle;
		D2RL::Items::ItemInfo held { .structSize = static_cast<uint32_t>(sizeof(D2RL::Items::ItemInfo)) };
		const bool holding = g_inventory->getCursorItem(context, player, &hand) == D2RL::Inventory::Result::Success && hand != D2RL::InvalidItemHandle;
		if (holding) {
			if (g_items->getItemInfo(context, hand, &held) == D2RL::Items::Result::Success && Store(context, player, held)) {
				Save(context);
				QueueRefresh(context);
			}
			return;
		}
		const Stash::Kind& kind = kinds[index];
		if ((reinterpret_cast<size_t>(data) & StoreOnly) != 0 || g_state.Count(kind.code) <= 0) {
			return;
		}
		D2RL::Items::ItemCreateSpec spec {
			.structSize      = D2RL::Items::ItemCreateSpecSize,
			.code            = CodeNumber(kind.code),
			.quality         = D2RL::Items::Quality::Normal,
			.qualityRecordId = D2RL::Items::RandomQualityRecord,
			.itemLevel       = 1,
			.seedMode        = D2RL::Items::SeedMode::Random,
			.quantity        = D2RL::Items::DefaultValue,
			.durability      = D2RL::Items::DefaultValue,
			.destination     = {
				.structSize = D2RL::Items::ItemDestinationSize,
				.container  = Container::Cursor,
				.placement  = D2RL::Items::Placement::Automatic,
			},
		};
		// Counted out first: if the game stops here, an item is lost rather than doubled.
		g_state.Add(kind.code, -1);
		if (!Save(context)) {
			g_state.Add(kind.code, 1);
			return;
		}
		D2RL::ItemHandle made = D2RL::InvalidItemHandle;
		auto result = g_items->createItem(context, player, &spec, &made);
		if (result != D2RL::Items::Result::Success) {
			spec.destination.container = Container::Inventory;
			result = g_items->createItem(context, player, &spec, &made);
		}
		if (result != D2RL::Items::Result::Success) {
			g_state.Add(kind.code, 1);
			Save(context);
			Log(context, "Stash: taking out %s failed (code %d); it stays in its slot.", kind.code.c_str(), static_cast<int>(result));
		}
		QueueRefresh(context);
	} catch (...) {
		context->LogWarn("Stash: a slot click failed.");
	}
}

void __cdecl LeaveTab(const D2RL::PluginContext* context, void*) noexcept;

// ---- Page locks ------------------------------------------------------------------------------
// `page`: in the stash file, from 0. With `x`, `y` at that cell, otherwise where there is room.
auto MoveToFilePage(const D2RL::PluginContext* context, D2RL::PlayerHandle player, D2RL::ItemHandle item, int page, int x = -1, int y = -1) -> bool {
	D2RL::Items::ExistingItemOperation operation {};
	operation.structSize                        = D2RL::Items::ExistingItemOperationSize;
	operation.kind                              = D2RL::Items::ExistingItemOperationKind::Move;
	operation.item                              = item;
	operation.move.destination.structSize       = D2RL::Items::ItemDestinationSize;
	operation.move.destination.container        = Container::SharedStash;
	operation.move.destination.placement        = D2RL::Items::Placement::Automatic;
	if (x >= 0 && y >= 0) {
		operation.move.destination.placement    = D2RL::Items::Placement::Exact;
		operation.move.destination.x            = static_cast<uint32_t>(x);
		operation.move.destination.y            = static_cast<uint32_t>(y);
	}
	operation.move.destination.sharedStashPage  = static_cast<uint32_t>(page);
	const D2RL::Items::ExistingItemTransaction transaction {
		.structSize     = D2RL::Items::ExistingItemTransactionSize,
		.player         = player,
		.operationCount = 1,
		.operations     = &operation,
	};
	D2RL::Items::ExistingItemTransactionResult result { .structSize = D2RL::Items::ExistingItemTransactionResultSize };
	return g_items->executeExistingItemTransaction(context, &transaction, &result) == D2RL::Items::Result::Success;
}

// `page`: the save's own, from 0.
auto MoveTo(const D2RL::PluginContext* context, D2RL::PlayerHandle player, D2RL::ItemHandle item, int page) -> bool {
	return MoveToFilePage(context, player, item, g_base.load() + page);
}

auto IdOf(const D2RL::Items::ItemInfo& item) -> ItemId {
	return { static_cast<uint64_t>(item.generationSeed) << 32 | item.itemSeed, item.code };
}

auto MoveToBag(const D2RL::PluginContext* context, D2RL::PlayerHandle player, D2RL::ItemHandle item) -> bool {
	D2RL::Items::ExistingItemOperation operation {};
	operation.structSize                  = D2RL::Items::ExistingItemOperationSize;
	operation.kind                        = D2RL::Items::ExistingItemOperationKind::Move;
	operation.item                        = item;
	operation.move.destination.structSize = D2RL::Items::ItemDestinationSize;
	operation.move.destination.container  = Container::Inventory;
	operation.move.destination.placement  = D2RL::Items::Placement::Automatic;
	const D2RL::Items::ExistingItemTransaction transaction {
		.structSize     = D2RL::Items::ExistingItemTransactionSize,
		.player         = player,
		.operationCount = 1,
		.operations     = &operation,
	};
	D2RL::Items::ExistingItemTransactionResult result { .structSize = D2RL::Items::ExistingItemTransactionResultSize };
	return g_items->executeExistingItemTransaction(context, &transaction, &result) == D2RL::Items::Result::Success;
}

// Game thread, locked. The ascended heroes' pages: only what was there may lie there; anything put
// in goes back to the bag (else an open page), and an emptied page is gone.
void GuardTabs(const D2RL::PluginContext* context, D2RL::PlayerHandle player, const std::vector<D2RL::Items::ItemInfo>& items, const fs::path& progress) {
	if (g_legacy.tabs.empty()) {
		return;
	}
	std::vector<int> emptied;
	for (const auto& [n, name] : g_legacy.tabs) {
		std::vector<ItemId> now;
		for (const auto& item : items) {
			if (item.container == Container::SharedStash && item.sharedStashPage == static_cast<uint32_t>(g_tabBase.load() + n)) {
				now.push_back(IdOf(item));
			}
		}
		const auto [known, first] = g_known.try_emplace(n, now);
		for (const ItemId& id : first ? std::vector<ItemId> {} : Legacy::Strangers(known->second, now)) {
			for (const auto& item : items) {
				if (item.container != Container::SharedStash || item.sharedStashPage != static_cast<uint32_t>(g_tabBase.load() + n) || IdOf(item) != id) {
					continue;
				}
				bool moved = MoveToBag(context, player, item.handle);
				for (int page = 0; page < g_state.Pages() && !moved; ++page) {
					moved = MoveTo(context, player, item.handle, page);
				}
			}
		}
		if (now.empty()) {
			emptied.push_back(n);
		}
	}
	for (const int n : emptied) {
		Log(context, "Stash: %s's page is empty and goes away.", g_legacy.tabs[n].c_str());
		g_legacy.tabs.erase(n);
		g_known.erase(n);
		Legacy::Save(LiveOf(progress), SlotOf(progress), g_legacy);
		if (g_page.load() == Stash::PageCount + 1 + n) {
			g_threads->runOnUiThread(context, LeaveTab, nullptr);
		}
		QueueRefresh(context);
	}
}

// Game thread, soon after a hero came in: the items of an ascended hero it carries in its own
// (hidden) stash page go to their page. Tried until done; they lie safe in the hero's save meanwhile.
int g_deliverTries = 0;

void __cdecl Deliver(const D2RL::PluginContext* context, void*) noexcept {
	try {
		const fs::path progress = Campfire::RouteProgressFile();
		const Lock lock(g_mutex);
		D2RL::PlayerHandle player = D2RL::InvalidPlayerHandle;
		std::vector<D2RL::Items::ItemInfo> items;
		if (!Current(progress)) {
			return;
		}
		const fs::path live = LiveOf(progress);
		const int slot = SlotOf(progress);
		const std::string hero = progress.stem().string();
		const Legacy::Estate* estate = g_legacy.CarriedBy(hero);
		if (estate == nullptr || !g_sharedWrite) {
			g_delivering = false;
			return;
		}
		if (!ItemsIn(context, D2RL::Items::ContainerBit(Container::PersonalStash), player, items)) {
			return;
		}
		if (items.empty()) {
			if (++g_deliverTries >= 20) {  // the page never came in: it is sent again
				Log(context, "Stash: %s carries none of %s's items; they will be sent again.", hero.c_str(), estate->name.c_str());
				g_legacy.CarriedBy(hero)->carrier.clear();
				Legacy::Save(live, slot, g_legacy);
				g_delivering = false;
			}
			return;
		}
		const int page = g_tabBase.load() + estate->tab;
		// They were packed to fit one page, so each goes to the same cell there; put wherever there is
		// room they fit worse, and one of 40 stayed behind (2026-11-25). The rest is for when the cell is
		// taken after all: elsewhere on the page, an open page, the bag.
		size_t moved = 0, onPage = 0;
		std::string stuck;
		for (const auto& item : items) {
			bool done = MoveToFilePage(context, player, item.handle, page, item.x, item.y) || MoveToFilePage(context, player, item.handle, page);
			onPage += done ? 1 : 0;
			for (int own = 0; own < g_state.Pages() && !done && g_deliverTries >= 2; ++own) {
				done = MoveTo(context, player, item.handle, own);
			}
			done = done || (g_deliverTries >= 2 && MoveToBag(context, player, item.handle));
			moved += done ? 1 : 0;
			if (!done) {
				stuck += " " + CodeText(item.code) + "@" + std::to_string(item.x) + "," + std::to_string(item.y);
			}
		}
		if (moved > 0 || g_deliverTries == 0) {
			Log(context, "Stash: %d of %d item(s) of %s moved, %d of them to their page.%s%s", static_cast<int>(moved), static_cast<int>(items.size()), estate->name.c_str(),
				static_cast<int>(onPage), stuck.empty() ? "" : " Left:", stuck.substr(0, 200).c_str());
		}
		if (onPage > 0) {  // the page can be turned to at once, even if something is still to come
			g_known.erase(estate->tab);
			g_legacy.tabs[estate->tab] = estate->name;
			Legacy::Save(live, slot, g_legacy);
			QueueRefresh(context);
		}
		if (moved == items.size()) {
			g_known.erase(estate->tab);
			const bool anyThere = g_legacy.tabs.count(estate->tab) != 0;
			const int tab = estate->tab;
			Legacy::Delivered(g_legacy, hero);
			if (!anyThere) {
				g_legacy.tabs.erase(tab);  // everything found room elsewhere
			}
			Legacy::Save(live, slot, g_legacy);
			g_delivering = false;
			context->WriteConsoleMessage("An ascended hero's belongings wait in the stash.");
			QueueRefresh(context);
		} else if (++g_deliverTries >= 60) {
			g_delivering = false;  // next game again
		}
	} catch (...) {
		context->LogWarn("Stash: bringing an ascended hero's items failed.");
	}
}

void StartDelivering(const D2RL::PluginContext* context) noexcept {
	if (g_delivering.exchange(true)) {
		return;
	}
	g_deliverTries = 0;
	try {
		std::thread([context] {
			while (g_delivering.load()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1000));
				if (g_delivering.load()) {
					g_threads->runOnGameThread(context, Deliver, nullptr);
				}
			}
		}).detach();
	} catch (...) {
		g_delivering = false;
	}
}

size_t g_stuck = 0;     // items the last sweep found no room for; game thread only
int    g_restSweeps = 0;

// The save's own page (from 0) of an item on the shared pages; -1 when it is another save's.
auto LocalPage(uint32_t page) noexcept -> int {
	const long long local = static_cast<long long>(page) - g_base.load();
	return local >= 0 && local < Stash::PageCount ? static_cast<int>(local) : -1;
}

// Which of the save's pages for ascended heroes (0-3) it is; -1: none of them.
auto TabOf(uint32_t page) noexcept -> int {
	const long long tab = static_cast<long long>(page) - g_tabBase.load();
	return tab >= 0 && tab < Legacy::TabCount ? static_cast<int>(tab) : -1;
}

// Game thread, locked. This save was made where another was deleted while the game held the stash:
// what still lies on its pages belongs to the deleted save (a copy is in camp/trash).
void WipePages(const D2RL::PluginContext* context, D2RL::PlayerHandle player, const std::vector<D2RL::Items::ItemInfo>& items) {
	int gone = 0, left = 0;
	for (const auto& item : items) {
		if (item.container == Container::SharedStash && (LocalPage(item.sharedStashPage) >= 0 || TabOf(item.sharedStashPage) >= 0)) {
			g_items->destroyItem(context, player, item.handle, D2RL::Items::SocketedItemPolicy::DestroyContents) == D2RL::Items::Result::Success ? ++gone : ++left;
		}
	}
	Log(context, "Stash: emptied the deleted save's pages (%d items, %d left).", gone, left);
	if (left == 0) {
		const fs::path live = g_file.parent_path().parent_path().parent_path();
		auto wipes = StashFile::Wipes(live);
		wipes.erase(g_base.load() / StashFile::PagesPerSave + 1);
		if (StashFile::SetWipes(live, wipes)) {
			g_wipe = false;
		}
	}
}

// Game thread, while the stash is open.
void __cdecl Sweep(const D2RL::PluginContext* context, void*) noexcept {
	try {
		const fs::path progress = Campfire::RouteProgressFile();
		const Lock lock(g_mutex);
		D2RL::PlayerHandle player = D2RL::InvalidPlayerHandle;
		std::vector<D2RL::Items::ItemInfo> items;
		if (!g_bankOpen.load() || !Current(progress) ||
			!ItemsIn(context, D2RL::Items::ContainerBit(Container::PersonalStash) | D2RL::Items::ContainerBit(Container::SharedStash), player, items)) {
			return;
		}
		if (g_wipe.load()) {
			WipePages(context, player, items);
			return;
		}
		GuardTabs(context, player, items, progress);
		const bool carrying = g_legacy.CarriedBy(progress.stem().string()) != nullptr;  // not ours to move (Deliver)
		std::set<int> used;
		for (const auto& item : items) {
			if (item.container == Container::SharedStash && LocalPage(item.sharedStashPage) >= 0) {
				used.insert(LocalPage(item.sharedStashPage));
			}
		}
		// The first look at a save's stash: pages that already hold items stay open until emptied.
		if (!g_state.legacyDone) {
			for (const int page : used) {
				if (page >= g_state.Pages()) {
					g_state.legacy.insert(page);
				}
			}
			g_state.legacyDone = true;
			Save(context);
			Log(context, "Stash: first look at this save; %d locked page(s) hold items and stay open until emptied.", static_cast<int>(g_state.legacy.size()));
		} else {
			const size_t before = g_state.legacy.size();
			std::erase_if(g_state.legacy, [&](int page) { return used.count(page) == 0 || page < g_state.Pages(); });
			if (g_state.legacy.size() != before) {
				Save(context);
			}
		}
		if (!g_sharedWrite) {
			return;
		}
		std::vector<const D2RL::Items::ItemInfo*> out;
		for (const auto& item : items) {
			if ((item.container == Container::PersonalStash && !carrying) ||
				(item.container == Container::SharedStash && LocalPage(item.sharedStashPage) >= 0 && !g_state.PageOpen(LocalPage(item.sharedStashPage)))) {
				out.push_back(&item);
			}
		}
		if (!out.empty() && out.size() == g_stuck && ++g_restSweeps < 22) {
			return;  // nothing changed since no room was found: try again a little later
		}
		g_restSweeps = 0;
		int moved = 0;
		for (const auto* item : out) {
			for (int page = 0; page < g_state.Pages(); ++page) {
				if (MoveTo(context, player, item->handle, page)) {
					++moved;
					break;
				}
			}
		}
		g_stuck = out.size() - static_cast<size_t>(moved);
		if (moved > 0) {
			Log(context, "Stash: moved %d item(s) to the open shared pages, %d left.", moved, static_cast<int>(g_stuck));
		}
	} catch (...) {
		context->LogWarn("Stash: checking the pages failed.");
	}
}

void StartSweeping(const D2RL::PluginContext* context) noexcept {
	const uintptr_t visit = g_visit.fetch_add(1) + 1;
	g_threads->runOnGameThread(context, Sweep, nullptr);
	try {
		std::thread([context, visit] {
			while (g_bankOpen.load() && g_visit.load() == visit) {
				std::this_thread::sleep_for(std::chrono::milliseconds(SweepMs));
				if (g_bankOpen.load() && g_visit.load() == visit) {
					g_threads->runOnGameThread(context, Sweep, nullptr);
				}
			}
		}).detach();
	} catch (...) {
	}
}

// ---- Points and gold -------------------------------------------------------------------------
void SyncActs(const D2RL::PluginContext* context) noexcept {
	try {
		const fs::path progress = Campfire::RouteProgressFile();
		GameState::Ready();  // finds the hero: nothing else may have looked yet (a game that starts outside Act I, 0.20.5)
		const int cls = Game::HeroInfo().classId;
		const Lock lock(g_mutex);
		if (progress.empty() || progress.stem() == "_unknown" || !Current(progress)) {
			return;
		}
		const int added = Stash::CountActs(g_state, cls, Route::Load(progress));
		if (added > 0) {
			Save(context);
			Log(context, "Stash: %d new caravan point(s), %d to spend.", added, g_state.Points());
			context->WriteConsoleMessage("An act is done: the caravan earns a passive point (Caravan button at the stash).");
			QueueRefresh(context);
		}
	} catch (...) {
		context->LogWarn("Stash: counting finished acts failed.");
	}
}

// UI thread, again and again until the gold has moved: only gold that really moved counts.
void __cdecl FinishPayment(const D2RL::PluginContext* context, void*) noexcept {
	try {
		const fs::path progress = Campfire::RouteProgressFile();
		const Lock lock(g_mutex);
		const Game::Unit hero = Game::Hero();
		const int gold = hero ? Game::Stat(hero, Game::StatId::Gold, 0, false).value_or(0) : g_payment.gold;
		const int bank = hero ? Game::Stat(hero, Game::StatId::StashGold, 0, false).value_or(0) : g_payment.bank;
		if (hero && (gold != g_payment.goldAfter || bank != g_payment.bankAfter) && GetTickCount64() - g_payment.started < PayLimitMs) {
			Later(context, PayPollMs, FinishPayment);
			return;
		}
		const long long taken = static_cast<long long>(g_payment.gold) - gold + g_payment.bank - bank;
		if (!Current(progress)) {
			g_payNote.clear();
		} else if (g_payment.kind == Moving::Take) {
			const long long back = std::clamp(g_payment.total + taken, 0LL, g_payment.total);  // what did not reach the hero
			g_state.purse += back;
			Save(context);
			Log(context, "Stash: %lld gold taken out, %lld in the purse.", g_payment.total - back, g_state.purse);
		} else if (g_payment.kind == Moving::Deposit) {
			if (taken > 0) {
				g_state.purse += (std::min)(taken, g_payment.total);
				Save(context);
			}
			Log(context, "Stash: %lld gold put in, %lld in the purse.", (std::max)(0LL, taken), g_state.purse);
		} else {
			const long long fromPurse = (std::min)(g_payment.purse, g_state.purse);
			const long long paid = std::clamp(taken, 0LL, g_payment.total - g_payment.purse) + fromPurse;
			if (paid > 0) {
				g_state.purse -= fromPurse;
				const bool unlocked = g_state.Pay(paid);
				Save(context);
				g_payNote = unlocked ? "Page unlocked" : "Paid " + Stash::Commas(paid) + " gold";
				Log(context, "Stash: %lld gold paid in%s; %d page(s), %lld toward the next.", paid, unlocked ? " and a page unlocked" : "", g_state.Pages(), g_state.fund);
			} else {
				Log(context, "Stash: the gold did not change (%d and %d); nothing was paid.", gold, bank);
				g_payNote = "Gold did not change, not paid";
			}
		}
	} catch (...) {
		context->LogWarn("Stash: the payment failed.");
	}
	g_paying = false;
	QueueRefresh(context);
}

// The hero to pay from; not found: says so and returns nothing.
auto PayingHero(const D2RL::PluginContext* context) -> Game::HeroDetails {
	GameState::Ready();
	const Game::HeroDetails hero = Game::HeroInfo();
	if (!hero.unit) {
		g_payNote = "Hero not found yet, try again";
		Log(context, "Stash: the hero can't be read (%s).", GameState::Status().c_str());
	}
	return hero;
}

void StartPayment(const D2RL::PluginContext* context, Payment pay) {
	pay.started = GetTickCount64();
	g_payment = pay;
	char command[64] {};
	if (pay.bankAfter != pay.bank) {
		std::snprintf(command, sizeof(command), "cheat stat goldbank,%d", pay.bankAfter);
		RunConsole(context, command);
	}
	if (pay.goldAfter != pay.gold) {
		std::snprintf(command, sizeof(command), "cheat stat gold,%d", pay.goldAfter);
		RunConsole(context, command);
	}
	Later(context, pay.bankAfter == pay.bank && pay.goldAfter == pay.gold ? 0 : PayPollMs, FinishPayment);
}

// UI thread. Pays up to `wanted` gold (negative: all) toward the next page: what the hero carries,
// then what lies in its old stash purse, then the save's purse.
void Pay(const D2RL::PluginContext* context, long long wanted) {
	const Game::HeroDetails hero = PayingHero(context);
	const long long owing = g_state.Owing();
	if (!hero.unit || owing <= 0 || g_paying.exchange(true)) {
		return;
	}
	Payment pay;
	pay.gold = (std::max)(0, hero.gold);
	pay.bank = (std::max)(0, hero.stashGold);
	pay.total = (std::min)({ owing, wanted < 0 ? owing : wanted, pay.gold + pay.bank + g_state.purse });
	if (pay.total <= 0) {
		g_paying = false;
		g_payNote = "No gold to pay with";
		Log(context, "Stash: nothing paid, there is no gold.");
		return;
	}
	const int fromGold = static_cast<int>((std::min)(pay.total, static_cast<long long>(pay.gold)));
	const int fromBank = static_cast<int>((std::min)(pay.total - fromGold, static_cast<long long>(pay.bank)));
	pay.purse = pay.total - fromGold - fromBank;
	pay.goldAfter = pay.gold - fromGold;
	pay.bankAfter = pay.bank - fromBank;
	Log(context, "Stash: paying %lld of %lld (carried %d, old purse %d, stash %lld).", pay.total, owing, fromGold, fromBank, pay.purse);
	StartPayment(context, pay);
}

// UI thread. `wanted` gold (0: all) of the hero's into the save's purse, or out of it as far as the
// hero may carry it.
void MoveGold(const D2RL::PluginContext* context, Moving kind, long long wanted) {
	if (wanted <= 0) {
		wanted = (std::numeric_limits<long long>::max)();
	}
	const Game::HeroDetails hero = PayingHero(context);
	if (!hero.unit || g_paying.exchange(true)) {
		return;
	}
	Payment pay;
	pay.kind = kind;
	pay.gold = (std::max)(0, hero.gold);
	pay.bank = (std::max)(0, hero.stashGold);
	pay.bankAfter = pay.bank;
	if (kind == Moving::Deposit) {
		pay.total = (std::min)(wanted, static_cast<long long>(pay.gold) + pay.bank);
		const int fromGold = static_cast<int>((std::min)(pay.total, static_cast<long long>(pay.gold)));
		pay.goldAfter = pay.gold - fromGold;
		pay.bankAfter = pay.bank - static_cast<int>(pay.total - fromGold);
	} else {
		pay.total = (std::min)({ wanted, g_state.purse, (std::max)(0LL, static_cast<long long>(hero.level) * 10000 - pay.gold) });
		pay.goldAfter = pay.gold + static_cast<int>((std::max)(0LL, pay.total));
	}
	if (pay.total <= 0) {
		g_paying = false;
		return;
	}
	if (kind == Moving::Take) {  // taken off first: if the game stops here, gold is lost rather than doubled
		g_state.purse -= pay.total;
		if (!Save(context)) {
			g_state.purse += pay.total;
			g_paying = false;
			return;
		}
	}
	StartPayment(context, pay);
}

// ---- Buttons ---------------------------------------------------------------------------------
auto StartsWith(const char* text, const char* prefix) noexcept -> bool {
	return std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

void LookUp(int rune) noexcept {
	if (g_journalRune.exchange(rune) != rune) {
		g_journalPage = 0;
	}
}

void OnAction(const D2RL::PluginContext* context, const char* action) {
	const fs::path progress = Campfire::RouteProgressFile();
	const Lock lock(g_mutex);
	if (!g_bankOpen.load() || !Current(progress)) {
		return;
	}
	if (std::strcmp(action, "tree") == 0) {
		g_payNote.clear();
		g_treeWanted = !RowOpen(context, g_tree);
		SyncActs(context);
	} else if (std::strcmp(action, "upgrades") == 0) {
		if (g_state.Has("tab")) {
			g_tabWanted = true;
		} else {
			g_treeWanted = true;  // not unlocked yet: show where it is
		}
	} else if (std::strcmp(action, "shared") == 0) {
		g_tabWanted = false;
	} else if (std::strcmp(action, "closebank") == 0) {
		g_tabWanted = false;
		Dispatch(context, "BankPanelMessage", "Close", "");
	} else if (std::strcmp(action, "deposit") == 0) {
		if (g_state.Has("quick")) {
			g_threads->runOnGameThread(context, StoreFromBag, reinterpret_cast<void*>(StoreAll));
		}
	} else if (std::strcmp(action, "goldin") == 0) {
		MoveGold(context, Moving::Deposit, Stash::GoldSteps[g_goldStep]);
	} else if (std::strcmp(action, "goldout") == 0) {
		MoveGold(context, Moving::Take, Stash::GoldSteps[g_goldStep]);
	} else if (std::strcmp(action, "goldless") == 0) {
		g_goldStep -= g_goldStep > 0 ? 1 : 0;
	} else if (std::strcmp(action, "goldmore") == 0) {
		g_goldStep += g_goldStep + 1 < Stash::GoldSteps.size() ? 1 : 0;
	} else if (std::strcmp(action, "journal") == 0) {
		g_journalWanted = !g_journalWanted.load();
	} else if (StartsWith(action, "jrune-")) {
		LookUp(std::clamp(std::atoi(action + 6), 0, Journal::RuneCount - 1));
	} else if (StartsWith(action, "jpage-")) {
		g_journalPage = std::atoi(action + 6);
	} else if (StartsWith(action, "pay-")) {
		const long long amount = std::atoll(action + 4);
		Pay(context, amount > 0 ? amount : -1);
	} else if (StartsWith(action, "node-")) {
		if (g_state.BuyNode(action + 5) == Stash::Buy::Bought) {
			Save(context);
			Log(context, "Stash: node %s bought, %d point(s) left.", action + 5, g_state.Points());
		}
	} else if (StartsWith(action, "cut-")) {
		if (const int made = g_state.CutGems(std::atoi(action + 4)); made > 0) {
			Save(context);
			Log(context, "Stash: the gem cutter made %d gem(s).", made);
		}
	} else if (StartsWith(action, "slot-")) {
		// With the journal up, a rune's first click only looks it up; the next one takes it out.
		size_t storeOnly = 0;
		if (const int rune = Journal::RuneIndex(action + 5); rune >= 0 && g_journalWanted.load() && g_journalRune.load() != rune) {
			LookUp(rune);
			storeOnly = StoreOnly;
		}
		const auto& kinds = Stash::Kinds();
		for (size_t i = 0; i < kinds.size(); ++i) {
			if (kinds[i].code == action + 5) {
				g_threads->runOnGameThread(context, ClickSlot, reinterpret_cast<void*>(i | storeOnly));
			}
		}
	}
	QueueRefresh(context);
}

// The number a page of the stash file has for the player: 1-10, or 11-14 for an ascended hero's
// page in use; 0: not for this save.
auto ShownPage(uint32_t page) -> int {
	if (const int local = LocalPage(page); local >= 0) {
		return local + 1;
	}
	const Lock lock(g_mutex);
	const int tab = TabOf(page);
	return tab >= 0 && g_legacy.tabs.count(tab) != 0 ? Stash::PageCount + 1 + tab : 0;
}

auto FilePage(int shown) noexcept -> int {
	return shown <= Stash::PageCount ? g_base.load() + shown - 1 : g_tabBase.load() + shown - Stash::PageCount - 1;
}

// UI thread. Turns the stash to a page by as many presses of the arrow as it takes.
void GoTo(const D2RL::PluginContext* context, int shown) noexcept {
	const int steps = FilePage(shown) - FilePage(g_page.load());
	const D2RL::Widgets::UiAction action { .structSize = D2RL::Widgets::UiActionSize, .target = "BankPanelMessage",
		.command = steps > 0 ? "SharedStashRight" : "SharedStashLeft", .text = "" };
	g_jumping = true;
	for (int i = 0; i < std::abs(steps) && g_widgets != nullptr; ++i) {
		g_widgets->dispatchUiAction(context, &action);
	}
	g_jumping = false;
	g_page = shown;
	QueueRefresh(context);
}

// UI thread. Shows the save's first page. The game can't be asked which page shows, so it goes all
// the way back and then forward to the save's own pages.
void __cdecl HomePage(const D2RL::PluginContext* context, void*) noexcept {
	if (!g_bankOpen.load() || g_widgets == nullptr) {
		return;
	}
	const auto send = [&](const char* command, int times) {
		const D2RL::Widgets::UiAction action { .structSize = D2RL::Widgets::UiActionSize, .target = "BankPanelMessage", .command = command, .text = "" };
		for (int i = 0; i < times; ++i) {
			g_widgets->dispatchUiAction(context, &action);
		}
	};
	g_jumping = true;
	send("SharedStashLeft", StashFile::FilePages - 1);
	send("SharedStashRight", g_base.load());
	g_jumping = false;
	g_page = 1;
	QueueRefresh(context);
}

// UI thread. Ctrl+click on a bag item while the Upgrades tab is up stores it. A click on a shared
// page tells which page is really showing. A rune clicked in either is looked up in the journal;
// Alt+click does only that (and opens the journal).
auto __cdecl OnItemClick(const D2RL::PluginContext* context, const D2RL::ItemInteractions::ItemInteractionEvent* event, void*) noexcept
	-> D2RL::ItemInteractions::Decision {
	using Decision = D2RL::ItemInteractions::Decision;
	try {
		if (event == nullptr || !g_bankOpen.load()) {
			return Decision::Continue;
		}
		D2RL::Items::ItemInfo info { .structSize = static_cast<uint32_t>(sizeof(D2RL::Items::ItemInfo)) };
		if (g_items->getItemInfo(context, event->item, &info) != D2RL::Items::Result::Success) {
			return Decision::Continue;
		}
		if (const int rune = Journal::RuneIndex(CodeText(info.code)); rune >= 0) {
			const bool alt = (event->modifiers & D2RL::ItemInteractions::ModifierBit(D2RL::ItemInteractions::Modifier::Alt)) != 0;
			if (alt) {
				g_journalWanted = true;
			}
			if (g_journalWanted.load()) {
				LookUp(rune);
				QueueRefresh(context);
			}
			if (alt) {
				return Decision::Consume;
			}
		}
		if (event->container == Container::SharedStash) {
			const int shown = ShownPage(info.sharedStashPage);
			if (shown == 0 || g_wipe.load()) {  // another save's page shows: never let it be taken, go back to our own
				Log(context, "Stash: page %u of the file is not this save's; going back.", info.sharedStashPage + 1);
				g_threads->runOnUiThread(context, HomePage, nullptr);
				return Decision::Consume;
			}
			if (g_page.exchange(shown) != shown) {
				QueueRefresh(context);
			}
			return Decision::Continue;
		}
		if (event->container == Container::Inventory && (event->modifiers & D2RL::ItemInteractions::ModifierBit(D2RL::ItemInteractions::Modifier::Control)) != 0 &&
			RowOpen(context, g_tab)) {
			if (Stash::KindOf(CodeText(info.code)) != nullptr && info.runtimeId != 0) {
				g_threads->runOnGameThread(context, StoreFromBag, reinterpret_cast<void*>(static_cast<uintptr_t>(info.runtimeId)));
			}
			return Decision::Consume;  // never into the page hidden behind the tab
		}
	} catch (...) {
	}
	return Decision::Continue;
}

// ---- Opening on the shared pages --------------------------------------------------------------
// The game opens the stash on the hero's own (hidden) page, and its message for choosing a tab
// carries a number that plugins can't send. So where the player clicks the Shared tab is
// remembered, and when the stash opens that spot is clicked for the player and the mouse put back.
// Another window size, or a click that chose no tab, and the spot is forgotten until the next click.
std::atomic<bool> g_clicking { false }, g_tabChosen { false };
std::atomic<int>  g_spotX { -1 }, g_spotY { -1 }, g_spotW { 0 }, g_spotH { 0 };

auto GameWindow(RECT& area) noexcept -> HWND {
	const HWND window = GetForegroundWindow();
	DWORD pid = 0;
	GetWindowThreadProcessId(window, &pid);
	return window != nullptr && pid == GetCurrentProcessId() && GetClientRect(window, &area) ? window : nullptr;
}

auto SpotFile() -> fs::path {
	return g_file.empty() ? fs::path() : g_file.parent_path().parent_path() / "stash-tab.txt";  // camp/
}

// UI thread: the player chose the Shared tab with the mouse.
void NoteTabClick() noexcept {
	g_tabChosen = true;
	RECT area {};
	POINT at {};
	const HWND window = GameWindow(area);
	if (g_clicking.load() || window == nullptr || !GetCursorPos(&at) || !ScreenToClient(window, &at) || !PtInRect(&area, at)) {
		return;
	}
	try {
		const Lock lock(g_mutex);
		g_spotX = at.x, g_spotY = at.y, g_spotW = area.right, g_spotH = area.bottom;
		if (const fs::path file = SpotFile(); !file.empty()) {
			Camp::WriteFile(file, std::to_string(area.right) + " " + std::to_string(area.bottom) + " " + std::to_string(at.x) + " " + std::to_string(at.y) + "\n");
		}
	} catch (...) {
	}
}

void ClickShared(const D2RL::PluginContext* context) noexcept {
	try {
		const Lock lock(g_mutex);
		if (g_spotX.load() < 0) {
			std::istringstream in(Camp::ReadFile(SpotFile()));
			int w = 0, h = 0, x = -1, y = -1;
			if (in >> w >> h >> x >> y) {
				g_spotX = x, g_spotY = y, g_spotW = w, g_spotH = h;
			}
		}
	} catch (...) {
	}
	if (g_spotX.load() < 0 || g_clicking.exchange(true)) {
		return;
	}
	g_tabChosen = false;
	try {
		std::thread([context, visit = g_visit.load()] {
			const auto wait = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
			const auto mine = [&] { return g_bankOpen.load() && g_visit.load() == visit; };
			wait(120);  // the window is up
			for (int i = 0; i < 100 && mine() && ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000) != 0; ++i) {
				wait(20);  // the player still holds the mouse
			}
			RECT area {};
			POINT before {}, spot { g_spotX.load(), g_spotY.load() };
			const HWND window = GameWindow(area);
			if (mine() && !g_tabChosen.load() && window != nullptr && area.right == g_spotW.load() && area.bottom == g_spotH.load() && GetCursorPos(&before) &&
				ClientToScreen(window, &spot)) {
				const auto button = [](DWORD flags) {
					INPUT input {};
					input.type = INPUT_MOUSE;
					input.mi.dwFlags = flags;
					SendInput(1, &input, sizeof(input));
				};
				SetCursorPos(spot.x, spot.y);
				wait(40);
				if (mine()) {
					button(MOUSEEVENTF_LEFTDOWN);
					wait(30);
					button(MOUSEEVENTF_LEFTUP);
					wait(40);
				}
				SetCursorPos(before.x, before.y);
				wait(250);
				if (mine() && !g_tabChosen.load()) {
					g_spotX = -1;
					context->LogInfo("Stash: the Shared tab was not where it was before; the next click on it is remembered.");
					try {
						const Lock lock(g_mutex);
						std::error_code ec;
						fs::remove(SpotFile(), ec);
					} catch (...) {
					}
				}
			}
			g_clicking = false;
		}).detach();
	} catch (...) {
		g_clicking = false;
	}
}

// UI thread. The shown page went away: back to the last open one.
void __cdecl LeaveTab(const D2RL::PluginContext* context, void*) noexcept {
	const Lock lock(g_mutex);
	if (g_bankOpen.load()) {
		GoTo(context, (std::max)(1, g_state.LastPage()));
	}
}

// UI thread, locked. An arrow was pressed. True: the game must not see it (nowhere to go, or the
// plugin turned several pages itself, past the locked ones).
auto TurnPage(const D2RL::PluginContext* context, int direction) -> bool {
	const int last = g_state.legacyDone ? g_state.LastPage() : Stash::PageCount;
	const int presses = Legacy::Presses(g_page.load(), direction, last, Stash::PageCount + 1, g_legacy);
	if (presses == 0) {
		Log(context, "Stash: no page %s of page %d.", direction > 0 ? "right" : "left", g_page.load());
		return true;
	}
	const int target = g_page.load() + direction * presses;
	if (FilePage(target) - FilePage(g_page.load()) != direction) {
		GoTo(context, target);
		return true;
	}
	g_page = target;
	QueueRefresh(context);
	return false;
}

void BankClosed(const D2RL::PluginContext* context) noexcept {
	if (g_bankOpen.exchange(false)) {
		g_treeWanted = false;
		g_tabWanted = false;
		QueueRefresh(context);
	}
}

// "dd-stash": what this save has. Test tools: "dd-stash points <n>", "dd-stash gold <n>", "dd-stash autotab".
auto StashCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	try {
		const fs::path progress = Campfire::RouteProgressFile();
		const Lock lock(g_mutex);
		const auto* context = command->plugin;
		if (!Current(progress)) {
			context->WriteConsoleMessage("Stash: no save loaded.");
			return D2RL::ConsoleCommandResult::Failed;
		}
		std::istringstream in(command->args != nullptr ? command->args : "");
		std::string what;
		long long n = 0;
		in >> what >> n;
		if (what == "points") {  // marks acts as finished, class by class
			for (int cls = 0; cls < Camp::ClassCount && n > 0; ++cls) {
				for (int act = 1; act <= 5 && n > 0; ++act) {
					n -= g_state.acts.insert({ cls, act }).second ? 1 : 0;
				}
			}
			Save(context);
			QueueRefresh(context);
		} else if (what == "gold") {
			while (n > 0 && g_state.Owing() > 0) {
				const long long part = (std::min)(n, g_state.Owing());
				g_state.Pay(part);
				n -= part;
			}
			Save(context);
			QueueRefresh(context);
		} else if (what == "autotab") {
			g_autoTab = !g_autoTab.load();
			context->WriteConsoleMessage(g_autoTab.load() ? "Stash: tries to open on the shared pages." : "Stash: opens as the game does.");
			return D2RL::ConsoleCommandResult::Handled;
		}
		char message[256] {};
		std::snprintf(message, sizeof(message), "Stash: %d of %d pages (%lld of %lld gold toward the next), %d of %d points to spend, %d kind(s) stored, %d old page(s) still open.",
			g_state.Pages(), Stash::PageCount, g_state.fund, g_state.Price(), g_state.Points(), g_state.Earned(), static_cast<int>(g_state.mats.size()),
			static_cast<int>(g_state.legacy.size()));
		context->WriteConsoleMessage(message);
		return D2RL::ConsoleCommandResult::Handled;
	} catch (...) {
		return D2RL::ConsoleCommandResult::Failed;
	}
}

void RegisterRow(const D2RL::PluginContext* context, Row& row, D2RL::Panels::PanelFlags flags) {
	for (int i = 0; i < row.count; ++i) {
		const std::string id = std::string(row.base) + std::to_string(i);
		const D2RL::Panels::PanelRegistration registration {
			.structSize = D2RL::Panels::PanelRegistrationSize,
			.flags      = flags,
			.localId    = id.c_str(),
		};
		D2RL::Panels::RegistrationHandle panel = D2RL::Panels::InvalidHandle;
		if (g_panels->registerPanel(context, &registration, &panel) != D2RL::Panels::Result::Success) {
			Log(context, "Stash: could not register the %s panel.", id.c_str());
			break;
		}
		row.panels.push_back(panel);
		row.layouts.push_back(D2RL::Resources::InvalidHandle);
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
			g_panels = nullptr;
			context->LogWarn("Stash: a service is missing; no caravan upgrades, and every stash page is open.");
			return true;
		}
		if (context->QueryService(&g_widgets) != D2RL::ServiceQueryResult::Success || !D2RL::HasWidgetServiceField(g_widgets, D2RL::WidgetServiceRequiredSize)) {
			g_widgets = nullptr;
		}
		g_sharedWrite = D2RL::HasItemServiceCapability(g_items, D2RL::Items::ItemServiceCapability::SharedStashWrite);
		RegisterRow(context, g_tree, D2RL::Panels::PanelFlags::CloseOnEscape);
		RegisterRow(context, g_wall, D2RL::Panels::PanelFlags::CloseOnEscape);
		RegisterRow(context, g_tab, D2RL::Panels::PanelFlags::CloseOnEscape);
		RegisterRow(context, g_pageRow, D2RL::Panels::PanelFlags::None);
		RegisterRow(context, g_name, D2RL::Panels::PanelFlags::None);
		RegisterRow(context, g_journal, D2RL::Panels::PanelFlags::None);  // Escape is for the stash
		const D2RL::Panels::PanelRegistration side {
			.structSize = D2RL::Panels::PanelRegistrationSize,
			.flags      = D2RL::Panels::PanelFlags::None,
			.localId    = "StashSide",
		};
		D2RL::Resources::RegistrationHandle sideLayout = D2RL::Resources::InvalidHandle;
		if (!RegisterLayout(context, "darkestdiablo/StashSide", Stash::SideLayout(), sideLayout) ||
			g_panels->registerPanel(context, &side, &g_side) != D2RL::Panels::Result::Success) {
			g_side = D2RL::Panels::InvalidHandle;
			context->LogWarn("Stash: could not register the Caravan button.");
		}
		const D2RL::ItemInteractionService* clicks = nullptr;
		if (context->QueryService(&clicks) == D2RL::ServiceQueryResult::Success &&
			D2RL::HasItemInteractionServiceField(clicks, static_cast<uint32_t>(offsetof(D2RL::ItemInteractionService, supportedContainerMask) + sizeof(uint32_t)))) {
			const D2RL::ItemInteractions::ItemInteractionListener listener {
				.structSize    = D2RL::ItemInteractions::ItemInteractionListenerSize,
				.callback      = OnItemClick,
				.containerMask = (D2RL::Items::ContainerBit(Container::Inventory) | D2RL::Items::ContainerBit(Container::SharedStash)) & clicks->supportedContainerMask,
			};
			D2RL::ItemInteractions::ListenerHandle handle = D2RL::ItemInteractions::InvalidHandle;
			if (clicks->registerListener(context, &listener, &handle) != D2RL::ItemInteractions::Result::Success) {
				context->LogWarn("Stash: could not listen for item clicks; Ctrl+click won't store.");
			}
		}
		Log(context, "Stash: ready (%d + %d + %d panels, moving items between pages %s).", static_cast<int>(g_tree.panels.size()), static_cast<int>(g_tab.panels.size()), static_cast<int>(g_journal.panels.size()),
			g_sharedWrite ? "works" : "is not offered");
		return context->RegisterConsoleCommand("dd-stash", StashCommand, "Show the caravan stash. Tests: dd-stash points <n>, gold <n>, autotab.");
	} catch (...) {
		g_panels = nullptr;
		context->LogWarn("Stash: setting up failed; no caravan upgrades.");
		return true;
	}
}

auto OnUiMessage(const D2RL::PluginContext* context, const char* target, const char* command, const char* text) noexcept -> bool {
	if (g_panels == nullptr) {
		return false;
	}
	try {
		if (std::strcmp(target, "PanelManager") == 0) {
			if (std::strcmp(command, "OpenPanel") == 0 && StartsWith(text, ActionPrefix)) {
				OnAction(context, text + sizeof(ActionPrefix) - 1);
				return true;
			}
			// Closed with Escape: it stays closed until asked for again. (Replacing a panel closes the
			// old one, which is no longer the row's current panel by then.)
			if (std::strcmp(command, "ClosePanel") == 0 && StartsWith(text, "darkestdiablo/Stash")) {
				const Lock lock(g_mutex);
				if (g_tree.current >= 0 && RowName(g_tree, g_tree.current) == text) {
					if (g_treeWanted.exchange(false)) {
						QueueRefresh(context);
					}
				} else if (g_wall.current >= 0 && RowName(g_wall, g_wall.current) == text) {
					if (g_treeWanted.exchange(false)) {
						QueueRefresh(context);
					}
				} else if (g_tab.current >= 0 && RowName(g_tab, g_tab.current) == text) {
					if (g_tabWanted.exchange(false)) {
						QueueRefresh(context);
					}
				}
				return false;
			}
			if (std::strcmp(text, BankPanel) == 0) {
				if (std::strcmp(command, "OpenPanel") == 0) {
					g_page = 1;
					g_treeWanted = false;
					g_tabWanted = false;
					if (!g_bankOpen.exchange(true)) {
						{
							const Lock lock(g_mutex);
							g_known.clear();
						}
						g_threads->runOnGameThread(context, Sweep, nullptr);
						StartSweeping(context);
					}
					SyncActs(context);
					Later(context, 150, Refresh);
					if (g_autoTab.load()) {
						ClickShared(context);
					}
				} else if (std::strcmp(command, "ClosePanel") == 0 || std::strcmp(command, "UnloadPanel") == 0) {
					BankClosed(context);
				}
			}
		} else if (g_bankOpen.load() && std::strcmp(target, "PlayerInventoryPanelMessage") == 0 && std::strcmp(command, "DropGold") == 0) {
			OnAction(context, "goldin");  // the bag's gold button at the stash: into the save's purse
			return true;
		} else if (std::strcmp(target, "BankPanelMessage") == 0) {
			if (std::strcmp(command, "Close") == 0) {
				BankClosed(context);
			} else if (g_jumping.load()) {
				return false;
			} else if (std::strcmp(command, "SelectTab") == 0) {
				NoteTabClick();
				Later(context, 40, HomePage);
			} else if (std::strcmp(command, "Withdraw") == 0) {
				return true;  // the game's purse is one for all saves; gold is in the save's own
			} else if (std::strcmp(command, "SharedStashLeft") == 0) {
				const fs::path progress = Campfire::RouteProgressFile();
				const Lock lock(g_mutex);
				return !Current(progress) || TurnPage(context, -1);
			} else if (std::strcmp(command, "SharedStashRight") == 0) {
				const fs::path progress = Campfire::RouteProgressFile();
				const Lock lock(g_mutex);
				return !Current(progress) || TurnPage(context, 1);
			}
		}
	} catch (...) {
		context->LogWarn("Stash: a message failed.");
	}
	return false;
}

void OnGameplayEvent(const D2RL::PluginContext* context, const D2RL::Lifecycle::GameplayEvent* event) noexcept {
	using Kind = D2RL::Lifecycle::GameplayEventKind;
	if (g_panels == nullptr) {
		return;
	}
	if (event->kind == Kind::LevelChanged) {
		if (event->previousValue <= 0) {
			StartDelivering(context);  // a hero just came in
		}
		SyncActs(context);
		const int level = event->currentValue;
		if (event->previousValue > 0 && (level == 1 || level == 40 || level == 75 || level == 103 || level == 109)) {
			g_threads->runOnGameThread(context, StoreFromBag, reinterpret_cast<void*>(StorePorter));
		}
	} else if (event->kind == Kind::GameLeft) {
		BankClosed(context);
		g_delivering = false;
		const Lock lock(g_mutex);
		g_known.clear();
		g_file.clear();  // read again in the next game: the save may have been deleted or switched
	}
}

void OnStopCleared(const D2RL::PluginContext* context) noexcept {
	if (g_panels != nullptr) {
		SyncActs(context);
	}
}

auto Nodes() -> std::set<std::string> {
	const fs::path progress = Campfire::RouteProgressFile();
	const Lock lock(g_mutex);
	return Current(progress) ? g_state.nodes : std::set<std::string> {};
}

auto CanPutOnSharedPages() noexcept -> bool {
	return g_sharedWrite && !g_wipe.load();
}

auto TurningPages() noexcept -> bool {
	return g_jumping.load();
}

auto FirstSharedPage() noexcept -> uint32_t {
	return static_cast<uint32_t>(g_base.load());
}

}
