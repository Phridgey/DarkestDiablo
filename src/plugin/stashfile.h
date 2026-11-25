// Darkest Diablo — one stash file for all three saves (no game or Windows code here, so it can be
// tested anywhere; see tests/camp_tests.cpp). Why and how it plays: docs/stash.md.
//
// The game reads SharedStashSoftCoreV2.d2i once and keeps it until it closes, so the file can't be
// swapped when another save is opened. Instead it holds 30 pages: save 1 has pages 1-10, save 2
// 11-20, save 3 21-30; then come 4 pages each for ascended heroes (31-34, 35-38, 39-42; legacy.h). The file is its pages one after another: 64 bytes of header (mark, 2, version,
// gold, length) and a list of items. Only page 1's gold counts, and it is one purse for every page,
// so gold is kept by the plugin (stash.txt "purse") and the header's is 0.
//
// On disk: camp/stash-joined.txt: "joining" or "done", and "gold <slot> <amount>" (what each save's
//   purse held, until stash.cpp has put it in that save's stash.txt).
//   camp/wipe.txt: "<slot>" lines: saves deleted while the game held their pages in memory.
//   camp/stash-backup-<stamp>/: the three files as they were before joining.
#pragma once

#include "camp.h"

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace StashFile {

namespace fs = std::filesystem;

inline constexpr int PagesPerSave = 10;
inline constexpr int AllPages     = PagesPerSave * Camp::SlotCount;  // the ordinary pages
inline constexpr int TabsPerSave  = 4;                                // ascended heroes' (Legacy::TabCount)
inline constexpr int FilePages    = AllPages + TabsPerSave * Camp::SlotCount;
inline constexpr size_t HeaderSize = 64;
inline constexpr uint32_t Version = 0x69;

inline auto Number(const std::string& page, size_t at) -> uint32_t {
	uint32_t n = 0;
	for (size_t i = 0; i < 4; ++i) {
		n |= static_cast<uint32_t>(static_cast<uint8_t>(page[at + i])) << (8 * i);
	}
	return n;
}
inline void SetNumber(std::string& page, size_t at, uint32_t n) {
	for (size_t i = 0; i < 4; ++i) {
		page[at + i] = static_cast<char>((n >> (8 * i)) & 0xFF);
	}
}
inline auto Gold(const std::string& page) -> uint32_t { return Number(page, 12); }

inline auto BlankPage() -> std::string {
	std::string page(HeaderSize, '\0');
	page += std::string("JM\0\0", 4);
	SetNumber(page, 0, 0xAA55AA55);
	SetNumber(page, 4, 2);
	SetNumber(page, 8, Version);
	SetNumber(page, 16, static_cast<uint32_t>(page.size()));
	return page;
}

inline auto ItemCount(const std::string& page) -> int {
	return static_cast<uint8_t>(page[HeaderSize + 2]) | static_cast<uint8_t>(page[HeaderSize + 3]) << 8;
}

// The file's pages, or nothing when it is not exactly a row of pages of the version we know.
inline auto Pages(const std::string& file) -> std::optional<std::vector<std::string>> {
	std::vector<std::string> pages;
	for (size_t at = 0; at < file.size();) {
		if (file.size() - at < HeaderSize + 4) {
			return std::nullopt;
		}
		const std::string head = file.substr(at, HeaderSize + 4);
		const size_t length = Number(head, 16);
		if (Number(head, 0) != 0xAA55AA55 || Number(head, 8) != Version || length < HeaderSize + 4 || length > file.size() - at ||
			(head[20] != 2 && head.substr(HeaderSize, 2) != "JM")) {  // type 2, the Chronicle (modernstash.h), has no list
			return std::nullopt;
		}
		pages.push_back(file.substr(at, length));
		at += length;
	}
	return pages;
}

inline auto JoinPages(const std::vector<std::string>& pages) -> std::string {
	std::string file;
	for (const auto& page : pages) {
		file += page;
	}
	return file;
}

// One save's ten pages out of its own file (a missing file: blank pages), and its ascended heroes'
// (pages 11-14 of it). `gold` is its purse.
inline auto SavePages(const fs::path& file, std::vector<std::string>& out, std::vector<std::string>& tabs, uint32_t& gold, std::string& error) -> bool {
	out.clear();
	tabs.clear();
	gold = 0;
	std::error_code ec;
	if (fs::exists(file, ec)) {
		const auto pages = Pages(Camp::ReadFile(file));
		if (!pages) {
			error = "not a stash file this version knows: " + file.string();
			return false;
		}
		for (const auto& page : *pages) {
			gold += Gold(page);
			if (out.size() >= PagesPerSave && tabs.size() >= TabsPerSave) {
				if (ItemCount(page) != 0) {
					error = "items past page 14: " + file.string();
					return false;
				}
				continue;
			}
			auto& to = out.size() < PagesPerSave ? out : tabs;
			to.push_back(page);
			SetNumber(to.back(), 12, 0);
		}
	}
	while (out.size() < PagesPerSave) {
		out.push_back(BlankPage());
	}
	while (tabs.size() < TabsPerSave) {
		tabs.push_back(BlankPage());
	}
	return true;
}

// Puts the three saves' stashes into one file. Call it while the game starts, before it reads the
// save folder. Nothing is replaced until the new file is written and reads back the same; the old
// files are kept in camp/stash-backup-<stamp>. Safe to run again after a crash at any point.
// `log` says what happened. Returns true when the stash is joined (now or already).
inline auto Join(const fs::path& live, const std::string& stamp, std::string& log) -> bool {
	log.clear();
	if (Camp::StashJoined(live)) {
		return true;
	}
	const fs::path marker = Camp::JoinedFile(live), joined = live / Camp::StashName;
	std::error_code ec;
	if (Camp::ReadFile(marker).rfind("joining", 0) == 0 && fs::exists(joined, ec)) {
		// Stopped half-way last time: was the file already replaced?
		if (const auto pages = Pages(Camp::ReadFile(joined)); pages && pages->size() >= AllPages) {
			std::string text = Camp::ReadFile(marker);
			text.replace(0, 7, "done");
			if (!Camp::WriteFile(marker, text)) {
				log = "could not write camp/stash-joined.txt";
				return false;
			}
			log = "finished joining the stashes.";
			return true;
		}
	}
	const int active = Camp::ActiveSlot(live);
	std::vector<std::string> all, allTabs;
	std::string marks;
	std::vector<fs::path> sources;
	int items = 0;
	for (int slot = 1; slot <= Camp::SlotCount; ++slot) {
		const fs::path file = slot == active ? joined : Camp::SlotDir(live, slot) / "files" / Camp::StashName;
		std::vector<std::string> pages, tabs;
		uint32_t gold = 0;
		if (!SavePages(file, pages, tabs, gold, log)) {
			return false;
		}
		all.insert(all.end(), pages.begin(), pages.end());
		allTabs.insert(allTabs.end(), tabs.begin(), tabs.end());
		marks += "gold " + std::to_string(slot) + " " + std::to_string(gold) + "\n";
		sources.push_back(file);
	}
	all.insert(all.end(), allTabs.begin(), allTabs.end());
	for (const auto& page : all) {
		items += ItemCount(page);
	}
	const fs::path backup = live / "camp" / ("stash-backup-" + stamp);
	fs::create_directories(backup, ec);
	for (size_t i = 0; i < sources.size(); ++i) {
		if (fs::exists(sources[i], ec)) {
			const fs::path copy = backup / ("slot" + std::to_string(i + 1) + ".d2i");
			fs::copy_file(sources[i], copy, fs::copy_options::overwrite_existing, ec);
			if (ec || Camp::ReadFile(copy) != Camp::ReadFile(sources[i])) {
				log = "could not back up " + sources[i].string();
				return false;
			}
		}
	}
	const std::string text = JoinPages(all);
	const fs::path next = joined.string() + ".joined";
	if (!Camp::WriteFile(next, text) || Camp::ReadFile(next) != text) {
		log = "could not write the joined stash";
		fs::remove(next, ec);
		return false;
	}
	const auto check = Pages(text);
	if (!check || check->size() != FilePages || *check != all) {
		log = "the joined stash does not read back the same";
		fs::remove(next, ec);
		return false;
	}
	if (!Camp::WriteFile(marker, "joining\n" + marks)) {
		log = "could not write camp/stash-joined.txt";
		fs::remove(next, ec);
		return false;
	}
	ec.clear();
	fs::rename(next, joined, ec);  // replaces the file in one step on Windows and elsewhere
	if (ec) {
		log = "could not replace the stash: " + ec.message();
		fs::remove(next, ec);
		return false;
	}
	if (!Camp::WriteFile(marker, "done\n" + marks)) {
		log = "could not finish camp/stash-joined.txt; it is tried again at the next start";
		return false;
	}
	// The parked files would be in the way when their save is opened; their copies are in the backup.
	for (int slot = 1; slot <= Camp::SlotCount; ++slot) {
		if (slot != active) {
			fs::rename(sources[static_cast<size_t>(slot - 1)], backup / ("slot" + std::to_string(slot) + "-parked.d2i"), ec);
		}
	}
	log = "joined the three stashes into one file (" + std::to_string(items) + " items); the old files are in camp/" + backup.filename().string() + ".";
	return true;
}

// What a save's purse held when the stashes were joined.
inline auto JoinedGold(const fs::path& live, int slot) -> long long {
	std::istringstream in(Camp::ReadFile(Camp::JoinedFile(live)));
	std::string word;
	while (in >> word) {
		int which = 0;
		long long gold = 0;
		if (word == "gold" && in >> which >> gold && which == slot) {
			return gold;
		}
	}
	return 0;
}

inline void ForgetJoinedGold(const fs::path& live, int slot) {
	std::istringstream in(Camp::ReadFile(Camp::JoinedFile(live)));
	const std::string mine = "gold " + std::to_string(slot) + " ";
	std::string line, text;
	while (std::getline(in, line)) {
		if (line.rfind(mine, 0) != 0) {
			text += line + "\n";
		}
	}
	if (!text.empty()) {
		Camp::WriteFile(Camp::JoinedFile(live), text);
	}
}

// ---- Deleting a save -----------------------------------------------------------------------
// `modern`: for the Warlock rules' file (modernstash.h), which the game keeps apart.
inline auto WipeFile(const fs::path& live, bool modern = false) -> fs::path { return live / "camp" / (modern ? "wipe-modern.txt" : "wipe.txt"); }

inline auto Wipes(const fs::path& live, bool modern = false) -> std::set<int> {
	std::set<int> slots;
	std::istringstream in(Camp::ReadFile(WipeFile(live, modern)));
	int slot = 0;
	while (in >> slot) {
		if (slot >= 1 && slot <= Camp::SlotCount) {
			slots.insert(slot);
		}
	}
	return slots;
}

inline auto SetWipes(const fs::path& live, const std::set<int>& slots, bool modern = false) -> bool {
	std::error_code ec;
	if (slots.empty()) {
		fs::remove(WipeFile(live, modern), ec);
		return !ec;
	}
	std::string text;
	for (const int slot : slots) {
		text += std::to_string(slot) + "\n";
	}
	return Camp::WriteFile(WipeFile(live, modern), text);
}

// Copies a save's pages to a file of their own (for the trash).
inline auto CopyPages(const fs::path& live, int slot, const fs::path& to, bool modern = false) -> bool {
	const auto pages = Pages(Camp::ReadFile(live / (modern ? Camp::ModernStashName : Camp::StashName)));
	if (!pages || pages->size() < AllPages || slot < 1 || slot > Camp::SlotCount) {
		return false;
	}
	const auto from = pages->begin() + (slot - 1) * PagesPerSave;
	std::vector<std::string> mine(from, from + PagesPerSave);
	if (pages->size() >= FilePages) {
		const auto tabs = pages->begin() + AllPages + (slot - 1) * TabsPerSave;
		mine.insert(mine.end(), tabs, tabs + TabsPerSave);
	}
	return Camp::WriteFile(to, JoinPages(mine));
}

// Empties a save's pages in the file. Only while the game does not hold the stash in memory.
inline auto BlankPages(const fs::path& live, int slot, bool modern = false) -> bool {
	const fs::path file = live / (modern ? Camp::ModernStashName : Camp::StashName);
	if (std::error_code ec; modern && !fs::exists(file, ec)) {
		return true;
	}
	auto pages = Pages(Camp::ReadFile(file));
	if (!pages || pages->size() < AllPages || slot < 1 || slot > Camp::SlotCount) {
		return false;
	}
	for (int i = 0; i < PagesPerSave; ++i) {
		(*pages)[static_cast<size_t>((slot - 1) * PagesPerSave + i)] = BlankPage();
	}
	for (int i = 0; i < TabsPerSave && pages->size() >= FilePages; ++i) {
		(*pages)[static_cast<size_t>(AllPages + (slot - 1) * TabsPerSave + i)] = BlankPage();
	}
	return Camp::WriteFile(file, JoinPages(*pages));
}

}
