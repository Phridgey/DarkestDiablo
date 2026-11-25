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
//   camp/next.txt                a save change that waits for the next start (Pending below)
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
#include <iterator>
#include <random>
#include <sstream>
#include <utility>
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
	std::string gear;       // what it wore when last seen: "<body spot>:<item code>" words, e.g. "4:hax 5:buc"
};

struct State {
	bool              started = false;
	int               difficulty = 0;  // the hardest difficulty played in this save: 0 Normal, 1 Nightmare, 2 Hell
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
	state.heroes.push_back({ name, cls, level, {} });
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
// One fact per line: "started 1", "difficulty <0-2>", "hero <Class> <level> <name>", "recruit <Class>",
// "fallen <Class>", "offer <Class>", "gear <Class> <spot>:<code> ..." (after its hero line).
// Unknown lines are ignored.

inline auto Serialize(const State& state) -> std::string {
	std::ostringstream out;
	out << "version 1\n" << "started " << (state.started ? 1 : 0) << "\n";
	if (state.difficulty > 0) {
		out << "difficulty " << state.difficulty << "\n";
	}
	for (const auto& hero : state.heroes) {
		out << "hero " << ClassNames[hero.cls] << " " << hero.level << " " << hero.name << "\n";
	}
	for (const auto& hero : state.heroes) {
		if (!hero.gear.empty()) {
			out << "gear " << ClassNames[hero.cls] << " " << hero.gear << "\n";
		}
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
		} else if (key == "difficulty") {
			state.difficulty = std::clamp(std::atoi(value.c_str()), 0, 2);
		} else if (key == "hero") {
			Hero hero;
			hero.cls = ClassFromName(value);
			words >> hero.level >> hero.name;
			if (hero.cls >= 0 && !hero.name.empty() && state.HeroOf(hero.cls) == nullptr) {
				hero.level = std::clamp(hero.level, 1, 99);
				state.heroes.push_back(hero);
			}
		} else if (key == "gear") {
			std::string rest;
			std::getline(words, rest);
			const size_t start = rest.find_first_not_of(' ');
			for (auto& hero : state.heroes) {
				if (hero.cls == ClassFromName(value) && start != std::string::npos) {
					hero.gear = rest.substr(start);
				}
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

inline constexpr const char* DifficultyNames[3] = { "Normal", "Nightmare", "Hell" };

// The words written into one Delve on the save slot screen (tools/camp_layouts.py), as
// (mark, text) pairs: HERO1-8 "<level> <Class>", RECRUITS, DIFF and its colour, BUTTON.
inline auto DelveText(const State& state) -> std::vector<std::pair<std::string, std::string>> {
	std::vector<std::pair<std::string, std::string>> out;
	for (size_t i = 0; i < ClassCount; ++i) {
		out.emplace_back("HERO" + std::to_string(i + 1),
			state.started && i < state.heroes.size()
				? std::to_string(state.heroes[i].level) + " " + ClassNames[state.heroes[i].cls] : std::string());
	}
	const size_t waiting = state.started ? state.recruits.size() : 0;
	out.emplace_back("RECRUITS", waiting == 0 ? std::string() : std::to_string(waiting) + (waiting == 1 ? " recruit waiting" : " recruits waiting"));
	const int d = std::clamp(state.difficulty, 0, 2);
	out.emplace_back("DIFFCOLOR", d == 2 ? "$FontColorRed" : d == 1 ? "$FontColorGoldYellow" : "$LightButtonTextColor");
	out.emplace_back("DIFF", state.started ? DifficultyNames[d] : "");
	out.emplace_back("BUTTON", state.started ? "Continue" : "Start New");
	return out;
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

// Once the three saves' stashes are one file (stashfile.h), it stays in the live folder.
inline constexpr const char* StashName = "SharedStashSoftCoreV2.d2i";
inline auto JoinedFile(const fs::path& live) -> fs::path { return live / "camp" / "stash-joined.txt"; }
inline auto StashJoined(const fs::path& live) -> bool { return ReadFile(JoinedFile(live)).rfind("done", 0) == 0; }

// The files of the live slot: everything at the top of the live folder except shared files.
inline auto SlotFiles(const fs::path& live) -> std::vector<fs::path> {
	std::vector<fs::path> out;
	std::error_code ec;
	const bool joined = StashJoined(live);
	for (const auto& entry : fs::directory_iterator(live, ec)) {
		if (entry.is_regular_file(ec) && !IsSharedFile(entry.path()) && !(joined && entry.path().filename() == StashName)) {
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
	const bool joined = StashJoined(live);
	for (const auto& entry : fs::directory_iterator(parkNew, ec)) {
		if (entry.is_regular_file(ec) && !(joined && entry.path().filename() == StashName)) {
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

// Empties a slot: its heroes, stash, camp and graveyard. Nothing is erased; it all moves to
// camp/trash/slot<N>-<stamp>/ so a save deleted by mistake can still be put back by hand.
// Works for the live slot too (its files are in the live folder). Call it at the menu only.
inline auto DeleteSlot(const fs::path& live, int slot, const std::string& stamp, std::string& error) -> bool {
	if (slot < 1 || slot > SlotCount) {
		error = "no such slot";
		return false;
	}
	const fs::path trash = live / "camp" / "trash" / ("slot" + std::to_string(slot) + "-" + stamp);
	std::error_code ec;
	if (fs::exists(trash, ec)) {
		error = "already there: " + trash.string();
		return false;
	}
	fs::create_directories(trash, ec);
	if (slot == ActiveSlot(live) && !MoveAll(SlotFiles(live), trash / "live", error)) {
		return false;
	}
	if (fs::exists(SlotDir(live, slot), ec)) {
		fs::rename(SlotDir(live, slot), trash / "camp", ec);
		if (ec) {
			error = "could not move " + SlotDir(live, slot).string() + ": " + ec.message();
			return false;
		}
	}
	const fs::path graves = live / "camp" / "graveyard" / ("slot" + std::to_string(slot));
	if (fs::exists(graves, ec)) {
		fs::rename(graves, trash / "graveyard", ec);  // if this fails the old graves stay: harmless
	}
	return true;
}

// ---- Changes that wait for the next start -------------------------------------------------
// The game reads the shared stash once, at the first game of a session, and keeps that copy in
// memory until it closes: every later game shows it and writes it back on leaving. Moving another
// save's stash file in while it runs is not enough (seen 2026-11-19: save 2's stash item showed up
// in save 1 and was written into save 1's stash). So once a game has been played, a change of
// save, or deleting the save whose stash the game holds, is only noted in camp/next.txt; the game
// restarts and the plugin makes the change while starting, before the game reads anything.
//   camp/next.txt   "delete <slot> <stamp>" lines, then "open <slot>"

struct Pending {
	std::vector<std::pair<int, std::string>> deletes;  // slot, trash stamp
	int open = 0;                                      // 0 = keep the live slot
	auto Empty() const -> bool { return deletes.empty() && open == 0; }
};

inline auto PendingFile(const fs::path& live) -> fs::path {
	return live / "camp" / "next.txt";
}

inline auto ReadPending(const fs::path& live) -> Pending {
	Pending pending;
	std::istringstream in(ReadFile(PendingFile(live)));
	std::string line;
	while (std::getline(in, line)) {
		std::istringstream words(line);
		std::string key;
		int slot = 0;
		words >> key >> slot;
		if (slot < 1 || slot > SlotCount) {
			continue;
		}
		if (key == "delete") {
			std::string stamp;
			words >> stamp;
			pending.deletes.emplace_back(slot, stamp.empty() ? std::string("pending") : stamp);
		} else if (key == "open") {
			pending.open = slot;
		}
	}
	return pending;
}

inline auto WritePending(const fs::path& live, const Pending& pending) -> bool {
	std::ostringstream out;
	for (const auto& [slot, stamp] : pending.deletes) {
		out << "delete " << slot << " " << stamp << "\n";
	}
	if (pending.open != 0) {
		out << "open " << pending.open << "\n";
	}
	return WriteFile(PendingFile(live), out.str());
}

// Makes the waiting changes. Call it while the game starts, before it reads the save folder.
// Returns what happened, one line each, for the log. The file is removed even if a change
// failed, so a broken change can't stop every start; the log says what went wrong.
inline auto ApplyPending(const fs::path& live) -> std::vector<std::string> {
	std::vector<std::string> log;
	std::error_code ec;
	if (!fs::exists(PendingFile(live), ec)) {
		return log;
	}
	const Pending pending = ReadPending(live);
	for (const auto& [slot, stamp] : pending.deletes) {
		std::string error;
		log.push_back(DeleteSlot(live, slot, stamp, error)
			? "save " + std::to_string(slot) + " was deleted (its files are kept in camp/trash)."
			: "could not delete save " + std::to_string(slot) + ": " + error);
	}
	if (pending.open != 0) {
		std::string error;
		log.push_back(SwitchSlot(live, pending.open, error)
			? "switched to save " + std::to_string(pending.open) + "."
			: "could not switch to save " + std::to_string(pending.open) + ": " + error);
	}
	fs::remove(PendingFile(live), ec);
	return log;
}

// ---- The bench ----------------------------------------------------------------------------
// The game's Play button loads the hero that is selected in its own list. To load the hero that
// was clicked at the campfire, every other hero's files wait in camp/slot<N>/bench/ while the
// menu loads, so the list holds one hero only. They come back before the camp is looked at again.

inline auto BenchDir(const fs::path& live, int slot) -> fs::path {
	return SlotDir(live, slot) / "bench";
}

inline auto HeroFiles(const fs::path& dir, const std::string& name) -> std::vector<fs::path> {
	std::vector<fs::path> files;
	std::error_code ec;
	for (const auto& entry : fs::directory_iterator(dir, ec)) {
		if (entry.is_regular_file(ec) && !IsSharedFile(entry.path()) && Lower(entry.path().stem().string()) == Lower(name)) {
			files.push_back(entry.path());
		}
	}
	return files;
}

// Key bindings are shared by the whole camp (2026-11-24): the game keeps them per hero
// (<hero>.key, and <hero>.ctl for a controller), so a new recruit started with the default keys.
// The newest binding file of any hero in the live folder becomes the camp's copy
// (camp/bindings/), and every hero's file is made the same as it. Run at the menu, after a game
// wrote its files, and before a hero is played. Returns how many hero files were changed.
inline auto SyncBindings(const fs::path& live) -> int {
	std::error_code ec;
	const fs::path store = live / "camp" / "bindings";
	fs::create_directories(store, ec);
	std::vector<std::string> heroes;
	for (const auto& entry : fs::directory_iterator(live, ec)) {
		if (entry.is_regular_file(ec) && Lower(entry.path().extension().string()) == ".d2s") {
			heroes.push_back(entry.path().stem().string());
		}
	}
	// Heroes seen at an earlier sync. A new recruit's first game writes the default keys, newest of
	// all, so only known heroes may set the camp's copy; a recruit just receives it.
	std::vector<std::string> known;
	{
		std::ifstream list(store / "heroes.txt");
		for (std::string line; std::getline(list, line);) {
			known.push_back(Lower(line));
		}
	}
	const bool firstSync = known.empty();
	const auto isKnown = [&](const std::string& hero) {
		return firstSync || std::find(known.begin(), known.end(), Lower(hero)) != known.end();
	};
	int changed = 0;
	for (const char* ext : { ".key", ".ctl" }) {
		const fs::path camp = store / (std::string("camp") + ext);
		fs::path newest;
		auto newestTime = fs::exists(camp, ec) ? fs::last_write_time(camp, ec) : fs::file_time_type::min();
		for (const auto& hero : heroes) {
			const fs::path file = live / (hero + ext);
			if (isKnown(hero) && fs::exists(file, ec) && fs::last_write_time(file, ec) > newestTime) {
				newest = file;
				newestTime = fs::last_write_time(file, ec);
			}
		}
		if (!newest.empty()) {
			fs::copy_file(newest, camp, fs::copy_options::overwrite_existing, ec);
			if (!ec) {
				fs::last_write_time(camp, newestTime, ec);  // so the same file isn't taken as newer next time
			}
		}
		if (!fs::exists(camp, ec)) {
			continue;
		}
		std::ifstream in(camp, std::ios::binary);
		const std::string wanted((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		for (const auto& hero : heroes) {
			const fs::path file = live / (hero + ext);
			std::string have;
			if (fs::exists(file, ec)) {
				std::ifstream h(file, std::ios::binary);
				have.assign((std::istreambuf_iterator<char>(h)), std::istreambuf_iterator<char>());
			}
			if (have != wanted) {
				fs::copy_file(camp, file, fs::copy_options::overwrite_existing, ec);
				if (!ec) {
					fs::last_write_time(file, newestTime, ec);  // not newer than the camp's copy
					++changed;
				}
			}
		}
	}
	std::ofstream list(store / "heroes.txt", std::ios::trunc);
	for (const auto& hero : heroes) {
		list << hero << "\n";
	}
	return changed;
}

// Benches every hero of the camp except `keep`.
inline auto Bench(const fs::path& live, int slot, const State& state, const std::string& keep, std::string& error) -> bool {
	std::vector<fs::path> files;
	for (const auto& hero : state.heroes) {
		if (Lower(hero.name) != Lower(keep)) {
			const auto own = HeroFiles(live, hero.name);
			files.insert(files.end(), own.begin(), own.end());
		}
	}
	return files.empty() || MoveAll(files, BenchDir(live, slot), error);
}

// Brings benched files back. A file that is already in the live folder wins; the benched copy
// then stays where it is.
inline auto Unbench(const fs::path& live, int slot) -> int {
	int left = 0;
	std::error_code ec;
	std::vector<fs::path> files;
	for (const auto& entry : fs::directory_iterator(BenchDir(live, slot), ec)) {
		if (entry.is_regular_file(ec)) {
			files.push_back(entry.path());
		}
	}
	for (const auto& file : files) {
		const fs::path target = live / file.filename();
		if (fs::exists(target, ec)) {
			++left;
			continue;
		}
		fs::rename(file, target, ec);
		if (ec) {
			++left;
		}
	}
	return left;
}

inline auto Benched(const fs::path& live, int slot, const std::string& name) -> bool {
	std::error_code ec;
	return fs::exists(BenchDir(live, slot) / (name + ".d2s"), ec);
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
	// Save version 105 (this game build) dropped 16 bytes from the start of the file: class and
	// level sit at 24 and 27 there (seen in real saves), where older versions have them at 40 and 43.
	const uint32_t version = head[4] | (head[5] << 8) | (head[6] << 16) | (static_cast<uint32_t>(head[7]) << 24);
	const int shift = version >= 105 ? 16 : 0;
	info.cls      = head[40 - shift];
	info.level    = head[43 - shift];
	info.hardcore = (head[36 - shift] & 0x04) != 0;
	info.dead     = (head[36 - shift] & 0x08) != 0;
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

// Moves every file of hero `name` out of the live folder into the graveyard, with the hero's
// route progress (route.h), so a later hero of the same name starts the act afresh.
inline auto Bury(const fs::path& live, int slot, const std::string& name, std::string& error) -> bool {
	std::vector<fs::path> files = HeroFiles(live, name);
	std::error_code ec;
	const fs::path progress = SlotDir(live, slot) / "progress" / (name + ".txt");
	if (fs::exists(progress, ec)) {
		files.push_back(progress);
	}
	const fs::path potions = SlotDir(live, slot) / "potions" / (name + ".potions.txt");  // potions.h
	if (fs::exists(potions, ec)) {
		files.push_back(potions);
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
	// Heroes whose save is gone (deleted): that counts as a death. The class locks and is
	// remembered as fallen. A hero waiting on the bench is not gone.
	for (const auto& hero : std::vector<Hero>(state.heroes)) {
		if (!onDisk(hero.name) && !Benched(live, slot, hero.name)) {
			MarkFallen(state, hero.name);
			log.push_back(hero.name + " the " + ClassNames[hero.cls] + " was deleted. That counts as a death: the class is locked again.");
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
