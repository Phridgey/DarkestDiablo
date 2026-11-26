// Darkest Diablo — the route rules: which stops of the act a hero may enter (no game or Windows
// code here, so they can be tested anywhere; see tests/camp_tests.cpp).
//
// Each hero has their own progress. The act is a row of tiers (docs/design.md, "Act I"):
//   Den of Evil -> Blood Raven -> fork 1 -> Tristram -> fork 2 -> Andariel
// - A main-line stop (Den, Blood Raven, Tristram, Andariel) is cleared once; then the next tier
//   opens and this one locks.
// - A fork gives 2 picks. The same option may be picked twice (its one replay). After the second
//   pick the fork locks and the next tier opens.
// - Only the current tier can be entered.
// - Entering a stop starts a run of it, and the run must be finished before any other stop can
//   be entered: a fork pick is spent on entry. The run remembers how many waves have spawned, so
//   leaving (town portal, or quitting the game) and coming back resumes at the next wave instead
//   of starting over; nothing can be farmed by leaving. Clearing the stop ends the run. A fork
//   pick quit with nothing left to spawn (the last wave out) ends as given up: it counts as done,
//   with no reward. A main-line stop is never given up: it resumes with its last wave.
// On disk: camp/slot<N>/progress/<hero>.txt, one line per stop: "clear <stop id> <times>", and
// "run <stop id> <waves spawned>" for a run in progress, and
// "flag <name> <n>": one-off rewards already given (Hratli's socket, the Golden Bird's life).
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

inline constexpr int TierCount    = 25;  // Act I: tiers 0-5, II: 6-10, III: 11-14, IV: 15-18, V: 19-24
inline constexpr int PicksPerFork = 2;

inline constexpr Stop Stops[] = {
	{ "den",       "NodeDen",        0, 13, 13 },  // the Den arena (one room, level 13)
	{ "burial",    "NodeBurial",     1, 10,  9 },  // arrive in the entry room (10), fight on 9
	{ "inifuss",   "NodeInifuss",    2, 12, 11 },  // Rakanishu & Treehead: entry 12, fight on 11
	{ "mausoleum", "Fork1Mausoleum", 2, 14, 14 },  // crafting hunt: a round crypt hall (level 14)
	{ "crypt",     "Fork1Crypt",     2, 15, 15 },  // XP grind: the same round hall (level 15)
	{ "tristram",  "NodeTristram",   3, 38, 38 },  // the game's own Tristram; cleared by killing Griswold
	{ "pit",       "Fork2Pit",       4, 16, 16 },  // MF run: the real Pit level 2 as an arena
	{ "countess",  "Fork2Countess",  4, 25, 25 },  // crafting hunt: the game's Tower Cellar 5
	{ "malus",     "Fork2Malus",     4, 28, 28 },  // XP grind: a cut-down Barracks; the Smith ends it
	{ "andariel",  "NodeAndariel",   5, 37, 37 },  // her lair (Catacombs 4); andariel.cpp; cleared when she dies
	// Act II (docs/act2.md)
	{ "halls",     "NodeHalls",      6, 60, 60 },  // Halls of the Dead 3: cleared on picking up the Horadric Cube
	{ "sewers",    "Fork3Sewers",    7, 49, 49 },  // XP grind: Sewers 3, shortened, with Radament
	{ "stony",     "Fork3Stony",     7, 55, 55 },  // crafting hunt: Stony Tomb
	{ "maggot",    "NodeMaggot",     8, 64, 64 },  // tier 8: both forced stops, any order; the Staff of Kings
	{ "clawviper", "NodeClawViper",  8, 61, 61 },  // the Amulet of the Viper
	{ "arcane",    "Fork4Arcane",    9, 62, 62 },  // crafting hunt; the Summoner ends it (spare level 62: the real 74 crashed)
	{ "tunnels",   "Fork4Tunnels",   9, 65, 65 },  // MF run
	{ "canyon",    "Fork4Canyon",    9, 59, 59 },  // XP grind (on level 59; the real 46 is outdoors)
	{ "duriel",    "NodeDuriel",    10, 66, 73 },  // arrive at the orifice (66); Duriel dies in his lair (73)
	// Act III (docs/act3.md)
	{ "spider",    "NodeSpider",    11, 85, 85 },  // tier 11: all three, any order; Khalim's Eye
	{ "flayer",    "NodeFlayer",    11, 91, 91 },  // Khalim's Brain
	{ "ksewers",   "NodeKSewers",   11, 93, 93 },  // Khalim's Heart
	{ "lamesen",   "Fork5LamEsen",  12, 94, 94 },  // XP grind: the Ruined Temple; Lam Esen's Tome ends it
	{ "swampy",    "Fork5Swampy",   12, 90, 90 },  // MF run: one Swampy Pit room
	{ "lowerkurast", "Fork5LowerKurast", 12, 95, 95 },  // crafting hunt: Lower Kurast's streets (on level 95)
	{ "travincal", "NodeTravincal", 13, 96, 96 },  // Travincal (on level 96); smashing the Compelling Orb ends it
	{ "durance",   "NodeDurance",   14, 99, 99 },  // Durance of Hate 3 on spare level 99 (the real 102 crashed); Mephisto dies
	// Act IV (docs/act4.md)
	{ "plains",    "NodePlains",    15, 105, 105 },  // XP grind: Plains of Despair; Izual dies
	{ "abaddon",   "Fork6Abaddon",  16, 125, 125 },  // XP grind (an Act V red portal)
	{ "acheron",   "Fork6Acheron",  16, 126, 126 },  // MF run
	{ "infernal",  "Fork6Infernal", 16, 127, 127 },  // crafting hunt
	{ "hellforge", "NodeHellforge", 17, 107, 107 },  // the Hellforge (on level 107); Hephasto dies
	{ "chaos",     "NodeChaos",     18, 108, 108 },  // Chaos Sanctuary, full; Diablo dies
	// Act V (docs/act5.md)
	{ "shenk",     "NodeShenk",     19, 116, 116 },  // the Bloody Foothills (on level 116); Shenk dies
	{ "highlands", "Fork7Highlands", 20, 113, 113 },  // XP grind: the barbarians' pens (on level 113)
	{ "pindle",    "Fork7Pindle",   20, 121, 121 },  // MF run: Nihlathak's Temple entrance; Pindleskin
	{ "cellar",    "Fork7Cellar",   20, 119, 119 },  // crafting hunt: the Icy Cellar
	{ "anya",      "NodeAnya",      21, 114, 114 },  // the Frozen River; freeing Anya ends it
	{ "vaught",    "Fork8Vaught",   22, 124, 124 },  // MF run: the Halls of Vaught; Nihlathak dies
	{ "cows",      "Fork8Cows",     22, 39, 39 },  // crafting hunt: the Cow Level
	{ "ancients",  "NodeAncients",  23, 120, 120 },  // Arreat Summit; the three Ancients die
	{ "baal",      "NodeBaal",      24, 131, 132 },  // the Throne room's waves (131); Baal dies in the Worldstone Chamber (132)
};

inline constexpr auto IsFork(int tier) -> bool {
	return tier == 2 || tier == 4 || tier == 7 || tier == 9 || tier == 12 || tier == 16 || tier == 20 || tier == 22;
}

// A tier of several forced stops on one row (Act II: Maggot Lair and Claw Viper Temple; Act III:
// Khalim's three parts): each is cleared once, in any order, and the next tier opens when all of
// them are.
inline constexpr auto IsAllOf(int tier) -> bool {
	return tier == 8 || tier == 11;
}

// Which act a tier belongs to (1-based): its route map page.
inline constexpr auto ActOfTier(int tier) -> int {
	return tier < 6 ? 1 : tier < 11 ? 2 : tier < 15 ? 3 : tier < 19 ? 4 : 5;
}

// The town of an act (levels.txt Ids).
inline constexpr auto TownOfAct(int act) -> int {
	return act == 2 ? 40 : act == 3 ? 75 : act == 4 ? 103 : act == 5 ? 109 : 1;
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
	std::string running;                // the stop of the run in progress ("" none)
	int         runWaves = 0;           // its waves spawned so far (a stop without waves: 1 once entered)
	std::map<std::string, int> flags;   // one-off rewards already given ("hratli", "goldenbird"...)

	auto Flag(const std::string& name) const -> int {
		const auto it = flags.find(name);
		return it == flags.end() ? 0 : it->second;
	}

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
		if (IsAllOf(tier)) {
			for (const auto& stop : Stops) {
				if (stop.tier == tier && Clears(stop) == 0) {
					return false;
				}
			}
			return true;
		}
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

	// May the hero go there now? Only built stops of the current tier, and while a run is in
	// progress only its stop.
	auto CanEnter(const Stop& stop) const -> bool {
		return stop.travelLevel >= 0 && stop.tier == CurrentTier() && (running.empty() || running == stop.id) &&
			(!IsAllOf(stop.tier) || Clears(stop) == 0);
	}

	// The hero entered the stop: starts its run, or resumes the one in progress. Returns the
	// waves already spawned in that run (0 for a new run). Call only when CanEnter.
	auto StartRun(const Stop& stop) -> int {
		if (running != stop.id) {
			running = stop.id;
			runWaves = 0;
		}
		return runWaves;
	}

	// The run's waves spawned so far.
	void NoteWaves(const Stop& stop, int waves) {
		if (running == stop.id && waves > runWaves) {
			runWaves = waves;
		}
	}

	// A stop was cleared (or given up). Ignored unless it was enterable (so a replay never counts
	// twice). Ends its run.
	auto RecordClear(const Stop& stop) -> bool {
		if (!CanEnter(stop)) {
			return false;
		}
		++clears[stop.id];
		if (running == stop.id) {
			running.clear();
			runWaves = 0;
		}
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
		} else if (kind == "flag" && !id.empty() && times > 0) {
			progress.flags[id] = times;
		} else if (kind == "run" && StopById(id) != nullptr && times >= 0) {
			progress.running = id;
			progress.runWaves = times;
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
	for (const auto& [name, value] : progress.flags) {
		out << "flag " << name << ' ' << value << '\n';
	}
	if (!progress.running.empty()) {
		out << "run " << progress.running << ' ' << progress.runWaves << '\n';
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
