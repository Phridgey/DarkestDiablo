// Darkest Diablo — the camp rules (no game or Windows code here, so they can be tested anywhere).
//
// A save slot is one camp: the heroes by the fire, the classes waiting to be recruited, and the
// classes whose hero fell. The rules:
// - A brand new camp offers every class. Picking one starts the camp; the others lock.
// - A traveler event offers up to 3 locked classes. The class of the last fallen hero is always
//   one of them. The picked class becomes a recruit: a new level 1 hero can be made of it.
// - One living hero per class. Death is final: the hero's files go to the graveyard, the class
//   locks again and is remembered as fallen.
// - If the fire is empty (every hero dead, no recruit waiting), a traveler turns up by himself.
//
// On disk, inside the mod's save folder (the "live" folder the game reads):
//   camp/active.txt              the slot that is live (1-3)
//   camp/slot<N>/camp.txt        that slot's camp
//   camp/slot<N>/files/          that slot's characters and shared stash while another slot is live
//   camp/graveyard/slot<N>/...   fallen heroes' files
// The game only reads one folder and one shared stash, so switching slot means moving files.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace Camp {

namespace fs = std::filesystem;

inline constexpr int ClassCount = 8;
inline constexpr int SlotCount  = 3;
inline constexpr int OfferSize  = 3;

// In charstats.txt order: the class number the game stores in a save.
inline constexpr const char* ClassNames[ClassCount] = {
	"Amazon", "Sorceress", "Necromancer", "Paladin", "Barbarian", "Druid", "Assassin", "Warlock",
};

inline auto ClassFromName(const std::string& name) -> int {
	for (int i = 0; i < ClassCount; ++i) {
		if (name == ClassNames[i]) {
			return i;
		}
	}
	return -1;
}

inline auto Lower(std::string text) -> std::string {
	for (auto& c : text) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return text;
}

struct Hero {
	std::string name;       // the save's file name without ".d2s"
	int         cls   = -1;
	int         level = 1;
};

struct State {
	bool              started = false;
	std::vector<Hero> heroes;    // alive
	std::vector<int>  recruits;  // unlocked, no hero made yet
	std::vector<int>  fallen;    // classes whose hero died and has not been recruited again; newest first
	std::vector<int>  offer;     // the traveler's current offer, kept until answered

	static auto Has(const std::vector<int>& list, int cls) -> bool {
		return std::find(list.begin(), list.end(), cls) != list.end();
	}
	static void Drop(std::vector<int>& list, int cls) {
		list.erase(std::remove(list.begin(), list.end(), cls), list.end());
	}
	auto HeroOf(int cls) const -> const Hero* {
		for (const auto& hero : heroes) {
			if (hero.cls == cls) {
				return &hero;
			}
		}
		return nullptr;
	}
	auto HeroNamed(const std::string& name) -> Hero* {
		for (auto& hero : heroes) {
			if (Lower(hero.name) == Lower(name)) {
				return &hero;
			}
		}
		return nullptr;
	}
	auto Locked(int cls) const -> bool {
		return HeroOf(cls) == nullptr && !Has(recruits, cls);
	}
	auto LockedClasses() const -> std::vector<int> {
		std::vector<int> out;
		for (int c = 0; c < ClassCount; ++c) {
			if (Locked(c)) {
				out.push_back(c);
			}
		}
		return out;
	}
};

// Up to 3 locked classes. The newest fallen class always comes first; the rest are random.
template <typename Rng>
auto RollOffer(const State& state, Rng& rng) -> std::vector<int> {
	std::vector<int> pool = state.LockedClasses();
	std::vector<int> offer;
	for (const int cls : state.fallen) {
		if (State::Has(pool, cls)) {
			offer.push_back(cls);
			State::Drop(pool, cls);
			break;
		}
	}
	std::shuffle(pool.begin(), pool.end(), rng);
	for (const int cls : pool) {
		if (static_cast<int>(offer.size()) >= OfferSize) {
			break;
		}
		offer.push_back(cls);
	}
	return offer;
}

// The classes the hero screen may make right now.
template <typename Rng>
auto Creatable(State& state, Rng& rng) -> std::vector<int> {
	if (!state.started) {
		std::vector<int> all;
		for (int c = 0; c < ClassCount; ++c) {
			all.push_back(c);
		}
		return all;
	}
	if (!state.recruits.empty()) {
		return state.recruits;
	}
	if (state.heroes.empty()) {  // the fire is empty: a traveler turns up
		if (state.offer.empty()) {
			state.offer = RollOffer(state, rng);
		}
		return state.offer;
	}
	return {};
}

// The traveler's offer was answered with `cls`. Returns false if it was not on offer.
inline auto Recruit(State& state, int cls) -> bool {
	if (!State::Has(state.offer, cls) || !state.Locked(cls)) {
		return false;
	}
	state.recruits.push_back(cls);
	State::Drop(state.fallen, cls);
	state.offer.clear();
	return true;
}

// A new hero of `cls` exists (its save file appeared).
inline void Adopt(State& state, const std::string& name, int cls, int level) {
	state.started = true;
	State::Drop(state.recruits, cls);
	State::Drop(state.fallen, cls);
	state.offer.clear();
	state.heroes.push_back({ name, cls, level });
}

inline void MarkFallen(State& state, const std::string& name) {
	for (auto it = state.heroes.begin(); it != state.heroes.end(); ++it) {
		if (Lower(it->name) == Lower(name)) {
			const int cls = it->cls;
			state.heroes.erase(it);
			State::Drop(state.fallen, cls);
			state.fallen.insert(state.fallen.begin(), cls);
			state.offer.clear();
			return;
		}
	}
}

// ---- camp.txt ----------------------------------------------------------------------------
// One fact per line: "started 1", "hero <Class> <level> <name>", "recruit <Class>",
// "fallen <Class>", "offer <Class>". Unknown lines are ignored.

inline auto Serialize(const State& state) -> std::string {
	std::ostringstream out;
	out << "version 1\n" << "started " << (state.started ? 1 : 0) << "\n";
	for (const auto& hero : state.heroes) {
		out << "hero " << ClassNames[hero.cls] << " " << hero.level << " " << hero.name << "\n";
	}
	for (const int c : state.recruits) { out << "recruit " << ClassNames[c] << "\n"; }
	for (const int c : state.fallen)   { out << "fallen " << ClassNames[c] << "\n"; }
	for (const int c : state.offer)    { out << "offer " << ClassNames[c] << "\n"; }
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
		if (key == "started") {
			state.started = value == "1";
		} else if (key == "hero") {
			Hero hero;
			hero.cls = ClassFromName(value);
			words >> hero.level >> hero.name;
			if (hero.cls >= 0 && !hero.name.empty() && state.HeroOf(hero.cls) == nullptr) {
				hero.level = std::clamp(hero.level, 1, 99);
				state.heroes.push_back(hero);
			}
		} else if (key == "recruit" || key == "fallen" || key == "offer") {
			const int cls = ClassFromName(value);
			auto& list = key == "recruit" ? state.recruits : key == "fallen" ? state.fallen : state.offer;
			if (cls >= 0 && !State::Has(list, cls)) {
				list.push_back(cls);
			}
		}
	}
	return state;
}

inline auto ReadFile(const fs::path& path) -> std::string {
	std::ifstream in(path, std::ios::binary);
	std::ostringstream out;
	out << in.rdbuf();
	return out.str();
}

// Writes through a temporary file, so a crash never leaves half a camp file.
inline auto WriteFile(const fs::path& path, const std::string& text) -> bool {
	std::error_code ec;
	fs::create_directories(path.parent_path(), ec);
	const fs::path temp = path.string() + ".new";
	{
		std::ofstream out(temp, std::ios::binary | std::ios::trunc);
		out << text;
		if (!out.good()) {
			return false;
		}
	}
	fs::rename(temp, path, ec);
	if (ec) {  // Windows: rename over an existing file can fail
		fs::remove(path, ec);
		fs::rename(temp, path, ec);
	}
	return !ec;
}

inline auto SlotDir(const fs::path& live, int slot) -> fs::path {
	return live / "camp" / ("slot" + std::to_string(slot));
}
inline auto LoadState(const fs::path& live, int slot) -> State {
	return Parse(ReadFile(SlotDir(live, slot) / "camp.txt"));
}
inline auto SaveState(const fs::path& live, int slot, const State& state) -> bool {
	return WriteFile(SlotDir(live, slot) / "camp.txt", Serialize(state));
}
inline auto ActiveSlot(const fs::path& live) -> int {
	const int slot = std::atoi(ReadFile(live / "camp" / "active.txt").c_str());
	return slot >= 1 && slot <= SlotCount ? slot : 1;
}

// One line for the save-slot screen, e.g. "Paladin 12, Sorceress 1 - 1 recruit waiting".
inline auto Summary(const State& state) -> std::string {
	if (!state.started) {
		return "Empty - start a new camp";
	}
	std::string out;
	for (const auto& hero : state.heroes) {
		out += (out.empty() ? "" : ", ") + std::string(ClassNames[hero.cls]) + " " + std::to_string(hero.level);
	}
	if (out.empty()) {
		out = "The fire is empty";
	}
	if (!state.recruits.empty()) {
		out += " - " + std::to_string(state.recruits.size()) + (state.recruits.size() == 1 ? " recruit waiting" : " recruits waiting");
	}
	return out;
}

// ---- Save files --------------------------------------------------------------------------

// Files that stay in the live folder whatever slot is live (game settings, loot filters).
inline auto IsSharedFile(const fs::path& file) -> bool {
	const std::string name = Lower(file.filename().string());
	const std::string ext  = Lower(file.extension().string());
	return name == "settings.json" || name == "lootfilter.json" || ext == ".fltr";
}

// The files of the live slot: everything at the top of the live folder except shared files.
inline auto SlotFiles(const fs::path& live) -> std::vector<fs::path> {
	std::vector<fs::path> out;
	std::error_code ec;
	for (const auto& entry : fs::directory_iterator(live, ec)) {
		if (entry.is_regular_file(ec) && !IsSharedFile(entry.path())) {
			out.push_back(entry.path());
		}
	}
	return out;
}

inline auto MoveAll(const std::vector<fs::path>& files, const fs::path& to, std::string& error) -> bool {
	std::error_code ec;
	fs::create_directories(to, ec);
	std::vector<std::pair<fs::path, fs::path>> done;
	for (const auto& file : files) {
		const fs::path target = to / file.filename();
		if (fs::exists(target, ec)) {
			error = "already there: " + target.string();
		} else {
			fs::rename(file, target, ec);
			if (ec) {
				error = "could not move " + file.string() + ": " + ec.message();
			}
		}
		if (!error.empty()) {  // put back what was moved, so no slot is ever half moved
			for (const auto& [from, moved] : done) {
				fs::rename(moved, from, ec);
			}
			return false;
		}
		done.emplace_back(file, target);
	}
	return true;
}

// Makes slot `to` live: parks the live files under the old slot, brings the new slot's files in.
// Call it only while no character is loaded (main menu). On failure nothing has changed.
inline auto SwitchSlot(const fs::path& live, int to, std::string& error) -> bool {
	const int from = ActiveSlot(live);
	if (to < 1 || to > SlotCount) {
		error = "no such slot";
		return false;
	}
	if (to == from) {
		return true;
	}
	const fs::path parkOld = SlotDir(live, from) / "files";
	const fs::path parkNew = SlotDir(live, to) / "files";
	const auto liveFiles = SlotFiles(live);
	if (!MoveAll(liveFiles, parkOld, error)) {
		return false;
	}
	std::vector<fs::path> incoming;
	std::error_code ec;
	for (const auto& entry : fs::directory_iterator(parkNew, ec)) {
		if (entry.is_regular_file(ec)) {
			incoming.push_back(entry.path());
		}
	}
	if (!MoveAll(incoming, live, error)) {
		std::vector<fs::path> parked;
		for (const auto& file : liveFiles) {
			parked.push_back(parkOld / file.filename());
		}
		std::string ignored;
		MoveAll(parked, live, ignored);
		return false;
	}
	if (!WriteFile(live / "camp" / "active.txt", std::to_string(to) + "\n")) {
		error = "could not write camp/active.txt";
		return false;
	}
	return true;
}

// What a save file says about its character. Only trusted when it looks right (`ok`).
struct SaveInfo {
	bool ok       = false;
	int  cls      = -1;
	int  level    = 1;
	bool hardcore = false;
	bool dead     = false;
};

inline auto ReadSave(const fs::path& file) -> SaveInfo {
	SaveInfo info;
	std::ifstream in(file, std::ios::binary);
	unsigned char head[48] {};
	in.read(reinterpret_cast<char*>(head), sizeof(head));
	if (in.gcount() < static_cast<std::streamsize>(sizeof(head))) {
		return info;
	}
	const uint32_t magic = head[0] | (head[1] << 8) | (head[2] << 16) | (static_cast<uint32_t>(head[3]) << 24);
	info.cls      = head[40];
	info.level    = head[43];
	info.hardcore = (head[36] & 0x04) != 0;
	info.dead     = (head[36] & 0x08) != 0;
	info.ok       = magic == 0xAA55AA55U && info.cls < ClassCount && info.level >= 1 && info.level <= 99;
	return info;
}

inline auto HeroNames(const fs::path& live) -> std::vector<std::string> {
	std::vector<std::string> out;
	std::error_code ec;
	for (const auto& entry : fs::directory_iterator(live, ec)) {
		if (entry.is_regular_file(ec) && Lower(entry.path().extension().string()) == ".d2s") {
			out.push_back(entry.path().stem().string());
		}
	}
	std::sort(out.begin(), out.end());
	return out;
}

// Moves every file of hero `name` out of the live folder into the graveyard.
inline auto Bury(const fs::path& live, int slot, const std::string& name, std::string& error) -> bool {
	std::vector<fs::path> files;
	std::error_code ec;
	for (const auto& entry : fs::directory_iterator(live, ec)) {
		if (entry.is_regular_file(ec) && !IsSharedFile(entry.path()) && Lower(entry.path().stem().string()) == Lower(name)) {
			files.push_back(entry.path());
		}
	}
	char stamp[32] {};
	const std::time_t now = std::time(nullptr);
	std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
	return MoveAll(files, live / "camp" / "graveyard" / ("slot" + std::to_string(slot)) / (std::string(stamp) + "-" + name), error);
}

struct Hints {
	int         newClass  = -1;  // the class picked on the hero screen, for a save that just appeared
	std::string diedName;        // the hero the game reported dead this session, if any
	std::string leveledName;     // the hero that was played ...
	int         leveledTo = 0;   // ... and the level it reached (0 = no news)
};

// Brings the camp in line with the save files in the live folder. Returns what happened, one
// line each, for the log.
inline auto Reconcile(const fs::path& live, int slot, State& state, const Hints& hints) -> std::vector<std::string> {
	std::vector<std::string> log;
	const auto names = HeroNames(live);
	const auto onDisk = [&](const std::string& name) {
		return std::any_of(names.begin(), names.end(), [&](const std::string& n) { return Lower(n) == Lower(name); });
	};
	// Heroes whose save is gone (deleted in the menu): they leave the camp; the class locks.
	for (auto it = state.heroes.begin(); it != state.heroes.end();) {
		if (!onDisk(it->name)) {
			log.push_back(it->name + " (" + ClassNames[it->cls] + ") has no save any more; removed from the camp.");
			it = state.heroes.erase(it);
		} else {
			++it;
		}
	}
	// Saves the camp doesn't know: a hero that was just made.
	for (const auto& name : names) {
		if (state.HeroNamed(name) != nullptr) {
			continue;
		}
		const SaveInfo info = ReadSave(live / (name + ".d2s"));
		const int cls = hints.newClass >= 0 ? hints.newClass : info.ok ? info.cls : -1;
		if (cls < 0) {
			log.push_back(name + ": unknown class; left out of the camp.");
		} else if (state.HeroOf(cls) != nullptr) {
			log.push_back(name + ": the camp already has a " + ClassNames[cls] + "; left out of the camp.");
		} else {
			Adopt(state, name, cls, info.ok && info.cls == cls ? info.level : 1);
			log.push_back(name + " the " + ClassNames[cls] + " joins the camp.");
		}
	}
	// Levels and deaths.
	for (const auto& hero : std::vector<Hero>(state.heroes)) {
		const SaveInfo info = ReadSave(live / (hero.name + ".d2s"));
		const bool fileAgrees = info.ok && info.cls == hero.cls;  // the save layout is the one we know
		Hero* live_hero = state.HeroNamed(hero.name);
		if (fileAgrees) {
			live_hero->level = info.level;
		}
		if (Lower(hints.leveledName) == Lower(hero.name) && hints.leveledTo > live_hero->level) {
			live_hero->level = hints.leveledTo;
		}
		const bool died = Lower(hints.diedName) == Lower(hero.name) || (fileAgrees && info.hardcore && info.dead);
		if (died) {
			std::string error;
			if (Bury(live, slot, hero.name, error)) {
				MarkFallen(state, hero.name);
				log.push_back(hero.name + " the " + ClassNames[hero.cls] + " has fallen. The class is locked again.");
			} else {
				log.push_back(hero.name + ": could not move the fallen hero's files (" + error + ").");
			}
		}
	}
	return log;
}

}
