// Darkest Diablo — the second stash file, of heroes with the Warlock rules (DLC owners' heroes may
// have them whatever their class). No game or Windows code; tested in tests/camp_tests.cpp. How it plays: docs/stash.md.
//
// ModernSharedStashSoftCoreV2.d2i holds ordinary pages first (the game's own 5, where the other file
// has 3), then one section of type 1 (the Gems, Materials and Runes tabs: items with a stack
// size) and one of type 2 (the Chronicle, no item list). The type is byte 20 of a section's header.
// Like the other file it is read once, when the game starts, so it is laid out the same way: save 1 has
// pages 1-10, save 2 11-20, save 3 21-30, then 4 pages each for ascended heroes; 43 and 44 are unused.
//
// On disk: camp/stash-joined-modern.txt: "joining <check>" or "done", and "gold <slot> <amount>".
//   camp/stash-backup-<stamp>-modern/: the files as they were. camp/safe-next.txt: see EmptyTabs.
#pragma once

#include "ascend.h"
#include "camp.h"
#include "stash.h"
#include "stashfile.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ModernStash {

namespace fs = std::filesystem;

inline constexpr int OrdinaryPages = 44;  // the game's 5 and the mod's 39 (d2rloader.toml)

inline auto Type(const std::string& page) -> int { return static_cast<uint8_t>(page[20]); }
inline auto File(const fs::path& live) -> fs::path { return live / Camp::ModernStashName; }

// Does this hero's save have the Warlock rules? Byte 0xF8 of a version 105 save: 1 Classic, 2 Lord of
// Destruction, 3 Reign of the Warlock.
inline auto HasWarlockRules(const std::string& save) -> bool {
	return save.size() > 0xF8 && StashFile::Number(save, 0) == 0xAA55AA55 && StashFile::Number(save, 4) == 105 && save[0xF8] == 3;
}

struct Sections { std::vector<std::string> ordinary, special; };

inline auto Split(const std::string& file) -> std::optional<Sections> {
	const auto pages = StashFile::Pages(file);
	if (!pages) {
		return std::nullopt;
	}
	Sections out;
	for (const auto& page : *pages) {
		if (Type(page) > 2) {
			return std::nullopt;
		}
		(Type(page) == 0 ? out.ordinary : out.special).push_back(page);
	}
	return out;
}

inline auto Whole(const Sections& s) -> std::string { return StashFile::JoinPages(s.ordinary) + StashFile::JoinPages(s.special); }

inline auto Check(const std::string& text) -> std::string {
	uint64_t h = 1469598103934665603ULL;
	for (const char c : text) {
		h = (h ^ static_cast<uint8_t>(c)) * 1099511628211ULL;
	}
	return std::to_string(h);
}

// Writes a file so that it is either the old one or the new one, never half.
inline auto Replace(const fs::path& file, const std::string& text) -> bool {
	const fs::path next = file.string() + ".next";
	std::error_code ec;
	if (!Camp::WriteFile(next, text) || Camp::ReadFile(next) != text) {
		fs::remove(next, ec);
		return false;
	}
	fs::rename(next, file, ec);
	return !ec;
}

inline auto Keep(const fs::path& file, const fs::path& copy) -> bool {
	std::error_code ec;
	fs::create_directories(copy.parent_path(), ec);
	fs::copy_file(file, copy, fs::copy_options::overwrite_existing, ec);
	return !ec && Camp::ReadFile(copy) == Camp::ReadFile(file);
}

// Lays the three saves' files out as one. Where the pages of the file the game holds (the open
// save's) lie right already (`laidOut`: a version with the one-file stash has played it, and put a
// save's items on its own pages of whatever file was there) they stay; everything else goes to its
// save's pages. Nothing is dropped except a page that is the same, byte for byte, as one already there
// (changing saves could write one file over another's place).
inline auto Join(const fs::path& live, const std::string& stamp, bool laidOut, std::string& log) -> bool {
	log.clear();
	if (Camp::ModernStashJoined(live)) {
		return true;
	}
	const fs::path marker = Camp::ModernJoinedFile(live), joined = File(live);
	std::error_code ec;
	std::string mark = Camp::ReadFile(marker);
	if (mark.rfind("joining ", 0) == 0 && fs::exists(joined, ec)) {  // stopped half-way last time
		const size_t end = mark.find('\n');
		if (end != std::string::npos && mark.substr(8, end - 8) == Check(Camp::ReadFile(joined))) {
			log = "finished joining the Warlock rules' stashes.";
			return Camp::WriteFile(marker, "done" + mark.substr(end));
		}
	}
	const int active = Camp::ActiveSlot(live);
	Sections files[Camp::SlotCount];
	fs::path sources[Camp::SlotCount];
	long long gold[Camp::SlotCount] {};
	bool any = false;
	for (int s = 0; s < Camp::SlotCount; ++s) {
		sources[s] = s + 1 == active ? joined : Camp::SlotDir(live, s + 1) / "files" / Camp::ModernStashName;
		if (!fs::exists(sources[s], ec)) {
			continue;
		}
		any = true;
		const auto sections = Split(Camp::ReadFile(sources[s]));
		if (!sections) {
			log = "not a Warlock stash file this version knows: " + sources[s].string();
			return false;
		}
		files[s] = *sections;
		for (auto& page : files[s].ordinary) {
			gold[s] += StashFile::Gold(page);
			StashFile::SetNumber(page, 12, 0);
		}
	}
	if (!any) {  // no such hero was ever played: the game makes the file, for all saves
		return Camp::WriteFile(marker, "done\n");
	}
	Sections all;
	all.ordinary.assign(OrdinaryPages, StashFile::BlankPage());
	std::vector<bool> used(OrdinaryPages, false);
	std::string failed;
	// Puts a page where it belongs, else on an empty page of the same save.
	const auto put = [&](const std::string& page, int target, int save) {
		if (target >= 0 && target < StashFile::FilePages) {
			if (!used[static_cast<size_t>(target)]) {
				all.ordinary[static_cast<size_t>(target)] = page;
				used[static_cast<size_t>(target)] = true;
				return;
			}
			if (all.ordinary[static_cast<size_t>(target)] == page) {
				return;
			}
		}
		for (int i = save * StashFile::PagesPerSave; i < (save + 1) * StashFile::PagesPerSave; ++i) {
			if (!used[static_cast<size_t>(i)]) {
				all.ordinary[static_cast<size_t>(i)] = page;
				used[static_cast<size_t>(i)] = true;
				return;
			}
		}
		failed = "save " + std::to_string(save + 1) + " has no empty page left";
	};
	// First the open save's file, then the others by number; special sections from the first that has them.
	std::vector<int> order { active - 1 };
	for (int s = 0; s < Camp::SlotCount; ++s) {
		if (s != active - 1) {
			order.push_back(s);
		}
	}
	int items = 0;
	std::map<std::string, int> stacks;
	for (const int s : order) {
		for (size_t i = 0; i < files[s].ordinary.size(); ++i) {
			const std::string& page = files[s].ordinary[i];
			if (StashFile::ItemCount(page) == 0) {
				continue;
			}
			items += StashFile::ItemCount(page);
			const int p = static_cast<int>(i);
			const bool first = s == active - 1;
			if (p >= StashFile::FilePages) {
				put(page, -1, s);
			} else if (laidOut && (first || p >= StashFile::PagesPerSave)) {
				put(page, p, p < StashFile::AllPages ? p / StashFile::PagesPerSave : (p - StashFile::AllPages) / StashFile::TabsPerSave);
			} else if (laidOut && s != 0 && page == all.ordinary[i]) {
				continue;  // save 1's page, written into this save's file
			} else if (p < StashFile::PagesPerSave) {
				put(page, s * StashFile::PagesPerSave + p, s);
			} else if (p < StashFile::PagesPerSave + StashFile::TabsPerSave) {
				put(page, StashFile::AllPages + s * StashFile::TabsPerSave + p - StashFile::PagesPerSave, s);
			} else {
				put(page, -1, s);
			}
		}
		if (all.special.empty()) {
			all.special = files[s].special;
			continue;
		}
		for (const auto& page : files[s].special) {  // the other files' Gems, Materials and Runes
			if (Type(page) != 1 || StashFile::ItemCount(page) == 0) {
				continue;
			}
			bool same = false;
			for (auto& into : all.special) {
				same = same || (Type(into) == 1 && into.substr(StashFile::HeaderSize) == page.substr(StashFile::HeaderSize));
			}
			for (auto& into : all.special) {
				if (Type(into) == 1 && !same) {  // one list after the other; EmptyTabs empties it before the game sees it
					const int count = StashFile::ItemCount(into) + StashFile::ItemCount(page);
					into += page.substr(StashFile::HeaderSize + 4);
					into[StashFile::HeaderSize + 2] = static_cast<char>(count & 0xFF);
					into[StashFile::HeaderSize + 3] = static_cast<char>(count >> 8 & 0xFF);
					StashFile::SetNumber(into, 16, static_cast<uint32_t>(into.size()));
					same = into.size() < 0xFFFF;
					failed = same ? failed : "too many items in the old tabs";
				}
			}
			failed = same ? failed : "no place for the old tabs' items";
		}
	}
	if (!failed.empty()) {
		log = failed;
		return false;
	}
	const std::string text = Whole(all);
	const auto check = Split(text);
	if (!check || check->ordinary != all.ordinary || check->special != all.special) {
		log = "the joined Warlock stash does not read back the same";
		return false;
	}
	const fs::path backup = live / "camp" / ("stash-backup-" + stamp + "-modern");
	std::string marks = "\n";
	for (int s = 0; s < Camp::SlotCount; ++s) {
		marks += "gold " + std::to_string(s + 1) + " " + std::to_string(gold[s]) + "\n";
		if (fs::exists(sources[s], ec) && !Keep(sources[s], backup / ("slot" + std::to_string(s + 1) + ".d2i"))) {
			log = "could not back up " + sources[s].string();
			return false;
		}
	}
	if (!Camp::WriteFile(marker, "joining " + Check(text) + marks) || !Replace(joined, text) || !Camp::WriteFile(marker, "done" + marks)) {
		log = "could not write the joined Warlock stash; it is tried again at the next start";
		return false;
	}
	for (int s = 0; s < Camp::SlotCount; ++s) {  // in the way when their save is opened; the copies are in the backup
		if (s + 1 != active && fs::exists(sources[s], ec)) {
			fs::rename(sources[s], backup / ("slot" + std::to_string(s + 1) + "-parked.d2i"), ec);
		}
	}
	log = "joined the Warlock rules' stashes into one file (" + std::to_string(items) + " items); the old files are in camp/" + backup.filename().string() + ".";
	return true;
}

// The gold a save's Warlock stash held; asked for and forgotten by stash.cpp, which keeps it in the save's purse.
inline auto JoinedGold(const fs::path& live, int slot) -> long long {
	std::istringstream in(Camp::ReadFile(Camp::ModernJoinedFile(live)));
	std::string line;
	while (std::getline(in, line)) {
		std::istringstream words(line);
		std::string key;
		int which = 0;
		long long gold = 0;
		if (words >> key >> which >> gold && key == "gold" && which == slot) {
			return gold;
		}
	}
	return 0;
}

inline void ForgetGold(const fs::path& live, int slot) {
	std::istringstream in(Camp::ReadFile(Camp::ModernJoinedFile(live)));
	const std::string mine = "gold " + std::to_string(slot) + " ";
	std::string line, text;
	while (std::getline(in, line)) {
		if (line.rfind(mine, 0) != 0) {
			text += line + "\n";
		}
	}
	if (!text.empty()) {
		Camp::WriteFile(Camp::ModernJoinedFile(live), text);
	}
}

// ---- The old tabs' items -------------------------------------------------------------------
// Reads a type 1 section: plain items of kinds the safe knows, to the last byte. Anything else: nothing.
inline auto ReadTabs(const std::string& page, Stash::Safe& out) -> bool {
	Ascend::Shapes shapes;  // all of them misc items without properties
	for (const auto& kind : Stash::SafeKinds()) {
		shapes.items[kind.code] = {};
	}
	const Ascend::Bytes b(page.begin(), page.end());
	std::vector<Ascend::Item> items;
	std::string error;
	if (Ascend::ReadList(b, StashFile::HeaderSize, shapes, items, error) != b.size()) {
		return false;
	}
	for (const auto& item : items) {
		if (item.count > 0) {
			out[item.code] += item.count;
		}
	}
	return true;
}

// While the game starts: what the old tabs hold goes to the safe, and their section is emptied. The new
// safe waits in safe-next.txt until the stash is written, so a crash neither loses nor doubles anything.
inline auto EmptyTabs(const fs::path& live, const std::string& stamp, std::string& log) -> bool {
	log.clear();
	std::error_code ec;
	const fs::path safeFile = live / "camp" / "safe.txt", next = live / "camp" / "safe-next.txt";
	if (!fs::exists(File(live), ec)) {
		return true;
	}
	const std::string old = Camp::ReadFile(File(live));
	auto sections = Split(old);
	if (!sections) {
		log = "the Warlock stash file is not one this version knows";
		return false;
	}
	Stash::Safe found;
	for (auto& page : sections->special) {
		if (Type(page) != 1 || StashFile::ItemCount(page) == 0) {
			continue;
		}
		if (!ReadTabs(page, found)) {
			log = "the old tabs hold something this version can't read; nothing was moved";
			return false;
		}
		page.resize(StashFile::HeaderSize + 4);
		page[StashFile::HeaderSize + 2] = page[StashFile::HeaderSize + 3] = '\0';
		StashFile::SetNumber(page, 16, static_cast<uint32_t>(page.size()));
	}
	if (fs::exists(next, ec)) {
		if (found.empty()) {  // the stash was emptied last time, then it stopped
			fs::rename(next, safeFile, ec);
			return !ec;
		}
		fs::remove(next, ec);
	}
	if (found.empty()) {
		return true;
	}
	Stash::Safe safe = Stash::ParseSafe(Camp::ReadFile(safeFile));
	int total = 0;
	for (const auto& [code, n] : found) {
		safe[code] += n;
		total += n;
	}
	if (!Keep(File(live), live / "camp" / ("stash-backup-" + stamp + "-safe") / Camp::ModernStashName) ||
		!Camp::WriteFile(next, Stash::SerializeSafe(safe)) || !Replace(File(live), Whole(*sections))) {
		log = "could not write the Warlock stash; nothing was moved";
		fs::remove(next, ec);
		return false;
	}
	fs::rename(next, safeFile, ec);
	log = "moved " + std::to_string(total) + " items of the old Gems, Materials and Runes tabs to the safe.";
	return !ec;
}

}
