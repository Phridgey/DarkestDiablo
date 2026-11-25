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
#include "campfire.h"
#include "gameapi.h"
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
constexpr int  SweepMs        = 700;
constexpr int  PayCheckMs     = 700;

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

std::atomic<bool>      g_bankOpen { false };
std::atomic<uintptr_t> g_visit { 0 };       // counts stash openings, so an old sweeper stops
std::atomic<int>       g_page { 1 };        // the shared page we believe is showing (from 1)
std::atomic<bool>      g_autoTab { false }; // dd-stash autotab: try to open the stash on the shared pages
std::atomic<bool>      g_treeWanted { false }, g_tabWanted { false };
std::atomic<bool>      g_journalWanted { true };  // not minimized; stays as it is between visits
std::atomic<int>       g_journalRune { 0 }, g_journalPage { 0 };
std::atomic<bool>      g_refresh { false };
std::atomic<bool>      g_paying { false };

// A payment on its way: what the hero had, and what should be left.
struct Payment { long long total = 0; int gold = 0, bank = 0, goldAfter = 0, bankAfter = 0; };
Payment g_payment;

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
// A plugin may register 64 panels in all; the whole plugin uses 63 now.
Row g_tree { "StashTree", 3 };
Row g_tab  { "StashTab", 4 };
Row g_journal { "StashJournal", 3 };
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
			CloseRow(context, g_tree);
			CloseRow(context, g_tab);
			CloseRow(context, g_journal);
			if (IsOpen(context, g_side)) {
				g_panels->closePanel(context, g_side);
			}
			return;
		}
		if (g_side != D2RL::Panels::InvalidHandle && !IsOpen(context, g_side)) {
			g_panels->openPanel(context, g_side);
		}
		if (g_tabWanted.load() && g_state.Has("tab")) {
			ShowRow(context, g_tab, Stash::UpgradesLayout(g_state));
		} else {
			CloseRow(context, g_tab);
		}
		if (g_treeWanted.load()) {
			ShowRow(context, g_tree, Stash::TreeLayout(g_state));
		} else {
			CloseRow(context, g_tree);
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
// everything but potions (Porter).
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
			if (kind == nullptr || (what == StorePorter && kind->section == Stash::Section::Potions) ||
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
// one of the slot's kind is taken out, into the hand (the bag if the hand is full).
void __cdecl ClickSlot(const D2RL::PluginContext* context, void* data) noexcept {
	try {
		const fs::path progress = Campfire::RouteProgressFile();
		const Lock lock(g_mutex);
		const auto& kinds = Stash::Kinds();
		const size_t index = reinterpret_cast<size_t>(data);
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
		if (g_state.Count(kind.code) <= 0) {
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

// ---- Page locks ------------------------------------------------------------------------------
auto MoveTo(const D2RL::PluginContext* context, D2RL::PlayerHandle player, D2RL::ItemHandle item, int page) -> bool {
	D2RL::Items::ExistingItemOperation operation {};
	operation.structSize                        = D2RL::Items::ExistingItemOperationSize;
	operation.kind                              = D2RL::Items::ExistingItemOperationKind::Move;
	operation.item                              = item;
	operation.move.destination.structSize       = D2RL::Items::ItemDestinationSize;
	operation.move.destination.container        = Container::SharedStash;
	operation.move.destination.placement        = D2RL::Items::Placement::Automatic;
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

size_t g_stuck = 0;     // items the last sweep found no room for; game thread only
int    g_restSweeps = 0;

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
		std::set<int> used;
		for (const auto& item : items) {
			if (item.container == Container::SharedStash && item.sharedStashPage < static_cast<uint32_t>(Stash::PageCount)) {
				used.insert(static_cast<int>(item.sharedStashPage));
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
			if (item.container == Container::PersonalStash ||
				(item.container == Container::SharedStash && item.sharedStashPage < static_cast<uint32_t>(Stash::PageCount) &&
					!g_state.PageOpen(static_cast<int>(item.sharedStashPage)))) {
				out.push_back(&item);
			}
		}
		if (!out.empty() && out.size() == g_stuck && ++g_restSweeps < 8) {
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

// UI thread, a moment after the gold was taken: only gold that is really gone counts.
void __cdecl FinishPayment(const D2RL::PluginContext* context, void*) noexcept {
	try {
		const fs::path progress = Campfire::RouteProgressFile();
		const Lock lock(g_mutex);
		const Game::Unit hero = Game::Hero();
		const int gold = hero ? Game::Stat(hero, Game::StatId::Gold, 0, false).value_or(0) : g_payment.gold;
		const int bank = hero ? Game::Stat(hero, Game::StatId::StashGold, 0, false).value_or(0) : g_payment.bank;
		const long long taken = static_cast<long long>(g_payment.gold) - gold + g_payment.bank - bank;
		if (taken > 0 && Current(progress)) {
			const bool unlocked = g_state.Pay((std::min)(taken, g_payment.total));
			Save(context);
			Log(context, "Stash: %lld gold paid in%s; %d page(s), %lld toward the next.", taken, unlocked ? " and a page unlocked" : "", g_state.Pages(), g_state.fund);
		} else {
			Log(context, "Stash: the gold did not change (%d and %d); nothing was paid.", gold, bank);
			context->WriteConsoleMessage("Stash: the payment did not go through; no gold was counted.");
		}
	} catch (...) {
		context->LogWarn("Stash: the payment failed.");
	}
	g_paying = false;
	QueueRefresh(context);
}

// UI thread. Pays up to `wanted` gold (negative: all) toward the next page: first what lies in
// the hero's old stash purse, then what it carries.
void Pay(const D2RL::PluginContext* context, long long wanted) {
	const Game::HeroDetails hero = Game::HeroInfo();
	const long long owing = g_state.Owing();
	if (!hero.unit || owing <= 0 || g_paying.exchange(true)) {
		return;
	}
	Payment pay;
	pay.gold = (std::max)(0, hero.gold);
	pay.bank = (std::max)(0, hero.stashGold);
	pay.total = (std::min)({ owing, wanted < 0 ? owing : wanted, static_cast<long long>(pay.gold) + pay.bank });
	if (pay.total <= 0) {
		g_paying = false;
		context->WriteConsoleMessage("Stash: this hero carries no gold. Gold on the shared pages has to be withdrawn first.");
		return;
	}
	const int fromBank = static_cast<int>((std::min)(pay.total, static_cast<long long>(pay.bank)));
	const int fromGold = static_cast<int>(pay.total - fromBank);
	pay.bankAfter = pay.bank - fromBank;
	pay.goldAfter = pay.gold - fromGold;
	g_payment = pay;
	char command[64] {};
	if (fromBank > 0) {
		std::snprintf(command, sizeof(command), "cheat stat goldbank,%d", pay.bankAfter);
		RunConsole(context, command);
	}
	if (fromGold > 0) {
		std::snprintf(command, sizeof(command), "cheat stat gold,%d", pay.goldAfter);
		RunConsole(context, command);
	}
	Later(context, PayCheckMs, FinishPayment);
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
		if (const int rune = Journal::RuneIndex(action + 5); rune >= 0) {
			LookUp(rune);
		}
		const auto& kinds = Stash::Kinds();
		for (size_t i = 0; i < kinds.size(); ++i) {
			if (kinds[i].code == action + 5) {
				g_threads->runOnGameThread(context, ClickSlot, reinterpret_cast<void*>(i));
			}
		}
	}
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
		if (event->container == Container::SharedStash && info.sharedStashPage < static_cast<uint32_t>(Stash::PageCount)) {
			g_page = static_cast<int>(info.sharedStashPage) + 1;
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

void __cdecl SelectShared(const D2RL::PluginContext* context, void*) noexcept {
	if (g_bankOpen.load()) {
		Dispatch(context, "BankPanelMessage", "SelectTab", "1");
	}
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
		RegisterRow(context, g_tab, D2RL::Panels::PanelFlags::CloseOnEscape);
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
					g_treeWanted = false;
				} else if (g_tab.current >= 0 && RowName(g_tab, g_tab.current) == text) {
					g_tabWanted = false;
				}
				return false;
			}
			if (std::strcmp(text, BankPanel) == 0) {
				if (std::strcmp(command, "OpenPanel") == 0) {
					g_page = 1;
					g_treeWanted = false;
					g_tabWanted = false;
					if (!g_bankOpen.exchange(true)) {
						StartSweeping(context);
					}
					SyncActs(context);
					Later(context, 150, Refresh);
					if (g_autoTab.load()) {
						Later(context, 100, SelectShared);
					}
				} else if (std::strcmp(command, "ClosePanel") == 0 || std::strcmp(command, "UnloadPanel") == 0) {
					BankClosed(context);
				}
			}
		} else if (std::strcmp(target, "BankPanelMessage") == 0) {
			if (std::strcmp(command, "Close") == 0) {
				BankClosed(context);
			} else if (std::strcmp(command, "SelectTab") == 0) {
				g_page = 1;
			} else if (std::strcmp(command, "SharedStashLeft") == 0) {
				g_page = (std::max)(1, g_page.load() - 1);
			} else if (std::strcmp(command, "SharedStashRight") == 0) {
				const fs::path progress = Campfire::RouteProgressFile();
				const Lock lock(g_mutex);
				if (Current(progress) && g_state.legacyDone && g_page.load() >= g_state.LastPage()) {
					Log(context, "Stash: page %d is still locked.", g_page.load() + 1);
					return true;
				}
				g_page = (std::min)(Stash::PageCount, g_page.load() + 1);
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
		SyncActs(context);
		const int level = event->currentValue;
		if (event->previousValue > 0 && (level == 1 || level == 40 || level == 75 || level == 103 || level == 109)) {
			g_threads->runOnGameThread(context, StoreFromBag, reinterpret_cast<void*>(StorePorter));
		}
	} else if (event->kind == Kind::GameLeft) {
		BankClosed(context);
		const Lock lock(g_mutex);
		g_file.clear();  // read again in the next game: the save may have been deleted or switched
	}
}

void OnStopCleared(const D2RL::PluginContext* context) noexcept {
	if (g_panels != nullptr) {
		SyncActs(context);
	}
}

auto CanPutOnSharedPages() noexcept -> bool {
	return g_sharedWrite;
}

}
