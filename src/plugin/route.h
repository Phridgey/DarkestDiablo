// Darkest Diablo — the route rules: which stops of the act a hero may enter (no game or Windows
// code here, so they can be tested anywhere; see tests/camp_tests.cpp).
//
// Each hero has their own progress. The act is a row of tiers (docs/design.md, "Act I"):
//   Den of Evil -> Blood Raven -> fork 1 -> Tristram -> fork 2 -> Andariel
// - A main-line stop (Den, Blood Raven, Tristram, Andariel) is cleared once; then the next tier
//   opens and this one locks.
// - A fork gives 2 picks. The same option may be picked twice (its one replay). After the second
//   pick the fork locks and the next tier opens.
// - Only the current tier can be entered. Leaving a stop before clearing it is always fine: it
//   stays open until it is cleared.
// On disk: camp/slot<N>/progress/<hero>.txt, one line per stop: "clear <stop id> <times>".
#pragma once

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <system_error>

namespace Route {

namespace fs = std::filesystem;

struct Stop {
	const char* id;          // stable name, used in progress files
	const char* node;        // its button on the route map (RouteMaphd.json)
	int         tier;
	int         travelLevel; // where the route map sends the player; -1 = not built yet
	int         arenaLevel;  // the level whose clear counts for this stop; -1 = not built yet
};

inline constexpr int TierCount    = 6;
inline constexpr int PicksPerFork = 2;

inline constexpr Stop Stops[] = {
	{ "den",       "NodeDen",        0, 13, 13 },  // the Den arena (one room, level 13)
	{ "burial",    "NodeBurial",     1, 10,  9 },  // arrive in the entry room (10), fight on 9
	{ "inifuss",   "NodeInifuss",    2, 12, 11 },  // Rakanishu & Treehead: entry 12, fight on 11
	{ "mausoleum", "Fork1Mausoleum", 2, 14, 14 },  // crafting hunt: a round crypt hall (level 14)
	{ "crypt",     "Fork1Crypt",     2, -1, -1 },
	{ "tristram",  "NodeTristram",   3, -1, -1 },
	{ "pit",       "Fork2Pit",       4, -1, -1 },
	{ "countess",  "Fork2Countess",  4, -1, -1 },
	{ "malus",     "Fork2Malus",     4, -1, -1 },
	{ "andariel",  "NodeAndariel",   5, -1, -1 },
};

inline constexpr auto IsFork(int tier) -> bool {
	return tier == 2 || tier == 4;
}

inline auto StopById(const std::string& id) -> const Stop* {
	for (const auto& stop : Stops) {
		if (id == stop.id) {
			return &stop;
		}
	}
	return nullptr;
}

inline auto StopByTravelLevel(int level) -> const Stop* {
	for (const auto& stop : Stops) {
		if (stop.travelLevel >= 0 && stop.travelLevel == level) {
			return &stop;
		}
	}
	return nullptr;
}

inline auto StopByArenaLevel(int level) -> const Stop* {
	for (const auto& stop : Stops) {
		if (stop.arenaLevel >= 0 && stop.arenaLevel == level) {
			return &stop;
		}
	}
	return nullptr;
}

struct Progress {
	std::map<std::string, int> clears;  // stop id -> times cleared

	auto Clears(const Stop& stop) const -> int {
		const auto it = clears.find(stop.id);
		return it == clears.end() ? 0 : it->second;
	}

	auto TierClears(int tier) const -> int {
		int total = 0;
		for (const auto& stop : Stops) {
			if (stop.tier == tier) {
				total += Clears(stop);
			}
		}
		return total;
	}

	auto TierDone(int tier) const -> bool {
		return TierClears(tier) >= (IsFork(tier) ? PicksPerFork : 1);
	}

	// The first tier not yet done; TierCount when the whole act is done.
	auto CurrentTier() const -> int {
		int tier = 0;
		while (tier < TierCount && TierDone(tier)) {
			++tier;
		}
		return tier;
	}

	// May the hero go there now? Only built stops of the current tier.
	auto CanEnter(const Stop& stop) const -> bool {
		return stop.travelLevel >= 0 && stop.tier == CurrentTier();
	}

	// A stop was cleared. Ignored unless it was enterable (so a replay never counts twice).
	auto RecordClear(const Stop& stop) -> bool {
		if (!CanEnter(stop)) {
			return false;
		}
		++clears[stop.id];
		return true;
	}
};

inline auto ProgressFile(const fs::path& live, int slot, const std::string& hero) -> fs::path {
	return live / "camp" / ("slot" + std::to_string(slot)) / "progress" / (hero + ".txt");
}

inline auto Load(const fs::path& file) -> Progress {
	Progress progress;
	std::ifstream in(file);
	std::string line;
	while (std::getline(in, line)) {
		std::istringstream words(line);
		std::string kind, id;
		int times = 0;
		if (words >> kind >> id >> times && kind == "clear" && StopById(id) != nullptr && times > 0) {
			progress.clears[id] = times;
		}
	}
	return progress;
}

inline auto Save(const fs::path& file, const Progress& progress) -> bool {
	std::error_code ec;
	fs::create_directories(file.parent_path(), ec);
	std::ostringstream out;
	for (const auto& [id, times] : progress.clears) {
		out << "clear " << id << ' ' << times << '\n';
	}
	const fs::path temp = file.string() + ".tmp";
	{
		std::ofstream f(temp, std::ios::binary | std::ios::trunc);
		f << out.str();
		if (!f) {
			return false;
		}
	}
	fs::rename(temp, file, ec);
	return !ec;
}

}
