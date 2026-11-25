// Darkest Diablo — the caravan's stash: what the save has unlocked and what lies in the Upgrades
// tab (no game or Windows code here, so it can be tested anywhere; see tests/camp_tests.cpp).
// Plan and play rules: docs/stash.md.
//
// - Pages: a save starts with 1 shared stash page. 9 tiers, bought in order with gold, add one
//   each. Gold is paid in as it comes, by any hero of the save, and counts toward the next tier.
// - Points: the first time a class finishes an act in this save, the save earns 1 point (8 classes
//   x 5 acts = 40 at most). Points buy nodes of the passive tree. A fallen hero's points stay.
// - Upgrades tab: gems, runes and potions kept as counts, one slot per kind. Nodes decide which
//   kinds may be put in and how many of each. What is in a slot can always be taken out again.
// On disk: camp/slot<N>/stash.txt, one fact per line:
//   "tier <n>", "fund <gold>", "act <Class> <1-5>", "node <id>", "mat <code> <count>",
//   "legacy <page>" (a locked page that held items when this was added), "legacydone 1".
#pragma once

#include "camp.h"
#include "route.h"

#include <array>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Stash {

namespace fs = std::filesystem;

// ---- Pages --------------------------------------------------------------------------------
inline constexpr int TierCount = 9;
inline constexpr int PageCount = TierCount + 1;  // the game's 3 shared pages + add_shared_tabs 7
inline constexpr std::array<long long, TierCount> TierPrice = {
	50000, 100000, 175000, 300000, 500000, 750000, 1000000, 1500000, 2500000,
};

// ---- Passive tree -------------------------------------------------------------------------
struct Node {
	const char* id;
	const char* parent;  // must be owned first ("" for the first node)
	int         cost;
	float       column;  // where it stands in the tree: 0-5 across, 0-5 down
	int         row;
	const char* label;
	const char* text;    // the tooltip
};

inline constexpr Node Nodes[] = {
	{ "tab",      "",         1, 2.5f, 0, "Upgrades Tab",  "Opens the Upgrades tab in the stash." },
	{ "gem1",     "tab",      1, 0.5f, 1, "Gem Pouch",     "Keeps Chipped and Flawed gems." },
	{ "gem2",     "gem1",     1, 0.0f, 2, "Cut Gems",      "Keeps normal and Flawless gems." },
	{ "gem3",     "gem2",     2, 0.0f, 3, "Perfect Gems",  "Keeps Perfect gems." },
	{ "gemcut",   "gem3",     2, 0.0f, 4, "Gem Cutter",    "Adds buttons that turn every 3 equal gems of a row into 1 of the next quality." },
	{ "gemskull", "gem1",     1, 1.0f, 2, "Skull Shelf",   "Keeps skulls, of every gem quality unlocked." },
	{ "gemcap",   "gemskull", 1, 1.0f, 3, "Deep Pouch",    "Each gem slot holds 99 instead of 20." },
	{ "rune1",    "tab",      1, 2.5f, 1, "Rune Rack",     "Keeps runes El to Ral." },
	{ "rune2",    "rune1",    1, 2.0f, 2, "Rune Rack II",  "Keeps runes Ort to Shael." },
	{ "rune3",    "rune2",    2, 2.0f, 3, "Rune Rack III", "Keeps runes Dol to Lem." },
	{ "rune4",    "rune3",    2, 2.0f, 4, "Rune Rack IV",  "Keeps runes Pul to Ohm." },
	{ "rune5",    "rune4",    3, 2.0f, 5, "Rune Rack V",   "Keeps runes Lo to Zod." },
	{ "runecap",  "rune1",    1, 3.0f, 2, "Deep Rack",     "Each rune slot holds 99 instead of 20." },
	{ "pot1",     "tab",      1, 4.0f, 1, "Mana Shelf",    "Keeps mana potions." },
	{ "pot2",     "pot1",     1, 4.0f, 2, "Healing Shelf", "Keeps healing potions." },
	{ "pot3",     "pot2",     2, 4.0f, 3, "Rejuvenation",  "Keeps Rejuvenation and Full Rejuvenation potions." },
	{ "potcap",   "pot2",     1, 5.0f, 3, "Deep Shelf",    "Each potion slot holds 99 instead of 20." },
	{ "quick",    "tab",      1, 5.0f, 1, "Quick Stash",   "Adds a button that stores everything storable in the bag." },
	{ "porter",   "quick",    3, 5.0f, 2, "Porter",        "Coming back to town stores the bag's gems and runes by itself." },
};

inline auto FindNode(const std::string& id) -> const Node* {
	for (const auto& node : Nodes) {
		if (id == node.id) {
			return &node;
		}
	}
	return nullptr;
}

// ---- What the tab keeps -------------------------------------------------------------------
enum class Section : int { Gems = 0, Runes = 1, Potions = 2 };
inline constexpr int SmallCap = 20;
inline constexpr int LargeCap = 99;

struct Kind {
	std::string code;
	std::string name;
	std::string picture;  // under items/misc
	Section     section = Section::Gems;
	int         column = 0, row = 0;  // its place in the section
	std::vector<const char*> needs;   // every node it needs
};

inline auto Kinds() -> const std::vector<Kind>& {
	static const std::vector<Kind> all = [] {
		std::vector<Kind> out;
		struct GemType { const char* codes[5]; const char* picture; const char* name; };
		static constexpr GemType Gems[] = {
			{ { "gcv", "gfv", "gsv", "gzv", "gpv" }, "amethyst", "Amethyst" },
			{ { "gcy", "gfy", "gsy", "gly", "gpy" }, "topaz", "Topaz" },
			{ { "gcb", "gfb", "gsb", "glb", "gpb" }, "saphire", "Sapphire" },  // the game's own spelling
			{ { "gcg", "gfg", "gsg", "glg", "gpg" }, "emerald", "Emerald" },
			{ { "gcr", "gfr", "gsr", "glr", "gpr" }, "ruby", "Ruby" },
			{ { "gcw", "gfw", "gsw", "glw", "gpw" }, "diamond", "Diamond" },
			{ { "skc", "skf", "sku", "skl", "skz" }, "skull", "Skull" },
		};
		static constexpr const char* Grades[]    = { "Chipped ", "Flawed ", "", "Flawless ", "Perfect " };
		static constexpr const char* Pictures[]  = { "chipped_", "flawed_", "", "flawless_", "perfect_" };
		static constexpr const char* GradeNode[] = { "gem1", "gem1", "gem2", "gem2", "gem3" };
		for (int type = 0; type < 7; ++type) {
			for (int grade = 0; grade < 5; ++grade) {
				Kind kind { Gems[type].codes[grade], std::string(Grades[grade]) + Gems[type].name,
					std::string("gem/") + Pictures[grade] + Gems[type].picture, Section::Gems, type, grade, { GradeNode[grade] } };
				if (type == 6) {
					kind.needs.push_back("gemskull");
				}
				out.push_back(std::move(kind));
			}
		}
		static constexpr const char* Runes[33] = { "El", "Eld", "Tir", "Nef", "Eth", "Ith", "Tal", "Ral", "Ort", "Thul", "Amn", "Sol", "Shael",
			"Dol", "Hel", "Io", "Lum", "Ko", "Fal", "Lem", "Pul", "Um", "Mal", "Ist", "Gul", "Vex", "Ohm", "Lo", "Sur", "Ber", "Jah", "Cham", "Zod" };
		for (int i = 0; i < 33; ++i) {
			const int n = i + 1;
			const std::string code = (n < 10 ? "r0" : "r") + std::to_string(n);
			out.push_back({ code, std::string(Runes[i]) + " Rune", "rune/" + Camp::Lower(Runes[i]) + "_rune", Section::Runes, i % 11, i / 11,
				{ n <= 8 ? "rune1" : n <= 13 ? "rune2" : n <= 20 ? "rune3" : n <= 27 ? "rune4" : "rune5" } });
		}
		struct Potion { const char* code; const char* name; const char* picture; int column, row; const char* need; };
		static constexpr Potion Potions[] = {
			{ "mp1", "Minor Mana Potion", "lesser_mana_potion", 0, 0, "pot1" },
			{ "mp2", "Light Mana Potion", "light_mana_potion", 0, 1, "pot1" },
			{ "mp3", "Mana Potion", "mana_potion2", 0, 2, "pot1" },
			{ "mp4", "Greater Mana Potion", "strong_mana_potion", 0, 3, "pot1" },
			{ "mp5", "Super Mana Potion", "greater_mana_potion", 0, 4, "pot1" },
			{ "hp1", "Minor Healing Potion", "lesser_healing_potion", 1, 0, "pot2" },
			{ "hp2", "Light Healing Potion", "light_healing_potion", 1, 1, "pot2" },
			{ "hp3", "Healing Potion", "healing_potion2", 1, 2, "pot2" },
			{ "hp4", "Greater Healing Potion", "strong_healing_potion", 1, 3, "pot2" },
			{ "hp5", "Super Healing Potion", "greater_healing_potion", 1, 4, "pot2" },
			{ "rvs", "Rejuvenation Potion", "rejuv_potion", 2, 0, "pot3" },
			{ "rvl", "Full Rejuvenation Potion", "full_rejuv_potion", 2, 1, "pot3" },
		};
		for (const auto& p : Potions) {
			out.push_back({ p.code, p.name, std::string("potion/") + p.picture, Section::Potions, p.column, p.row, { p.need } });
		}
		return out;
	}();
	return all;
}

inline auto KindOf(const std::string& code) -> const Kind* {
	for (const auto& kind : Kinds()) {
		if (kind.code == code) {
			return &kind;
		}
	}
	return nullptr;
}

// ---- The save's state ---------------------------------------------------------------------
enum class Buy { Bought, AlreadyOwned, Locked, NotEnoughPoints, Unknown };

struct State {
	int       tier = 0;   // tiers bought
	long long fund = 0;   // gold paid toward the next tier
	std::set<std::pair<int, int>> acts;     // (class, act) finished: a point each
	std::set<std::string>         nodes;
	std::map<std::string, int>    mats;
	std::set<int>                 legacy;   // locked pages (0-based) that still hold old items
	bool                          legacyDone = false;

	auto Pages() const -> int { return 1 + tier; }
	auto PageOpen(int page) const -> bool { return page >= 0 && (page < Pages() || legacy.count(page) != 0); }
	// The last page the arrows may reach, counted from 1.
	auto LastPage() const -> int { return (std::max)(Pages(), legacy.empty() ? 0 : *legacy.rbegin() + 1); }
	auto Has(const std::string& node) const -> bool { return nodes.count(node) != 0; }
	auto Price() const -> long long { return tier < TierCount ? TierPrice[static_cast<size_t>(tier)] : 0; }
	auto Owing() const -> long long { return tier < TierCount ? (std::max)(0LL, Price() - fund) : 0; }
	auto Earned() const -> int { return static_cast<int>(acts.size()); }
	auto Spent() const -> int {
		int total = 0;
		for (const auto& node : Nodes) {
			total += Has(node.id) ? node.cost : 0;
		}
		return total;
	}
	auto Points() const -> int { return Earned() - Spent(); }

	// Gold was paid in (never more than Owing()). True when it finished the tier.
	auto Pay(long long gold) -> bool {
		if (tier >= TierCount || gold <= 0) {
			return false;
		}
		fund += (std::min)(gold, Owing());
		if (fund < Price()) {
			return false;
		}
		fund = 0;
		++tier;
		return true;
	}

	// What buying the node would do (Bought: it can be bought now).
	auto CanBuy(const std::string& id) const -> Buy {
		const Node* node = FindNode(id);
		if (node == nullptr) { return Buy::Unknown; }
		if (Has(id)) { return Buy::AlreadyOwned; }
		if (node->parent[0] != '\0' && !Has(node->parent)) { return Buy::Locked; }
		return Points() >= node->cost ? Buy::Bought : Buy::NotEnoughPoints;
	}
	auto BuyNode(const std::string& id) -> Buy {
		const Buy result = CanBuy(id);
		if (result == Buy::Bought) {
			nodes.insert(id);
		}
		return result;
	}

	auto Count(const std::string& code) const -> int {
		const auto it = mats.find(code);
		return it == mats.end() ? 0 : it->second;
	}
	auto Open(const Kind& kind) const -> bool {
		for (const char* node : kind.needs) {
			if (!Has(node)) {
				return false;
			}
		}
		return Has("tab");
	}
	auto Cap(const Kind& kind) const -> int {
		return Has(kind.section == Section::Gems ? "gemcap" : kind.section == Section::Runes ? "runecap" : "potcap") ? LargeCap : SmallCap;
	}
	// How many more of this kind may be put in.
	auto Room(const Kind& kind) const -> int { return Open(kind) ? (std::max)(0, Cap(kind) - Count(kind.code)) : 0; }
	void Add(const std::string& code, int n) {
		int& count = mats[code];
		count += n;
		if (count <= 0) {
			mats.erase(code);
		}
	}

	// The gem cutter: every 3 of one quality (0 Chipped - 3 Flawless) become 1 of the next, as far
	// as there is room for the better gem. Returns how many were made.
	auto CutGems(int grade) -> int {
		if (!Has("gemcut") || grade < 0 || grade > 3) {
			return 0;
		}
		int made = 0;
		for (const auto& from : Kinds()) {
			if (from.section != Section::Gems || from.row != grade) {
				continue;
			}
			for (const auto& to : Kinds()) {
				if (to.section != Section::Gems || to.row != grade + 1 || to.column != from.column) {
					continue;
				}
				const int n = (std::min)(Count(from.code) / 3, Room(to));
				if (n > 0) {
					Add(from.code, -3 * n);
					Add(to.code, n);
					made += n;
				}
			}
		}
		return made;
	}
};

inline constexpr int LastTierOfAct[5] = { 5, 10, 14, 18, 24 };  // route.h: Andariel, Duriel, Mephisto, Diablo, Baal

// Counts the acts this hero's route progress says are finished. Returns the new points.
inline auto CountActs(State& state, int cls, const Route::Progress& progress) -> int {
	int added = 0;
	if (cls < 0 || cls >= Camp::ClassCount) {
		return 0;
	}
	for (int act = 1; act <= 5; ++act) {
		if (progress.TierDone(LastTierOfAct[act - 1]) && state.acts.insert({ cls, act }).second) {
			++added;
		}
	}
	return added;
}

// ---- stash.txt ----------------------------------------------------------------------------
inline auto Serialize(const State& state) -> std::string {
	std::ostringstream out;
	out << "version 1\ntier " << state.tier << "\nfund " << state.fund << "\n";
	for (const auto& [cls, act] : state.acts) { out << "act " << Camp::ClassNames[cls] << " " << act << "\n"; }
	for (const auto& node : state.nodes)      { out << "node " << node << "\n"; }
	for (const auto& [code, n] : state.mats)  { out << "mat " << code << " " << n << "\n"; }
	for (const int page : state.legacy)       { out << "legacy " << page << "\n"; }
	out << "legacydone " << (state.legacyDone ? 1 : 0) << "\n";
	return out.str();
}

inline auto Parse(const std::string& text) -> State {
	State state;
	std::istringstream in(text);
	std::string line;
	while (std::getline(in, line)) {
		std::istringstream words(line);
		std::string key, value;
		words >> key >> value;
		const long long number = std::atoll(value.c_str());
		if (key == "tier") {
			state.tier = static_cast<int>(std::clamp(number, 0LL, static_cast<long long>(TierCount)));
		} else if (key == "fund") {
			state.fund = (std::max)(0LL, number);
		} else if (key == "act") {
			int act = 0;
			words >> act;
			const int cls = Camp::ClassFromName(value);
			if (cls >= 0 && act >= 1 && act <= 5) {
				state.acts.insert({ cls, act });
			}
		} else if (key == "node" && FindNode(value) != nullptr) {
			state.nodes.insert(value);
		} else if (key == "mat") {
			int n = 0;
			words >> n;
			if (n > 0 && KindOf(value) != nullptr) {
				state.mats[value] = n;
			}
		} else if (key == "legacy" && number > 0 && number < PageCount) {
			state.legacy.insert(static_cast<int>(number));
		} else if (key == "legacydone") {
			state.legacyDone = number == 1;
		}
	}
	return state;
}

// ---- The panels ---------------------------------------------------------------------------
// A plugin can't change a widget in a game (CLAUDE.md, "Lessons learned"), so every change makes a
// whole new layout. PanelMark is replaced by the panel's name.
inline constexpr char PanelMark[]     = "@PANEL@";
inline constexpr char ActionMessage[] = "PanelManager:OpenPanel:darkestdiablo/stash-";

inline auto Commas(long long n) -> std::string {
	std::string text = std::to_string(n);
	for (int at = static_cast<int>(text.size()) - 3; at > 0; at -= 3) {
		text.insert(static_cast<size_t>(at), ",");
	}
	return text;
}

namespace Ui {

struct Rect { int x, y, w, h; };

inline constexpr char Gold[] = "[0.75,0.56,0.16,1.0]", Bronze[] = "[0.36,0.28,0.14,1.0]", Dim[] = "[0.24,0.22,0.19,1.0]", Dark[] = "[0.06,0.055,0.05,1.0]";
inline constexpr char TextGold[] = "$FontColorGoldYellow", TextWhite[] = "$FontColorWhite", TextGrey[] = "$FontColorDarkGrayBlue",
	TextBeige[] = "$FontColorBeige", TextGreen[] = "$FontColorGreen";

inline auto RectJson(const Rect& r) -> std::string {
	return "{\"x\":" + std::to_string(r.x) + ",\"y\":" + std::to_string(r.y) + ",\"width\":" + std::to_string(r.w) + ",\"height\":" + std::to_string(r.h) + "}";
}
inline auto Box(const std::string& name, const Rect& r, const char* color) -> std::string {
	return "{\"type\":\"RectangleWidget\",\"name\":\"" + name + "\",\"fields\":{\"rect\":" + RectJson(r) + ",\"color\":" + color + "}}";
}
inline auto Text(const std::string& name, const Rect& r, const std::string& text, const char* color, const char* size, const char* h = "center") -> std::string {
	return "{\"type\":\"TextBoxWidget\",\"name\":\"" + name + "\",\"fields\":{\"rect\":" + RectJson(r) + ",\"text\":\"" + text +
		"\",\"style\":{\"fontColor\":\"" + color + "\",\"pointSize\":" + (size[0] == '$' ? "\"" + std::string(size) + "\"" : std::string(size)) + ",\"alignment\":{\"h\":\"" + h +
		"\",\"v\":\"center\"},\"dropShadow\":\"$DefaultDropShadow\"}}}";
}
inline constexpr int ButtonW = 366, ButtonH = 93;  // FrontEnd_ButtonSmall at full size
inline auto Button(const std::string& name, int x, int y, double scale, const std::string& text, const char* color, const std::string& action,
	const std::string& tooltip = {}, const char* size = "$LargeFontSize") -> std::string {
	char rect[96] {};
	std::snprintf(rect, sizeof(rect), "{\"x\":%d,\"y\":%d,\"scale\":%.2f}", x, y, scale);
	return "{\"type\":\"ButtonWidget\",\"name\":\"" + name + "\",\"fields\":{\"rect\":" + rect +
		",\"filename\":\"FrontEnd\\\\HD\\\\Final\\\\FrontEnd_ButtonSmall\",\"pressedFrame\":1,\"disabledFrame\":2,\"hoveredFrame\":3,\"textString\":\"" + text +
		"\",\"pointSize\":\"" + size + "\",\"textColor\":\"" + color + "\"" + (tooltip.empty() ? "" : ",\"tooltipString\":\"" + tooltip + "\"") +
		",\"onClickMessage\":\"" + ActionMessage + action + "\"}}";
}
inline auto CloseButton(int x, const std::string& action) -> std::string {
	return "{\"type\":\"ButtonWidget\",\"name\":\"Close\",\"fields\":{\"rect\":{\"x\":" + std::to_string(x) +
		",\"y\":9},\"filename\":\"PANEL\\\\closebtn_4x\",\"hoveredFrame\":3,\"tooltipString\":\"@d2r:strClose\",\"sound\":\"cursor_close_window_hd\",\"onClickMessage\":\"" +
		ActionMessage + action + "\"}}";
}
// Keeps clicks from the game behind the panel.
inline auto Catcher(int w, int h) -> std::string {
	return "{\"type\":\"ClickCatcherWidget\",\"name\":\"Catcher\",\"fields\":{\"rect\":" + RectJson({ 0, 0, w, h }) + "}}";
}
inline auto Panel(const Rect& r, const char* anchor, int priority, const std::string& children) -> std::string {
	return std::string("{\"type\":\"Panel\",\"name\":\"") + PanelMark + "\",\"fields\":{\"rect\":" + RectJson(r) + ",\"anchor\":\"" + anchor +
		"\",\"priority\":" + std::to_string(priority) + "},\"children\":[" + children + "]}";
}

}  // namespace Ui

inline constexpr int    TreeW = 1900, TreeH = 1507, TreeX = 420, TreeColumnW = 245, TreeRowY = 190, TreeRowH = 205;
inline constexpr double NodeScale = 0.65;
inline constexpr int    SideW = 275, SideH = 70, SideY = 151, SidePitch = 76;  // the Caravan / Stash button at a window's right edge

// The caravan window, in the stash's place (the inventory stays beside it): the page tiers down its left side, the passive tree beside them.
inline auto TreeLayout(const State& state) -> std::string {
	using namespace Ui;
	std::string out = Catcher(TreeW, TreeH) + "," + Box("Edge", { 0, 0, TreeW, TreeH }, Bronze) + "," +
		Box("Sheet", { 4, 4, TreeW - 8, TreeH - 8 }, "[0.035,0.03,0.028,1.0]") + "," + CloseButton(TreeW - 90, "closebank") + "," +
		Button("Stash", TreeW + 2, SideY, 0.75, "Stash", TextGold, "tree", "Back to the stash");
	const auto add = [&](const std::string& widget) { out += "," + widget; };

	add(Text("PagesTitle", { 30, 50, 360, 50 }, "Caravan Stash", TextGold, "$LargeFontSize"));
	add(Text("PagesNow", { 30, 100, 360, 40 }, std::to_string(state.Pages()) + " of " + std::to_string(PageCount) + " pages", TextBeige, "$MediumFontSize"));
	for (int i = 0; i < TierCount; ++i) {
		const std::string n = std::to_string(i);
		const int y = 150 + i * 125;
		const bool owned = i < state.tier, next = i == state.tier;
		const long long price = TierPrice[static_cast<size_t>(i)];
		add(Box("TierEdge" + n, { 40, y, 340, 112 }, owned ? Gold : next ? "[0.55,0.52,0.45,1.0]" : "[0.16,0.15,0.14,1.0]"));
		add(Box("TierBox" + n, { 43, y + 3, 334, 106 }, owned ? "[0.17,0.13,0.06,1.0]" : Dark));
		if (next && state.fund > 0) {
			add(Box("TierBar", { 43, y + 97, static_cast<int>(334 * state.fund / price), 12 }, Gold));
		}
		add(Text("TierName" + n, { 43, y + 8, 334, 46 }, "Stash page " + std::to_string(i + 2), owned ? TextGold : next ? TextWhite : TextGrey, "$MediumLargeFontSize"));
		add(Text("TierPrice" + n, { 43, y + 54, 334, 38 },
			owned ? "Unlocked" : (next && state.fund > 0 ? Commas(state.fund) + " / " : std::string()) + Commas(price) + " gold",
			owned ? TextGreen : next ? TextBeige : TextGrey, "$MediumFontSize"));
	}
	if (state.tier < TierCount) {
		add(Button("PaySome", 44, 1290, 0.9, "Pay 10,000 gold", TextWhite, "pay-10000", "Pay gold toward the next page. Any hero of this save can add to it."));
		add(Button("PayAll", 44, 1384, 0.9, "Pay all gold", TextWhite, "pay-all", "Pay all the gold this hero carries, up to what is still missing."));
	} else {
		add(Text("PagesDone", { 40, 1320, 340, 60 }, "Every page is unlocked", TextGreen, "$MediumFontSize"));
	}

	add(Box("Divider", { 408, 40, 3, TreeH - 80 }, Bronze));
	add(Text("TreeTitle", { TreeX, 50, 1470, 50 }, "Caravan Passives", TextGold, "$LargeFontSize"));
	add(Text("Points", { TreeX, 100, 1470, 40 }, std::to_string(state.Points()) + (state.Points() == 1 ? " point" : " points") + " to spend  (" +
		std::to_string(state.Earned()) + " of " + std::to_string(Camp::ClassCount * 5) + " earned)", state.Points() > 0 ? TextGreen : TextBeige, "$MediumFontSize"));
	const int bw = static_cast<int>(ButtonW * NodeScale), bh = static_cast<int>(ButtonH * NodeScale);
	const auto at = [&](const Node& node) -> std::pair<int, int> {  // a node's button: top left
		return { TreeX + static_cast<int>(node.column * TreeColumnW) + (TreeColumnW - bw) / 2, TreeRowY + node.row * TreeRowH };
	};
	// Lines first, so the buttons lie over them: down from the parent, across, down to the child.
	for (const bool lit : { false, true }) {
		for (const auto& node : Nodes) {
			const Node* parent = FindNode(node.parent);
			if (parent == nullptr || state.Has(node.id) != lit) {
				continue;
			}
			const auto [px, py] = at(*parent);
			const auto [cx, cy] = at(node);
			const int from = px + bw / 2, to = cx + bw / 2, turn = cy - 46, top = py + bh + 36;
			const char* color = lit ? Gold : Dim;
			const std::string id = node.id;
			add(Box("LineA" + id, { from - 2, top, 4, turn - top + 2 }, color));
			add(Box("LineB" + id, { (std::min)(from, to) - 2, turn, (std::max)(from, to) - (std::min)(from, to) + 4, 4 }, color));
			add(Box("LineC" + id, { to - 2, turn, 4, cy - turn }, color));
		}
	}
	for (const auto& node : Nodes) {
		const auto [x, y] = at(node);
		const Buy buy = state.CanBuy(node.id);
		const bool owned = buy == Buy::AlreadyOwned, now = buy == Buy::Bought;
		const std::string id = node.id;
		add(Button("Node" + id, x, y, NodeScale, node.label, owned ? TextGold : now ? TextWhite : "$FontColorDarkGrayGold", "node-" + id, node.text));
		add(Text("Cost" + id, { x, y + bh + 2, bw, 32 }, owned ? "Owned" : std::to_string(node.cost) + (node.cost == 1 ? " point" : " points"),
			owned ? TextGreen : now ? TextBeige : TextGrey, "$SmallFontSize"));
	}
	add(Text("Hint", { TreeX, TreeH - 62, 1470, 36 }, "A point is earned each time a class finishes an act for the first time in this save.", "$FontColorDarkGrayGold", "$MediumFontSize"));
	return Panel({ 236, -651, TreeW + 2 + SideW, TreeH }, "$LeftPanelAnchor", 8700, out);  // over the Upgrades tab (8600) too
}

// The Upgrades tab: lies over the stash, the same size, on the stash's own picture. Only unlocked
// slots are drawn, and any slot that holds something.
inline auto UpgradesLayout(const State& state) -> std::string {
	using namespace Ui;
	constexpr int W = 1162, H = 1507;
	std::string out = Catcher(W, H) + ",{\"type\":\"ImageWidget\",\"name\":\"Background\",\"fields\":{\"filename\":\"PANEL\\\\Stash\\\\StashPanel_BG_Shared\"}}," +
		Box("Sheet", { 70, 218, W - 140, 1215 }, "[0.035,0.03,0.028,1.0]") + "," + CloseButton(1075, "closebank");
	const auto add = [&](const std::string& widget) { out += "," + widget; };
	const auto tab = [&](const std::string& name, int x, bool on, const std::string& text, const std::string& action) {
		const std::string frame = on ? "1" : "0";
		add("{\"type\":\"ButtonWidget\",\"name\":\"" + name + "\",\"fields\":{\"rect\":{\"x\":" + std::to_string(x) +
			",\"y\":146},\"filename\":\"PANEL\\\\stash\\\\MKB_Tabs_195_TGA\",\"normalFrame\":" + frame + ",\"pressedFrame\":1,\"hoveredFrame\":" + frame +
			",\"disabledFrame\":" + frame + ",\"textString\":\"" + text + "\",\"pointSize\":\"$SmallPanelFontSize\",\"textColor\":\"" +
			(on ? "$TabsActiveTextColor" : "$TabsInactiveTextColor") + "\",\"onClickMessage\":\"" + ActionMessage + action + "\"}}");
	};
	add("{\"type\":\"TextBoxWidget\",\"name\":\"Title\",\"fields\":{\"rect\":\"$InGamePanelTitleRect\",\"text\":\"Upgrades\",\"style\":\"$StyleTitleBlock\"}}");
	tab("TabShared", 82, false, "@d2r:shared", "shared");
	tab("TabUpgrades", 277, true, "Upgrades", "upgrades");
	if (state.Has("quick")) {
		add(Button("Deposit", 820, 1340, 0.7, "Store bag", TextWhite, "deposit", "Store everything in the bag that the unlocked slots take."));
	}
	struct Place { int x, y, pitchX, pitchY; const char* title; int w; };
	static constexpr Place Places[3] = {
		{ 84, 290, 88, 98, "Gems", 612 },
		{ 84, 880, 91, 116, "Runes", 994 },
		{ 816, 290, 91, 98, "Potions", 266 },
	};
	const auto shown = [&](const Kind& kind) { return state.Open(kind) || state.Count(kind.code) > 0; };
	bool any[3] {};
	for (const auto& kind : Kinds()) {
		any[static_cast<int>(kind.section)] |= shown(kind);
	}
	for (int s = 0; s < 3; ++s) {
		const Place& p = Places[s];
		const std::string n = std::to_string(s);
		if (any[s]) {
			add(Text("Section" + n, { p.x, p.y - 58, p.w, 44 }, p.title, TextGold, "$MediumLargeFontSize", "left"));
			add(Box("Rule" + n, { p.x, p.y - 12, p.w, 3 }, Bronze));
		}
	}
	if (!any[0] && !any[1] && !any[2]) {
		add(Text("Empty", { 100, 620, W - 200, 60 }, "Caravan points open the sections here.", TextBeige, "$MediumFontSize"));
	}
	for (const auto& kind : Kinds()) {
		if (!shown(kind)) {
			continue;
		}
		const Place& p = Places[static_cast<int>(kind.section)];
		const int x = p.x + kind.column * p.pitchX, y = p.y + kind.row * p.pitchY, count = state.Count(kind.code);
		const int cap = state.Open(kind) ? state.Cap(kind) : count;
		std::string picture = "items\\\\misc\\\\" + kind.picture;
		for (size_t slash = picture.find('/'); slash != std::string::npos; slash = picture.find('/')) {
			picture.replace(slash, 1, "\\\\");
		}
		add(Box("SlotEdge_" + kind.code, { x, y, 84, 84 }, count > 0 ? "[0.55,0.43,0.18,1.0]" : "[0.2,0.18,0.15,1.0]"));
		add(Box("Slot_" + kind.code, { x + 2, y + 2, 80, 80 }, "[0.08,0.075,0.07,1.0]"));
		add("{\"type\":\"ButtonWidget\",\"name\":\"Take_" + kind.code + "\",\"fields\":{\"rect\":{\"x\":" + std::to_string(x + 2) + ",\"y\":" + std::to_string(y + 2) +
			",\"scale\":0.81},\"filename\":\"" + picture + "\",\"normalFrame\":0,\"pressedFrame\":0,\"hoveredFrame\":0,\"disabledFrame\":0" +
			(count > 0 ? "" : ",\"tint\":{\"r\":0.35,\"g\":0.35,\"b\":0.35,\"a\":0.5}") + ",\"tooltipString\":\"" + kind.name + ": " + std::to_string(count) + " of " +
			std::to_string(cap) + "\",\"onClickMessage\":\"" + ActionMessage + "slot-" + kind.code + "\"}}");
		if (count > 0) {
			add(Text("Count_" + kind.code, { x + 6, y + 2, 72, 26 }, std::to_string(count), TextWhite, "$SmallFontSize", "left"));
		}
	}
	if (state.Has("gemcut")) {
		for (int grade = 0; grade < 4; ++grade) {
			add("{\"type\":\"ButtonWidget\",\"name\":\"Cut" + std::to_string(grade) + "\",\"fields\":{\"rect\":{\"x\":" + std::to_string(Places[0].x + 7 * 88 + 12) +
				",\"y\":" + std::to_string(Places[0].y + grade * 98 + 10) +
				",\"width\":64,\"height\":64},\"filename\":\"PANEL\\\\Stash\\\\Stash_RightArrow\",\"normalFrame\":0,\"pressedFrame\":2,\"hoveredFrame\":1,\"disabledFrame\":3,"
				"\"tooltipString\":\"Turn every 3 equal gems of this row into 1 of the next quality\",\"onClickMessage\":\"" + ActionMessage + "cut-" + std::to_string(grade) + "\"}}");
		}
	}
	add(Text("Help1", { 84, 1284, 994, 36 }, "Click: take one out, or store the item in hand.", "$FontColorDarkGrayGold", "$SmallFontSize", "left"));
	add(Text("Help3", { 84, 1356, 730, 36 }, "Runes: first click looks up, next takes out.", "$FontColorDarkGrayGold", "18", "left"));
	add(Text("Help2", { 84, 1320, 730, 36 }, "Ctrl + click a bag item to store it.", "$FontColorDarkGrayGold", "$SmallFontSize", "left"));
	return Panel({ 236, -651, W, H }, "$LeftPanelAnchor", 8600, out);
}

// The Journal button (journal.h), and the button attached to the stash's right side that shows the caravan window in its place.
inline auto SideLayout() -> std::string {
	using namespace Ui;
	return Panel({ 1400, -651 + SideY, SideW, SidePitch + SideH }, "$LeftPanelAnchor", 8500, Button("Caravan", 0, 0, 0.75, "Caravan", TextGold, "tree", "Stash pages and passives") + "," +
		Button("Journal", 0, SidePitch, 0.75, "Journal", TextGold, "journal", "Runewords of each rune"));
}

}
