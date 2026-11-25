// Darkest Diablo — Ascend: lifting a hero's items out of their save file and putting them into
// another hero's. No game code here; tested in tests/camp_tests.cpp.
//
// The SDK can't move worn or belt items, and the game reads the shared stash only once a session
// but every hero's save on every load. So items travel as the save's own bytes: out of the
// ascended hero's file, into the personal stash of the next hero loaded, and from there
// (in the game, through the SDK) onto the temporary stash page.
//
// Rule of this file: never guess. Anything not understood to the last byte is an error, and an
// error leaves every file as it was.
//
// Save version 105, checked on 64 real saves and 400 stash pages (2026-11-24). Not seen yet, so
// refused: ears, and items whose last bit is set. Not yet seen in a real save: runewords,
// sets, personalized items.
#pragma once

#include <cstdint>
#include <istream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace Ascend {

using Bytes = std::vector<uint8_t>;

struct ItemShape { char kind = 'm'; bool stack = false, book = false; int w = 1, h = 1; };
struct StatShape { int bits = 0, param = 0; };

struct Shapes {
	std::map<std::string, ItemShape> items;
	std::map<int, StatShape> stats;

	// Takes the "isave" and "ssave" lines of loot_data.txt; other lines are skipped.
	void Read(std::istream& in) {
		std::string line;
		while (std::getline(in, line)) {
			std::istringstream words(line);
			std::string key;
			words >> key;
			if (key == "isave") {
				std::string code;
				ItemShape s;
				int stack = 0, book = 0;
				if (words >> code >> s.kind >> stack >> book >> s.w >> s.h) {
					s.stack = stack != 0, s.book = book != 0;
					items[code] = s;
				}
			} else if (key == "ssave") {
				int id = 0;
				StatShape s;
				if (words >> id >> s.bits >> s.param) {
					stats[id] = s;
				}
			}
		}
	}
};

struct Item {
	Bytes bytes;        // the item and the items in its sockets
	std::string code;
	int loc = 0, slot = 0, x = 0, y = 0, page = 0;  // loc: 0 stored, 1 worn, 2 belt, 4 hand
	int w = 1, h = 1;
};

class Reader {
public:
	Reader(const Bytes& b, size_t at) : m_bytes(b), m_bit(at * 8) {}
	auto Get(int n) -> uint32_t {
		uint32_t v = 0;
		for (int i = 0; i < n; ++i, ++m_bit) {
			if (m_bit / 8 >= m_bytes.size()) {
				m_ok = false;
				return 0;
			}
			const uint32_t bit = (m_bytes[m_bit / 8] >> (m_bit % 8)) & 1U;
			if (i < 32) {
				v |= bit << i;
			}
		}
		return v;
	}
	void Skip(int n) {
		m_bit += static_cast<size_t>(n);
		m_ok = m_ok && m_bit <= m_bytes.size() * 8;
	}
	auto Ok() const -> bool { return m_ok; }
	auto End() const -> size_t { return (m_bit + 7) / 8; }
private:
	const Bytes& m_bytes;
	size_t m_bit;
	bool m_ok = true;
};

inline auto CodeLetter(const std::string& bits) -> char {
	static const std::map<std::string, char> letters = {
		{ "10", ' ' }, { "11111011", '0' }, { "1111100", '1' }, { "001100", '2' }, { "1101101", '3' }, { "11111010", '4' },
		{ "00010110", '5' }, { "1101111", '6' }, { "01111", '7' }, { "000100", '8' }, { "01110", '9' }, { "11110", 'a' },
		{ "0101", 'b' }, { "01000", 'c' }, { "110001", 'd' }, { "110000", 'e' }, { "010011", 'f' }, { "11010", 'g' },
		{ "00011", 'h' }, { "1111110", 'i' }, { "000101110", 'j' }, { "010010", 'k' }, { "11101", 'l' }, { "01101", 'm' },
		{ "001101", 'n' }, { "1111111", 'o' }, { "11001", 'p' }, { "11011001", 'q' }, { "11100", 'r' }, { "0010", 's' },
		{ "01100", 't' }, { "00001", 'u' }, { "1101110", 'v' }, { "00000", 'w' }, { "00111", 'x' }, { "0001010", 'y' }, { "11011000", 'z' },
	};
	const auto it = letters.find(bits);
	return it == letters.end() ? '\0' : it->second;
}

inline auto SkipStats(Reader& r, const Shapes& shapes) -> bool {
	for (int guard = 0; guard < 512 && r.Ok(); ++guard) {
		const int id = static_cast<int>(r.Get(9));
		if (id == 511) {
			return r.Ok();
		}
		// Some stats are stored with the ones that follow them (minimum and maximum damage, length).
		const int more = id == 17 || id == 48 || id == 50 || id == 52 ? 1 : id == 54 || id == 57 ? 2 : 0;
		for (int k = id; k <= id + more; ++k) {
			const auto it = shapes.stats.find(k);
			if (it == shapes.stats.end() || it->second.bits <= 0) {
				return false;
			}
			r.Skip(it->second.param + it->second.bits);
		}
	}
	return false;
}

// Reads one item at `at`; returns where it ends, or 0 with `error` set.
inline auto ReadItem(const Bytes& b, size_t at, const Shapes& shapes, Item& out, std::string& error, int depth = 0) -> size_t {
	Reader r(b, at);
	const uint32_t flags = r.Get(32);
	r.Skip(3);
	out.loc = static_cast<int>(r.Get(3)), out.slot = static_cast<int>(r.Get(4));
	out.x = static_cast<int>(r.Get(4)), out.y = static_cast<int>(r.Get(4)), out.page = static_cast<int>(r.Get(3));
	if ((flags & 0x10000U) != 0) {
		error = "an ear";
		return 0;
	}
	for (int i = 0; i < 4; ++i) {
		std::string bits;
		char c = '\0';
		while (c == '\0' && bits.size() < 10 && r.Ok()) {
			bits += r.Get(1) != 0 ? '1' : '0';
			c = CodeLetter(bits);
		}
		if (c == '\0') {
			error = "an unreadable item code";
			return 0;
		}
		if (c != ' ') {
			out.code += c;
		}
	}
	const auto shape = shapes.items.find(out.code);
	if (shape == shapes.items.end()) {
		error = "an unknown item (" + out.code + ")";
		return 0;
	}
	out.w = shape->second.w, out.h = shape->second.h;
	uint32_t children = 0;
	bool last = false;
	if ((flags & 0x200000U) != 0) {  // a simple item
		if (r.Get(1) != 0) {
			r.Skip(128);
		}
		last = r.Get(1) != 0;
	} else {
		children = r.Get(3);
		r.Skip(32 + 7);
		const uint32_t quality = r.Get(4);
		if (r.Get(1) != 0) { r.Skip(3); }   // picture
		if (r.Get(1) != 0) { r.Skip(11); }  // automatic affix
		if (quality == 1 || quality == 3) {
			r.Skip(3);
		} else if (quality == 4) {
			r.Skip(22);
		} else if (quality == 5 || quality == 7) {
			r.Skip(12);
		} else if (quality == 6 || quality == 8) {
			r.Skip(16);
			for (int i = 0; i < 6; ++i) {
				if (r.Get(1) != 0) { r.Skip(11); }
			}
		} else if (quality != 2) {
			error = "an unknown quality on " + out.code;
			return 0;
		}
		const bool runeword = (flags & 0x4000000U) != 0;
		if (runeword) { r.Skip(16); }
		if ((flags & 0x1000000U) != 0) {  // a name
			int letters = 0;
			while (r.Get(8) != 0 && r.Ok() && ++letters < 32) {}
		}
		if (shape->second.book) { r.Skip(5); }
		if (r.Get(1) != 0) { r.Skip(128); }
		if (shape->second.kind == 'a') { r.Skip(11); }
		if (shape->second.kind != 'm' && r.Get(8) != 0) { r.Skip(9); }
		if (shape->second.stack) { r.Skip(9); }
		if ((flags & 0x800U) != 0) { r.Skip(4); }
		const uint32_t sets = quality == 5 ? r.Get(5) : 0;
		if (r.Get(1) != 0) {
			error = "an unknown mark on " + out.code;
			return 0;
		}
		bool ok = SkipStats(r, shapes);
		for (int i = 0; i < 5 && ok; ++i) {
			ok = (sets >> i & 1U) == 0 || SkipStats(r, shapes);
		}
		ok = ok && (!runeword || SkipStats(r, shapes));
		if (!ok) {
			error = "unreadable properties on " + out.code;
			return 0;
		}
		last = r.Get(1) != 0;
	}
	if (!r.Ok() || last) {
		error = (last ? "an unknown ending on " : "the file ends inside ") + out.code + " (byte " + std::to_string(at) + ")";
		return 0;
	}
	size_t end = r.End();
	for (uint32_t i = 0; i < children; ++i) {
		Item child;
		end = depth > 0 ? 0 : ReadItem(b, end, shapes, child, error, depth + 1);
		if (end == 0) {
			error = error.empty() ? "sockets in sockets" : error;
			return 0;
		}
	}
	out.bytes.assign(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(end));
	return end;
}

inline auto Marker(const Bytes& b, size_t at, char first, char second) -> bool {
	return at + 2 <= b.size() && b[at] == static_cast<uint8_t>(first) && b[at + 1] == static_cast<uint8_t>(second);
}

// A "JM" list at `at`. Returns its end, or 0.
inline auto ReadList(const Bytes& b, size_t at, const Shapes& shapes, std::vector<Item>& out, std::string& error) -> size_t {
	if (!Marker(b, at, 'J', 'M') || at + 4 > b.size()) {
		error = "no item list where one should be";
		return 0;
	}
	const int count = b[at + 2] | b[at + 3] << 8;
	at += 4;
	for (int i = 0; i < count; ++i) {
		Item item;
		at = ReadItem(b, at, shapes, item, error);
		if (at == 0) {
			return 0;
		}
		out.push_back(std::move(item));
	}
	return at;
}

struct Hero {
	std::vector<Item> items, merc, corpse;
	size_t list = 0, listEnd = 0;  // the hero's own item list in the file
};

// The whole item part of a version 105 save, from the hero's list to the last byte. The list is
// found by trying each "JM": only the true one reads through to the file's end.
inline auto ReadHero(const Bytes& b, const Shapes& shapes, Hero& out, std::string& error) -> bool {
	if (b.size() < 400 || b[0] != 0x55 || b[1] != 0xAA || b[2] != 0x55 || b[3] != 0xAA || b[4] != 105 || b[5] != 0) {
		error = "not a version 105 save";
		return false;
	}
	error = "no item list";
	for (size_t at = 300; at + 4 <= b.size(); ++at) {
		if (!Marker(b, at, 'J', 'M')) {
			continue;
		}
		Hero hero;
		std::string why;
		hero.list = at;
		hero.listEnd = ReadList(b, at, shapes, hero.items, why);
		size_t p = hero.listEnd;
		// Corpses: a count, then for each 12 bytes and an item list.
		if (p != 0 && Marker(b, p, 'J', 'M') && p + 4 <= b.size()) {
			const int corpses = b[p + 2] | b[p + 3] << 8;
			p += 4;
			for (int i = 0; i < corpses && p != 0; ++i) {
				p = ReadList(b, p + 12, shapes, hero.corpse, why);
			}
		} else {
			p = 0;
		}
		if (p != 0 && !Marker(b, p, 'j', 'f')) {
			p = 0;
		}
		p = p == 0 ? 0 : p + 2;
		if (p != 0 && Marker(b, p, 'J', 'M')) {
			p = ReadList(b, p, shapes, hero.merc, why);
		}
		if (p != 0 && Marker(b, p, 'k', 'f')) {
			p += 2;
			// "kf", then whether there is a golem; a golem made from an item is refused.
			if (p + 1 == b.size() && b[p] == 0) {
				out = std::move(hero);
				error.clear();
				return true;
			}
			why = "an iron golem, or an unknown ending";
		}
		if (!why.empty() && error == "no item list") {
			error = why;  // the first candidate's reason; the true list is nearly always the first
		}
	}
	return false;
}

// Puts an item in a place: changes only its place bits (42 to 60 hold place, slot, x, y, page).
inline void Place(Item& item, int x, int y, int page) {
	const uint32_t values[5] = { 0, 0, static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(page) };
	const int sizes[5] = { 3, 4, 4, 4, 3 };
	size_t bit = 35;
	for (int f = 0; f < 5; ++f) {
		for (int i = 0; i < sizes[f]; ++i, ++bit) {
			const uint8_t mask = static_cast<uint8_t>(1U << (bit % 8));
			item.bytes[bit / 8] = static_cast<uint8_t>((values[f] >> i & 1U) != 0 ? item.bytes[bit / 8] | mask : item.bytes[bit / 8] & ~mask);
		}
	}
	item.loc = 0, item.slot = 0, item.x = x, item.y = y, item.page = page;
}

// Packs items into pages of `size` x `size` cells, biggest first; gives each its page number.
inline auto Pack(std::vector<Item>& items, int size, std::vector<int>& pageOf) -> int {
	std::vector<size_t> order;
	for (size_t i = 0; i < items.size(); ++i) {
		order.push_back(i);
	}
	for (size_t i = 1; i < order.size(); ++i) {  // steady insertion sort by area
		for (size_t k = i; k > 0 && items[order[k]].w * items[order[k]].h > items[order[k - 1]].w * items[order[k - 1]].h; --k) {
			std::swap(order[k], order[k - 1]);
		}
	}
	std::vector<std::vector<bool>> pages;
	pageOf.assign(items.size(), -1);
	for (const size_t i : order) {
		Item& item = items[i];
		for (size_t p = 0; pageOf[i] < 0; ++p) {
			if (item.w > size || item.h > size) {
				return -1;
			}
			if (p == pages.size()) {
				pages.emplace_back(static_cast<size_t>(size * size), false);
			}
			for (int y = 0; y + item.h <= size && pageOf[i] < 0; ++y) {
				for (int x = 0; x + item.w <= size && pageOf[i] < 0; ++x) {
					bool open = true;
					for (int k = 0; k < item.w * item.h; ++k) {
						open = open && !pages[p][static_cast<size_t>((y + k / item.w) * size + x + k % item.w)];
					}
					if (open) {
						for (int k = 0; k < item.w * item.h; ++k) {
							pages[p][static_cast<size_t>((y + k / item.w) * size + x + k % item.w)] = true;
						}
						Place(item, x, y, 5);  // 5: the personal stash in a save
						pageOf[i] = static_cast<int>(p);
					}
				}
			}
		}
	}
	return static_cast<int>(pages.size());
}

// What an ascended hero leaves: everything on the hero and its corpse (not the mercenary's gear), without the Cube itself
// (what lies in it stays).
inline auto Legacy(const Hero& hero) -> std::vector<Item> {
	std::vector<Item> out;
	for (const auto* list : { &hero.items, &hero.corpse }) {
		for (const Item& item : *list) {
			if (item.code != "box") {
				out.push_back(item);
			}
		}
	}
	return out;
}

inline void Checksum(Bytes& b) {
	const auto put = [&](size_t at, uint32_t v) {
		for (size_t i = 0; i < 4; ++i) {
			b[at + i] = static_cast<uint8_t>(v >> (8 * i));
		}
	};
	put(8, static_cast<uint32_t>(b.size()));
	put(12, 0);
	uint32_t sum = 0;
	for (const uint8_t c : b) {
		sum = (sum << 1 | sum >> 31) + c;
	}
	put(12, sum);
}

// Adds items (already placed) to a save. The hero's personal stash must be empty. The result is
// read back in full; false leaves `b` as it was.
inline auto Inject(Bytes& b, const std::vector<Item>& items, const Shapes& shapes, std::string& error) -> bool {
	Hero before;
	if (!ReadHero(b, shapes, before, error)) {
		return false;
	}
	for (const Item& item : before.items) {
		if (item.loc == 0 && item.page == 5) {
			error = "the hero's own stash page is not empty";
			return false;
		}
	}
	Bytes made = b, extra;
	for (const Item& item : items) {
		extra.insert(extra.end(), item.bytes.begin(), item.bytes.end());
	}
	const size_t count = before.items.size() + items.size();
	if (count > 0xFFFF) {
		error = "too many items";
		return false;
	}
	made[before.list + 2] = static_cast<uint8_t>(count & 0xFF);
	made[before.list + 3] = static_cast<uint8_t>(count >> 8);
	made.insert(made.begin() + static_cast<std::ptrdiff_t>(before.listEnd), extra.begin(), extra.end());
	Checksum(made);
	Hero after;
	if (!ReadHero(made, shapes, after, error) || after.items.size() != count || after.merc.size() != before.merc.size()) {
		error = "the changed save does not read back: " + error;
		return false;
	}
	b = std::move(made);
	return true;
}

}
