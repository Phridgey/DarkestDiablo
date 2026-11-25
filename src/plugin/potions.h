// Darkest Diablo — the hero's permanent potions: charges, loadout, what each one does, and the stop
// affixes that give more of them (no game or Windows code here, so it can be tested anywhere; see
// tests/camp_tests.cpp). How it plays and what the engine forced: docs/potions.md.
//
// On disk: camp/slot<N>/potions/<hero>.txt, one fact per line:
//   "charge <kind> <charges>", "own <kind>", "slot <1-4> <kind>", "blend <0-2>",
//   "affix <affix> <stop id>" (still to be won), "rolled 1".
#pragma once

#include "route.h"
#include "stash.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace Potions {

namespace fs = std::filesystem;

enum Kind : int { Heal = 0, Mana = 1, Rejuv = 2, Antidote = 3, Thawing = 4, Stamina = 5, Fire = 6, Gas = 7, KindCount = 8, None = -1 };
inline constexpr const char* KindIds[KindCount] = { "heal", "mana", "rejuv", "antidote", "thawing", "stamina", "fire", "gas" };
inline auto KindFromId(const std::string& id) -> int {
	for (int k = 0; k < KindCount; ++k) {
		if (id == KindIds[k]) {
			return k;
		}
	}
	return None;
}
inline constexpr auto Support(int kind) -> bool { return kind == Antidote || kind == Thawing || kind == Stamina; }
inline constexpr auto Thrown(int kind) -> bool { return kind == Fire || kind == Gas; }

// ---- Numbers --------------------------------------------------------------------------------
inline constexpr int    SlotCount       = 4;
inline constexpr double UseCost         = 10;     // charges one use takes
inline constexpr double MainCapacity    = 50;     // healing and mana: 5 uses
inline constexpr double SupportCapacity = 20;
inline constexpr double ThrownCapacity  = 30;
inline constexpr double KillCharge      = 0.5;    // a normal monster of the hero's level
inline constexpr double SupportKillRate = 0.75;
inline constexpr double RegenPerSecond  = 0.001;  // of the capacity
inline constexpr int    ThrownPerUse    = 5;      // throwing potions one use makes
inline constexpr int    ThrownCarried   = 25;     // the most the hero may own of one kind for a use to make more
inline constexpr double EchoStrength    = 0.5;    // a support potion set off by a healing or mana potion
inline constexpr int    SupportSeconds  = 30;

// Quality by level; the amounts and lengths are misc.txt's own (before the class's bonus).
struct Grade { int level; const char* name; const char* picture; int amount; double seconds; };
inline constexpr Grade HealGrades[] = {
	{  1, "Minor Healing Potion",   "lesser_healing_potion",   30, 192 / 25.0 },
	{ 10, "Light Healing Potion",   "light_healing_potion",    60, 160 / 25.0 },
	{ 20, "Healing Potion",         "healing_potion2",        100, 171 / 25.0 },
	{ 36, "Greater Healing Potion", "strong_healing_potion",  180, 192 / 25.0 },
	{ 54, "Super Healing Potion",   "greater_healing_potion", 320, 256 / 25.0 },
};
inline constexpr Grade ManaGrades[] = {
	{  1, "Light Mana Potion",   "light_mana_potion",    40, 128 / 25.0 },
	{ 20, "Mana Potion",         "mana_potion2",         80, 128 / 25.0 },
	{ 36, "Greater Mana Potion", "strong_mana_potion",  150, 128 / 25.0 },
	{ 54, "Super Mana Potion",   "greater_mana_potion", 250, 128 / 25.0 },
};
template <size_t N>
auto GradeAt(const Grade (&grades)[N], int level) -> const Grade& {
	size_t best = 0;
	for (size_t i = 0; i < N; ++i) {
		best = level >= grades[i].level ? i : best;
	}
	return grades[best];
}

// The game's class bonus (class numbers as in camp.h: 0 Amazon ... 7 Warlock).
inline auto HealFactor(int cls) -> double { return cls == 4 ? 2.0 : cls == 0 || cls == 3 || cls == 6 ? 1.5 : 1.0; }
inline auto ManaFactor(int cls) -> double { return cls == 4 ? 1.0 : cls == 0 || cls == 3 || cls == 6 ? 1.5 : 2.0; }

struct Pot { const char* code; const char* name; const char* picture; int level; };
inline constexpr Pot FirePots[] = {
	{ "opl", "Fulminating Potion", "fulminating_potion", 1 }, { "opm", "Exploding Potion", "exploding_potion", 12 }, { "ops", "Oil Potion", "oil_potion", 20 },
};
inline constexpr Pot GasPots[] = {
	{ "gpl", "Strangling Gas Potion", "strangling_gas_potion", 6 }, { "gpm", "Choking Gas Potion", "choking_gas_potion", 16 }, { "gps", "Rancid Gas Potion", "rancid_gas_potion", 24 },
};

// ---- Kills ------------------------------------------------------------------------------------
enum class Rank : int { Normal = 0, Champion = 1, Unique = 2 };  // Unique: uniques, super uniques, minions and bosses, as for experience
// The game's own experience multipliers: the monster's kind, then the gap between the two levels.
inline auto RankFactor(Rank rank) -> double { return rank == Rank::Unique ? 5 : rank == Rank::Champion ? 3 : 1; }
inline auto LevelFactor(int hero, int monster) -> double {
	if (hero <= 0 || monster <= 0) {
		return 1;  // unknown
	}
	static constexpr double Falling[] = { 1, 0.81, 0.62, 0.43, 0.24, 0.05 };
	const int gap = monster - hero;
	if (gap > 0 && hero >= 25) {
		return static_cast<double>(hero) / monster;
	}
	return Falling[std::clamp(std::abs(gap) - 5, 0, 5)];
}

// ---- Belts ------------------------------------------------------------------------------------
// The slots a belt adds past the first 4 in the game's tables (belts.txt), as extra charges.
inline auto BeltCharges(const std::string& code) -> int {
	if (code == "lbl" || code == "vbl") { return 4; }
	if (code == "mbl" || code == "tbl") { return 8; }
	for (const char* c : { "hbl", "zlb", "zvb", "zmb", "ztb", "zhb", "ulc", "uvc", "umc", "utc", "uhc" }) {
		if (code == c) { return 12; }
	}
	return 0;
}

// ---- Stop affixes -----------------------------------------------------------------------------
struct Affix { const char* id; int act; const char* label; const char* picture; };
inline constexpr Affix Affixes[] = {
	{ "antidote", 1, "Antidote",                  "antidote_potion" },
	{ "stamina",  1, "Stamina",                   "stamina_potion" },
	{ "thawing",  3, "Thawing",                   "thawing_potion" },
	{ "concoct",  4, "Concoct Rejuvenation",      "rejuv_potion" },
	{ "full",     5, "Concoct Full Rejuvenation", "full_rejuv_potion" },
};
inline constexpr int FullChance = 5;  // percent, once per hero

inline auto AffixById(const std::string& id) -> const Affix* {
	for (const auto& affix : Affixes) {
		if (id == affix.id) {
			return &affix;
		}
	}
	return nullptr;
}

// ---- The hero's potions -----------------------------------------------------------------------
struct Owner {  // what the rules need to know besides the potions themselves
	std::set<std::string> nodes;  // the save's caravan passives
	int level = 1;
	int belt = 0;                 // BeltCharges of what is worn
	auto Has(const char* node) const -> bool { return nodes.count(node) != 0; }
};

struct State {
	std::array<double, KindCount> charge {};
	std::set<int>                 own { Heal, Mana };
	std::array<int, SlotCount>    slots { Heal, Mana, None, None };
	int                           blend = 0;   // 0 healing and mana apart, 1 Rejuvenation, 2 Full Rejuvenation
	bool                          rolled = false;
	std::map<std::string, std::string> affixes;  // affix -> stop id, still to be won

	auto Owns(int kind, const Owner& o) const -> bool {
		if (kind == None) {
			return false;
		}
		if (Thrown(kind)) {
			return o.Has(kind == Fire ? "potfire" : "potgas");
		}
		return own.count(kind) != 0;
	}
	auto Charge(int kind) const -> double { return charge[static_cast<size_t>(kind)]; }
	auto Capacity(int kind, const Owner& o) const -> double {
		const double heal = o.Has("potheal") ? 0.25 : 0, mana = o.Has("potmana") ? 0.25 : 0;
		if (Thrown(kind))  { return ThrownCapacity + o.belt; }
		if (Support(kind)) { return SupportCapacity + o.belt; }
		if (kind == Heal)  { return MainCapacity * (1 + heal) + o.belt; }
		if (kind == Mana)  { return MainCapacity * (1 + mana) + o.belt; }
		return (MainCapacity * (1 + heal + mana) + o.belt) * (blend == 2 ? 0.5 : 0.75);
	}
	auto Regen(const Owner& o) const -> double { return RegenPerSecond * (1 + (o.Has("potregen") ? 1 : 0) + (o.Has("potregen2") ? 1 : 0)); }
	auto Ready(int kind, const Owner& o) const -> bool { return Owns(kind, o) && Charge(kind) >= UseCost - 1e-6; }
	auto Uses(int kind) const -> int { return static_cast<int>((Charge(kind) + 1e-6) / UseCost); }

	void Gain(int kind, double amount, const Owner& o) {
		charge[static_cast<size_t>(kind)] = (std::min)(Capacity(kind, o), Charge(kind) + amount);
	}
	void Tick(double seconds, const Owner& o) {
		for (int k = 0; k < KindCount; ++k) {
			if (Owns(k, o)) {
				Gain(k, Capacity(k, o) * Regen(o) * seconds, o);
			}
		}
	}
	void Kill(Rank rank, int monsterLevel, const Owner& o) {
		const double amount = KillCharge * RankFactor(rank) * LevelFactor(o.level, monsterLevel) * (o.Has("potkill") ? 1.25 : 1.0);
		for (int k = 0; k < KindCount; ++k) {
			if (Owns(k, o)) {
				Gain(k, amount * (Support(k) ? SupportKillRate : 1.0), o);
			}
		}
	}
	void Fill(const Owner& o) {
		for (int k = 0; k < KindCount; ++k) {
			charge[static_cast<size_t>(k)] = Owns(k, o) ? Capacity(k, o) : 0;
		}
	}
	auto Use(int kind, const Owner& o) -> bool {
		if (!Ready(kind, o)) {
			return false;
		}
		charge[static_cast<size_t>(kind)] = (std::max)(0.0, Charge(kind) - UseCost);
		return true;
	}
	// The support potions a healing or mana potion sets off too, at half effect and no charge (passives).
	auto Echoes(int kind, const Owner& o) const -> std::vector<int> {
		std::vector<int> out;
		const bool heal = kind == Heal || kind == Rejuv, mana = kind == Mana || kind == Rejuv;
		if (heal && o.Has("potanti") && Owns(Antidote, o)) { out.push_back(Antidote); }
		if (heal && o.Has("potstam") && Owns(Stamina, o))  { out.push_back(Stamina); }
		if (mana && o.Has("potthaw") && Owns(Thawing, o))  { out.push_back(Thawing); }
		return out;
	}

	// Puts an owned potion in a slot; one already in another slot swaps places. None empties it.
	auto Equip(int slot, int kind, const Owner& o) -> bool {
		if (slot < 0 || slot >= SlotCount || (kind != None && !Owns(kind, o))) {
			return false;
		}
		int& target = slots[static_cast<size_t>(slot)];
		if (kind != None) {
			for (int& other : slots) {
				if (other == kind) {
					other = target;
				}
			}
		}
		target = kind;
		return true;
	}
	void AutoEquip(int kind) {
		if (std::find(slots.begin(), slots.end(), kind) != slots.end()) {
			return;
		}
		const auto free = std::find(slots.begin(), slots.end(), static_cast<int>(None));
		if (free != slots.end()) {
			*free = kind;
		}
	}
	void Give(int kind, const Owner& o) {
		if (own.insert(kind).second) {
			charge[static_cast<size_t>(kind)] = Capacity(kind, o);
			AutoEquip(kind);
		}
	}
	// Healing and mana become one potion (and later Full Rejuvenation), as full as they were.
	void Concoct(int to, const Owner& o) {
		if (to <= blend) {
			return;
		}
		double filled = 0;
		if (blend == 0) {
			filled = (Charge(Heal) / Capacity(Heal, o) + Charge(Mana) / Capacity(Mana, o)) / 2;
			charge[Heal] = charge[Mana] = 0;
			own.erase(Heal);
			own.erase(Mana);
			own.insert(Rejuv);
			bool placed = false;
			for (int& slot : slots) {
				if (slot == Heal || slot == Mana) {
					slot = placed ? static_cast<int>(None) : static_cast<int>(Rejuv);
					placed = true;
				}
			}
		} else {
			filled = Charge(Rejuv) / Capacity(Rejuv, o);
		}
		blend = to;
		charge[Rejuv] = Capacity(Rejuv, o) * std::clamp(filled, 0.0, 1.0);
		AutoEquip(Rejuv);
	}

	// Places the affixes, once per hero, on stops it can still clear (never an act's last stop). A
	// supporting potion whose act is already behind the hero (one older than this) is simply given.
	void Roll(const Route::Progress& progress, std::mt19937& rng, const Owner& o) {
		if (rolled) {
			return;
		}
		rolled = true;
		std::set<std::string> taken;
		for (const auto& affix : Affixes) {
			if (std::string(affix.id) == "full" && std::uniform_int_distribution<int>(1, 100)(rng) > FullChance) {
				continue;
			}
			std::vector<const Route::Stop*> open;
			for (const auto& stop : Route::Stops) {
				const bool last = std::find(std::begin(Stash::LastTierOfAct), std::end(Stash::LastTierOfAct), stop.tier) != std::end(Stash::LastTierOfAct);
				if (Route::ActOfTier(stop.tier) == affix.act && stop.travelLevel >= 0 && !last && !progress.TierDone(stop.tier) &&
					!(Route::IsAllOf(stop.tier) && progress.Clears(stop) > 0) && taken.count(stop.id) == 0) {
					open.push_back(&stop);
				}
			}
			if (!open.empty()) {
				const auto* stop = open[std::uniform_int_distribution<size_t>(0, open.size() - 1)(rng)];
				affixes[affix.id] = stop->id;
				taken.insert(stop->id);
			} else if (const int kind = KindFromId(affix.id); kind != None) {
				Give(kind, o);
			}
		}
	}
	// A stop was cleared: its affix takes effect and is gone. Returns it, or nothing.
	auto Cleared(const std::string& stop, const Owner& o) -> const Affix* {
		for (auto it = affixes.begin(); it != affixes.end(); ++it) {
			if (it->second != stop) {
				continue;
			}
			const Affix* affix = AffixById(it->first);
			affixes.erase(it);
			if (affix != nullptr) {
				const std::string id = affix->id;
				if (id == "concoct")   { Concoct(1, o); }
				else if (id == "full") { Concoct(2, o); }
				else                   { Give(KindFromId(id), o); }
			}
			return affix;
		}
		return nullptr;
	}
};

inline auto PotFor(int kind, const Owner& o) -> const Pot& {
	const auto& pots = kind == Fire ? FirePots : GasPots;
	int best = 0;
	for (int tier = 1; tier <= (o.Has("potmaster") ? 2 : o.Has("potstrong") ? 1 : 0); ++tier) {
		best = o.level >= pots[tier].level ? tier : best;
	}
	return pots[best];
}

inline auto Name(int kind, const State& s, const Owner& o) -> std::string {
	switch (kind) {
	case Heal:     return GradeAt(HealGrades, o.level).name;
	case Mana:     return GradeAt(ManaGrades, o.level).name;
	case Rejuv:    return s.blend == 2 ? "Full Rejuvenation Potion" : "Rejuvenation Potion";
	case Antidote: return "Antidote Potion";
	case Thawing:  return "Thawing Potion";
	case Stamina:  return "Stamina Potion";
	case Fire:
	case Gas:      return PotFor(kind, o).name;
	default:       return "Empty";
	}
}

// The picture, as a layout's JSON writes it.
inline auto Picture(int kind, const State& s, const Owner& o) -> std::string {
	const std::string misc = "items\\\\misc\\\\potion\\\\";
	switch (kind) {
	case Heal:     return misc + GradeAt(HealGrades, o.level).picture;
	case Mana:     return misc + GradeAt(ManaGrades, o.level).picture;
	case Rejuv:    return misc + (s.blend == 2 ? "full_rejuv_potion" : "rejuv_potion");
	case Antidote: return misc + "antidote_potion";
	case Thawing:  return misc + "thawing_potion";
	case Stamina:  return misc + "stamina_potion";
	default:       return std::string("items\\\\weapon\\\\potion\\\\") + PotFor(kind, o).picture;
	}
}

inline auto Effect(int kind, const State& s, const Owner& o) -> std::string {
	switch (kind) {
	case Heal:     return "heals " + std::to_string(GradeAt(HealGrades, o.level).amount) + " life (more for some classes) over time";
	case Mana:     return "restores " + std::to_string(GradeAt(ManaGrades, o.level).amount) + " mana (more for some classes) over time";
	case Rejuv:    return s.blend == 2 ? "restores all life and mana at once" : "restores 35% of life and mana at once";
	case Antidote: return "ends poison and keeps it off for 30 seconds";
	case Thawing:  return "ends cold and freezing and keeps them off for 30 seconds";
	case Stamina:  return "fills stamina and keeps it full for 30 seconds";
	default:       return "puts " + std::to_string(ThrownPerUse) + " throwing potions in the bag (level " + std::to_string(PotFor(kind, o).level) + ")";
	}
}

// ---- The file ---------------------------------------------------------------------------------
inline auto Serialize(const State& s) -> std::string {
	std::ostringstream out;
	out << "version 1\nrolled " << (s.rolled ? 1 : 0) << "\nblend " << s.blend << "\n";
	for (const int kind : s.own) {
		out << "own " << KindIds[kind] << "\n";
	}
	for (int k = 0; k < KindCount; ++k) {
		out << "charge " << KindIds[k] << " " << s.Charge(k) << "\n";
	}
	for (int i = 0; i < SlotCount; ++i) {
		const int kind = s.slots[static_cast<size_t>(i)];
		out << "slot " << i + 1 << " " << (kind == None ? "none" : KindIds[kind]) << "\n";
	}
	for (const auto& [affix, stop] : s.affixes) {
		out << "affix " << affix << " " << stop << "\n";
	}
	return out.str();
}

inline auto Parse(const std::string& text) -> State {
	State s;
	s.own.clear();
	s.slots.fill(None);
	std::istringstream in(text);
	std::string line;
	while (std::getline(in, line)) {
		std::istringstream words(line);
		std::string key, a, b;
		words >> key >> a >> b;
		const int kind = KindFromId(key == "slot" ? b : a);
		if (key == "rolled") {
			s.rolled = a == "1";
		} else if (key == "blend") {
			s.blend = std::clamp(std::atoi(a.c_str()), 0, 2);
		} else if (key == "own" && kind != None && !Thrown(kind)) {
			s.own.insert(kind);
		} else if (key == "charge" && kind != None) {
			const double c = std::atof(b.c_str());
			s.charge[static_cast<size_t>(kind)] = std::isfinite(c) ? std::clamp(c, 0.0, 1000.0) : 0;
		} else if (key == "slot") {
			const int slot = std::atoi(a.c_str()) - 1;
			if (slot >= 0 && slot < SlotCount && kind != None && std::find(s.slots.begin(), s.slots.end(), kind) == s.slots.end()) {
				s.slots[static_cast<size_t>(slot)] = kind;
			}
		} else if (key == "affix" && AffixById(a) != nullptr && Route::StopById(b) != nullptr) {
			s.affixes[a] = b;
		}
	}
	// The basic potions follow from the blend, whatever the file says.
	for (const int kind : { static_cast<int>(Heal), static_cast<int>(Mana), static_cast<int>(Rejuv) }) {
		if ((kind == Rejuv) == (s.blend > 0)) {
			s.own.insert(kind);
		} else {
			s.own.erase(kind);
			std::replace(s.slots.begin(), s.slots.end(), kind, static_cast<int>(None));
		}
	}
	return s;
}

// ---- On screen --------------------------------------------------------------------------------
inline constexpr char HudMessage[] = "PanelManager:OpenPanel:darkestdiablo/potion-";
inline constexpr int  CellPitch = 125, CellSize = 98, PickPitch = 108;
inline constexpr int  BarSteps = 20;  // the fill bars move in steps, so the panel isn't redrawn on every kill

// The four slots over the game's belt. `picking`: the slot whose list of potions is open (-1 none).
inline auto HudLayout(const State& s, const Owner& o, int picking) -> std::string {
	using namespace Stash::Ui;
	std::vector<int> owned;
	for (int k = 0; k < KindCount; ++k) {
		if (s.Owns(k, o)) {
			owned.push_back(k);
		}
	}
	const int ownedRows = static_cast<int>(owned.size()) * PickPitch;
	const int list = picking >= 0 ? ownedRows + 120 : 0;
	const int top = list + 40, w = SlotCount * CellPitch + 20, h = top + CellSize + 14;
	std::string out;
	const auto add = [&](const std::string& widget) { out += (out.empty() ? "" : ",") + widget; };
	const auto icon = [&](const std::string& name, int x, int y, int kind, bool bright, const std::string& action) {
		const double cap = s.Capacity(kind, o);
		add("{\"type\":\"ButtonWidget\",\"name\":\"" + name + "\",\"fields\":{\"rect\":" + RectJson({ x, y, CellSize, CellSize }) + ",\"filename\":\"" + Picture(kind, s, o) +
			"\",\"normalFrame\":0,\"pressedFrame\":0,\"hoveredFrame\":0,\"disabledFrame\":0" + (bright ? "" : ",\"tint\":{\"r\":0.35,\"g\":0.35,\"b\":0.35,\"a\":0.7}") +
			",\"tooltipString\":\"" + Name(kind, s, o) + ": " + Effect(kind, s, o) + ". Holds " + std::to_string(static_cast<int>(cap)) + " charges, a use takes " +
			std::to_string(static_cast<int>(UseCost)) + ".\",\"onClickMessage\":\"" + HudMessage + action + "\"}}");
		const int steps = std::clamp(static_cast<int>(s.Charge(kind) / cap * BarSteps + 1e-6), 0, BarSteps);
		const int bar = (CellSize - 4) * steps / BarSteps;
		add(Box(name + "Back", { x + CellSize + 2, y, 12, CellSize }, Dark));
		if (bar > 0) {
			add(Box(name + "Bar", { x + CellSize + 4, y + 2 + (CellSize - 4) - bar, 8, bar }, bright ? Gold : Dim));
		}
		add(Text(name + "Uses", { x + 50, y, 46, 28 }, std::to_string(s.Uses(kind)), bright ? TextWhite : "$FontColorRed", "$SmallFontSize", "right"));
	};
	if (picking >= 0) {
		const int x = picking * CellPitch;
		add(Box("ListEdge", { x, 0, CellPitch + 4, list }, Bronze));
		add(Box("ListBox", { x + 3, 3, CellPitch - 2, list - 6 }, Dark));
		for (size_t i = 0; i < owned.size(); ++i) {
			icon("Pick" + std::to_string(i), x + 8, 8 + static_cast<int>(i) * PickPitch, owned[i], true,
				"equip-" + std::to_string(picking + 1) + "-" + KindIds[owned[i]]);
		}
		add(Text("PickNone", { x + 8, 8 + ownedRows, CellSize, 40 }, "Empty", TextGrey, "$SmallFontSize"));
		add("{\"type\":\"ButtonWidget\",\"name\":\"PickNoneButton\",\"fields\":{\"rect\":" + RectJson({ x + 25, 48 + ownedRows, 64, 60 }) +
			",\"filename\":\"PANEL\\\\closebtn_4x\",\"hoveredFrame\":3,\"pressedFrame\":1,\"tooltipString\":\"Leave this slot empty\",\"onClickMessage\":\"" + HudMessage +
			"equip-" + std::to_string(picking + 1) + "-none\"}}");
	}
	for (int i = 0; i < SlotCount; ++i) {
		const int x = i * CellPitch + 4, kind = s.slots[static_cast<size_t>(i)];
		const std::string n = std::to_string(i + 1);
		add("{\"type\":\"ButtonWidget\",\"name\":\"Swap" + n + "\",\"fields\":{\"rect\":" + RectJson({ x + 31, list, 36, 36 }) +
			",\"filename\":\"PANEL\\\\HUD_02\\\\Belt_UpArrow\",\"normalFrame\":0,\"pressedFrame\":0,\"hoveredFrame\":0,\"disabledFrame\":0,\"tooltipString\":\"Choose the potion for this slot\",\"onClickMessage\":\"" +
			HudMessage + "pick-" + n + "\"}}");
		if (s.Owns(kind, o)) {
			icon("Slot" + n, x, top, kind, s.Ready(kind, o), "use-" + n);
		}
		add(Text("Key" + n, { x + 2, top + 66, 40, 32 }, n, TextGold, "$MediumFontSize", "left"));
	}
	// The game's belt starts 250 right of the middle and its cells 128 above the bottom (hudpanelhd).
	return std::string("{\"type\":\"Panel\",\"name\":\"") + Stash::PanelMark + "\",\"fields\":{\"rect\":" + RectJson({ 259, -128 - top, w, h }) +
		",\"anchor\":{\"x\":0.5,\"y\":1.0},\"priority\":8400},\"children\":[" + out + "]}";
}

// "Potions: Antidote at Crypt   Stamina at Pit" for an act; empty when it has none left.
inline auto AffixLine(const State& s, int act, const std::map<std::string, std::string>& labels) -> std::string {
	std::string line;
	for (const auto& [id, stopId] : s.affixes) {
		const Affix* affix = AffixById(id);
		const Route::Stop* stop = Route::StopById(stopId);
		if (affix != nullptr && stop != nullptr && Route::ActOfTier(stop->tier) == act) {
			const auto label = labels.find(stop->node);
			line += std::string(line.empty() ? "Potions: " : "   ") + affix->label + " at " + (label == labels.end() ? stopId : label->second);
		}
	}
	return line;
}

// A route map page with this hero's affixes drawn in: the potion's picture at the right edge of the
// stop's button, and a line above the legend.
inline auto WithAffixes(const std::string& page, int act, const State& s, const std::map<std::string, std::string>& labels) -> std::string {
	const std::string line = AffixLine(s, act, labels);
	const size_t end = page.rfind("]}");
	if (line.empty() || end == std::string::npos) {
		return page;
	}
	std::string more;
	for (const auto& [id, stopId] : s.affixes) {
		const Affix* affix = AffixById(id);
		const Route::Stop* stop = Route::StopById(stopId);
		if (affix == nullptr || stop == nullptr || Route::ActOfTier(stop->tier) != act) {
			continue;
		}
		const std::string mark = std::string("\"name\":\"") + stop->node + "\",\"fields\":{\"rect\":";
		const size_t at = page.find(mark);
		int x = 0, y = 0;
		if (at != std::string::npos && std::sscanf(page.c_str() + at + mark.size(), "{\"x\":%d,\"y\":%d", &x, &y) == 2) {
			more += std::string(",{\"type\":\"ImageWidget\",\"name\":\"Affix_") + id + "\",\"fields\":{\"rect\":{\"x\":" + std::to_string(x + 346) + ",\"y\":" + std::to_string(y + 18) +
				",\"scale\":0.46},\"filename\":\"items\\\\misc\\\\potion\\\\" + affix->picture + "\"}}";
		}
	}
	more += "," + Stash::Ui::Text("AffixLine", { 40, 1743, 1431, 34 }, line, "$FontColorLightPurple", "$SmallFontSize");
	return page.substr(0, end) + more + page.substr(end);
}

}  // namespace Potions
