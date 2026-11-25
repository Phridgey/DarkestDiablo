// Darkest Diablo — Ascend: what an ascended hero leaves to the camp, on its way to a temporary
// stash page. No game code; tested in tests/camp_tests.cpp. See docs/ascend.md.
//
//   camp/slot<N>/ascend.txt          "fight <hero>" from stepping into the arena, "won <hero>" from
//                                    the boss's death. Read when no game runs: a fight left in any
//                                    way is a death, a won one is an ascension.
//   camp/slot<N>/ascended/<id>.d2s   the ascended hero's save, kept for good
//   camp/slot<N>/ascended.txt        "legacy <id> <pages sent> <done> <carrier or -> <carried page> <name>"
//                                    "tab <0-3> <name>"
//
// One page of items at a time is put into the save of the hero about to be loaded (the carrier);
// in the game the plugin moves it to the tab. If the carrier is gone before that, it is sent again.
#pragma once

#include "ascend.h"
#include "camp.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace Legacy {

namespace fs = std::filesystem;

inline constexpr int TabCount = 4;       // stash pages kept for ascended heroes
inline constexpr int ShownTabs = 3;      // of them in use at once: what fits beside Shared and Upgrades
inline constexpr int PageSize = 10;      // cells, across and down

struct Estate {
	std::string id, name, carrier;  // carrier: the hero whose save holds the next page now
	int  sent = 0;
	int  tab = -1;                  // where the carried page goes
	bool done = false;
};

struct State {
	std::vector<Estate> estates;
	std::map<int, std::string> tabs;  // in use: number -> its hero
	auto FreeTab() const -> int {
		for (int i = 0; i < ShownTabs; ++i) {
			if (tabs.count(i) == 0) {
				return i;
			}
		}
		return -1;
	}
	auto CarriedBy(const std::string& hero) -> Estate* {
		for (Estate& e : estates) {
			if (!e.carrier.empty() && Camp::Lower(e.carrier) == Camp::Lower(hero)) {
				return &e;
			}
		}
		return nullptr;
	}
};

inline auto StateFile(const fs::path& live, int slot) -> fs::path { return Camp::SlotDir(live, slot) / "ascended.txt"; }
inline auto FightFile(const fs::path& live, int slot) -> fs::path { return Camp::SlotDir(live, slot) / "ascend.txt"; }
inline auto EstateDir(const fs::path& live, int slot) -> fs::path { return Camp::SlotDir(live, slot) / "ascended"; }

inline auto Load(const fs::path& live, int slot) -> State {
	State state;
	std::istringstream in(Camp::ReadFile(StateFile(live, slot)));
	std::string line;
	while (std::getline(in, line)) {
		std::istringstream words(line);
		std::string key;
		words >> key;
		if (key == "legacy") {
			Estate e;
			int done = 0;
			if (words >> e.id >> e.sent >> done >> e.carrier >> e.tab >> e.name) {
				e.done = done != 0;
				if (e.carrier == "-") {
					e.carrier.clear();
				}
				state.estates.push_back(e);
			}
		} else if (key == "tab") {
			int n = -1;
			std::string name;
			if (words >> n >> name && n >= 0 && n < TabCount) {
				state.tabs[n] = name;
			}
		}
	}
	return state;
}

inline auto Save(const fs::path& live, int slot, const State& state) -> bool {
	std::ostringstream out;
	for (const Estate& e : state.estates) {
		out << "legacy " << e.id << " " << e.sent << " " << (e.done ? 1 : 0) << " " << (e.carrier.empty() ? "-" : e.carrier) << " " << e.tab << " " << e.name << "\n";
	}
	for (const auto& [n, name] : state.tabs) {
		out << "tab " << n << " " << name << "\n";
	}
	return Camp::WriteFile(StateFile(live, slot), out.str());
}

// ---- The fight ------------------------------------------------------------------------------
enum class Fight { None, Fighting, Won };

inline auto ReadFight(const fs::path& live, int slot, std::string& hero) -> Fight {
	std::istringstream in(Camp::ReadFile(FightFile(live, slot)));
	std::string key;
	in >> key >> hero;
	return hero.empty() ? Fight::None : key == "won" ? Fight::Won : key == "fight" ? Fight::Fighting : Fight::None;
}

inline auto WriteFight(const fs::path& live, int slot, Fight fight, const std::string& hero) -> bool {
	if (fight == Fight::None) {
		std::error_code ec;
		fs::remove(FightFile(live, slot), ec);
		return !ec;
	}
	return Camp::WriteFile(FightFile(live, slot), std::string(fight == Fight::Won ? "won " : "fight ") + hero + "\n");
}

// Keeps a copy of a hero's save as its legacy. The caller buries the hero only when this worked.
inline auto Keep(const fs::path& live, int slot, const std::string& hero, const std::string& stamp, std::string& error) -> bool {
	std::error_code ec;
	const std::string id = stamp + "-" + hero;
	fs::create_directories(EstateDir(live, slot), ec);
	fs::copy_file(live / (hero + ".d2s"), EstateDir(live, slot) / (id + ".d2s"), fs::copy_options::overwrite_existing, ec);
	if (ec) {
		error = ec.message();
		return false;
	}
	State state = Load(live, slot);
	Estate e;
	e.id = id, e.name = hero;
	state.estates.push_back(e);
	return Save(live, slot, state);
}

inline auto ReadBytes(const fs::path& file) -> Ascend::Bytes {
	std::ifstream in(file, std::ios::binary);
	return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

inline auto WriteBytes(const fs::path& file, const Ascend::Bytes& bytes) -> bool {
	const fs::path part = fs::path(file).concat(".part");
	{
		std::ofstream out(part, std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		if (!out.flush()) {
			return false;
		}
	}
	std::error_code ec;
	fs::rename(part, file, ec);
	return !ec;
}

// Before `hero` is loaded: puts the next waiting page in its save. Returns a line for the log
// ("" when there was nothing to do). `living`: the camp's heroes.
inline auto Send(const fs::path& live, int slot, const std::string& hero, const std::vector<std::string>& living, const Ascend::Shapes& shapes) -> std::string {
	State state = Load(live, slot);
	bool changed = false;
	for (Estate& e : state.estates) {  // a carrier that died with the page: send it again
		const bool alive = std::any_of(living.begin(), living.end(), [&](const std::string& n) { return Camp::Lower(n) == Camp::Lower(e.carrier); });
		if (!e.carrier.empty() && !alive) {
			e.carrier.clear();
			changed = true;
		}
		// Noted as carried, but nothing is in the carrier's save (saves put back from a backup): it
		// goes again now, unless it did reach its page.
		Ascend::Hero carrier;
		std::string unread;
		if (!e.carrier.empty() && Ascend::ReadHero(ReadBytes(live / (e.carrier + ".d2s")), shapes, carrier, unread) &&
			std::none_of(carrier.items.begin(), carrier.items.end(), [](const Ascend::Item& i) { return i.loc == 0 && i.page == 5; })) {
			const auto tab = state.tabs.find(e.tab);
			e.sent += tab != state.tabs.end() && tab->second == e.name ? 1 : 0;
			e.carrier.clear();
			changed = true;
		}
	}
	std::string note;
	if (state.CarriedBy(hero) == nullptr) {
		for (Estate& e : state.estates) {
			const int tab = state.FreeTab();
			const bool reserved = std::any_of(state.estates.begin(), state.estates.end(), [&](const Estate& o) { return !o.carrier.empty() && o.tab == tab; });
			if (e.done || !e.carrier.empty() || tab < 0 || reserved) {
				continue;
			}
			std::string error;
			Ascend::Hero from;
			if (!Ascend::ReadHero(ReadBytes(EstateDir(live, slot) / (e.id + ".d2s")), shapes, from, error)) {
				note = e.name + "'s items can't be read (" + error + "); the save is kept in ascended/.";
				continue;
			}
			std::vector<Ascend::Item> items = Ascend::Legacy(from), page;
			std::vector<int> pageOf;
			const int pages = Ascend::Pack(items, PageSize, pageOf);
			for (size_t i = 0; i < items.size(); ++i) {
				if (pageOf[i] == e.sent) {
					page.push_back(items[i]);
				}
			}
			if (pages < 0 || page.empty()) {
				e.done = true;
				changed = true;
				continue;
			}
			const fs::path file = live / (hero + ".d2s");
			const Ascend::Bytes before = ReadBytes(file);
			Ascend::Bytes bytes = before;
			if (!Ascend::Inject(bytes, page, shapes, error)) {
				note = e.name + "'s items wait: " + hero + "'s save can't take them (" + error + ").";
				break;
			}
			e.carrier = hero, e.tab = tab;
			// Noted first: a page noted but not written is found missing in the game and sent again.
			if (!WriteBytes(EstateDir(live, slot) / (hero + ".before.d2s"), before) || !Save(live, slot, state) || !WriteBytes(file, bytes)) {
				e.carrier.clear();
				note = "could not write " + hero + "'s save; nothing was sent.";
				changed = true;
				break;
			}
			return hero + " carries " + std::to_string(page.size()) + " of " + e.name + "'s items (page " + std::to_string(e.sent + 1) + " of " + std::to_string(pages) + ").";
		}
	}
	if (changed) {
		Save(live, slot, state);
	}
	return note;
}

// The carried page stands in its tab.
inline void Delivered(State& state, const std::string& hero) {
	if (Estate* e = state.CarriedBy(hero)) {
		state.tabs[e->tab] = e->name;
		++e->sent;
		e->carrier.clear();
	}
}

// ---- Turning pages ----------------------------------------------------------------------------
// The pages a player can turn to (from 1): the open ones, then the tabs in use behind the locked
// pages. Returns how many presses of the arrow lead from `page` to the next one; 0 = none.
inline auto Presses(int page, int direction, int lastOpen, int firstTabPage, const State& state) -> int {
	std::vector<int> pages;
	for (int p = 1; p <= lastOpen; ++p) {
		pages.push_back(p);
	}
	for (const auto& [n, name] : state.tabs) {
		pages.push_back(firstTabPage + n);
	}
	int best = 0;
	for (const int p : pages) {
		const int steps = (p - page) * direction;
		if (steps > 0 && (best == 0 || steps < best)) {
			best = steps;
		}
	}
	return best;
}

// Which of `now` don't belong on a tab: everything not there at the last look. `known` loses what
// was taken out, so nothing taken out can be put back.
template <class Id>
auto Strangers(std::vector<Id>& known, const std::vector<Id>& now) -> std::vector<Id> {
	std::vector<Id> strangers;
	for (const Id& id : now) {
		if (std::find(known.begin(), known.end(), id) == known.end()) {
			strangers.push_back(id);
		}
	}
	std::erase_if(known, [&](const Id& id) { return std::find(now.begin(), now.end(), id) == now.end(); });
	return strangers;
}

}
