// See gameapi.h and docs/game-state-api.md. READ ONLY.
#include "gameapi.h"

#include "gamememory.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <set>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <utility>

namespace Game {
namespace {

// Offsets beyond gamestate.cpp's, from the community's maps of D2R (D2RMH, MapAssist).
constexpr std::size_t UnitData        = 0x10;  // pointer: PlayerData / MonsterData / ItemData
constexpr std::size_t UnitAct         = 0x20;  // pointer: the act the unit is in
constexpr std::size_t ActNumber       = 0x28;  // uint32: 0 = Act I
constexpr std::size_t ActMisc         = 0x78;  // pointer: act misc
constexpr std::size_t MiscDifficulty  = 0x830; // uint8: 0 Normal, 1 Nightmare, 2 Hell
constexpr std::size_t MonsterFlags    = 0x1A;  // uint8: see below
constexpr std::size_t MonsterEnchants = 0x20;  // uint8[9]: monumod.txt rows, 0 = none
constexpr std::size_t MonsterUniqueNo = 0x2A;  // uint16: superuniques.txt row
constexpr std::size_t ItemQuality     = 0x00;  // uint32
constexpr std::size_t ItemOwner       = 0x0C;  // uint32: holder's unit number
constexpr std::size_t ItemFlags       = 0x18;  // uint32
constexpr std::size_t ItemLevel       = 0x3C;  // uint32
constexpr uint8_t     FlagSuperUnique = 0x02, FlagChampion = 0x04, FlagUnique = 0x08, FlagMinion = 0x10, FlagPossessed = 0x20, FlagGhostly = 0x40;

std::atomic<uint32_t> g_localId { 0 };

template <typename T>
auto Get(uintptr_t address, T& out) -> bool {
	return GameState::ReadBytes(address, &out, sizeof(T));
}

auto Distance2(const Unit& a, const Unit& b) -> double {
	const double dx = a.fx - b.fx, dy = a.fy - b.fy;
	return dx * dx + dy * dy;
}

auto Near(const Unit& hero, const Unit& u, int radius) -> bool {
	return radius < 0 || (u.placed && Distance2(hero, u) <= static_cast<double>(radius) * radius);
}

// Players: mode 0 dying, 17 dead. Monsters: 0 dying, 12 dead.
auto DeadMode(Kind kind, uint32_t mode) -> bool {
	return kind == Kind::Player ? (mode == 0 || mode == 17) : kind == Kind::Monster && (mode == 0 || mode == 12);
}

auto Dead(const Unit& u) -> bool {
	return DeadMode(u.kind, u.mode);
}

}  // namespace

auto Stat(const Unit& unit, uint16_t stat, uint16_t param, bool withItems) -> std::optional<int> {
	for (const GameState::StatEntry& e : GameState::Stats(unit, withItems)) {
		if (e.id == stat && e.param == param) {
			return stat >= StatId::Life && stat <= StatId::MaxStamina ? e.value >> 8 : e.value;
		}
	}
	return std::nullopt;
}

void SetLocalPlayerId(uint32_t id) {
	g_localId = id;
}

auto Hero() -> Unit {
	if (!GameState::Known()) {
		return {};
	}
	// 1. The SDK's number for the local player. Several player units can share it (the game keeps
	//    copies), so the placed one the Campfire's search found wins, then any placed one.
	const uintptr_t scanned = GameMemory::HeroUnit();
	if (const uint32_t id = g_localId.load(); id != 0) {
		Unit best;
		for (const Unit& u : GameState::Units(Kind::Player)) {
			if (u.id == id && u.placed && (!best || u.address == scanned)) {
				best = u;
			}
		}
		if (best) {
			return best;
		}
	}
	// 2. The unit the Campfire's town search found by the hero's name.
	if (const Unit u = GameState::At(scanned, Kind::Player); u && u.placed) {
		return u;
	}
	// 3. The first placed, named player.
	return GameState::Player();
}

auto HeroInfo() -> HeroDetails {
	HeroDetails h;
	h.unit = Hero();
	if (!h.unit) {
		return h;
	}
	h.name = h.unit.name;
	h.classId = static_cast<int>(h.unit.row);
	h.level = Stat(h.unit, StatId::Level, 0, false).value_or(-1);
	h.life = Stat(h.unit, StatId::Life, 0, false).value_or(-1);
	h.maxLife = Stat(h.unit, StatId::MaxLife).value_or(-1);
	h.mana = Stat(h.unit, StatId::Mana, 0, false).value_or(-1);
	h.maxMana = Stat(h.unit, StatId::MaxMana).value_or(-1);
	h.experience = Stat(h.unit, StatId::Experience, 0, false).value_or(-1);
	h.gold = Stat(h.unit, StatId::Gold, 0, false).value_or(0);
	h.stashGold = Stat(h.unit, StatId::StashGold, 0, false).value_or(0);
	h.area = GameState::Level(h.unit);
	h.dead = Dead(h.unit);
	return h;
}

auto World() -> WorldDetails {
	WorldDetails w;
	const Unit hero = Hero();
	if (!hero) {
		return w;
	}
	w.area = GameState::Level(hero);
	uintptr_t act = 0, misc = 0;
	uint32_t number = 0;
	uint8_t difficulty = 0;
	if (Get(hero.address + UnitAct, act) && act != 0 && Get(act + ActNumber, number) && number < 5) {
		w.act = static_cast<int>(number);
		if (Get(act + ActMisc, misc) && misc != 0 && Get(misc + MiscDifficulty, difficulty) && difficulty < 3) {
			w.difficulty = difficulty;
		}
	}
	return w;
}

auto Monster(const Unit& unit) -> MonsterDetails {
	MonsterDetails m;
	m.unit = unit;
	if (!unit || unit.kind != Kind::Monster) {
		return m;
	}
	m.alive = !Dead(unit);
	uintptr_t data = 0;
	uint8_t flags = 0;
	if (Get(unit.address + UnitData, data) && data != 0 && Get(data + MonsterFlags, flags)) {
		m.superUnique = (flags & FlagSuperUnique) != 0;
		m.champion = (flags & FlagChampion) != 0;
		m.unique = (flags & FlagUnique) != 0;
		m.minion = (flags & FlagMinion) != 0;
		m.possessed = (flags & FlagPossessed) != 0;
		m.ghostly = (flags & FlagGhostly) != 0;
		uint8_t enchants[9] {};
		if ((m.champion || m.unique || m.superUnique) && Get(data + MonsterEnchants, enchants)) {
			for (const uint8_t e : enchants) {
				if (e != 0) {
					m.enchantments.push_back(e);
				}
			}
		}
		uint16_t no = 0;
		if (m.superUnique && Get(data + MonsterUniqueNo, no)) {
			m.superUniqueId = no;
		}
	}
	m.life = Stat(unit, StatId::Life, 0, false).value_or(-1);
	m.maxLife = Stat(unit, StatId::MaxLife).value_or(-1);
	return m;
}

auto Monsters(int radius, bool aliveOnly) -> std::vector<MonsterDetails> {
	std::vector<MonsterDetails> out;
	const Unit hero = Hero();
	if (!hero) {
		return out;
	}
	for (const Unit& u : GameState::Units(Kind::Monster)) {
		if (Near(hero, u, radius) && (!aliveOnly || !Dead(u))) {
			out.push_back(Monster(u));
		}
	}
	std::sort(out.begin(), out.end(), [&](const MonsterDetails& a, const MonsterDetails& b) { return Distance2(hero, a.unit) < Distance2(hero, b.unit); });
	return out;
}

auto Objects(int radius, uint32_t row) -> std::vector<Unit> {
	std::vector<Unit> out;
	const Unit hero = Hero();
	if (!hero) {
		return out;
	}
	for (const Unit& u : GameState::Units(Kind::Object)) {
		if (Near(hero, u, radius) && (row == GameState::AnyRow || u.row == row)) {
			out.push_back(u);
		}
	}
	std::sort(out.begin(), out.end(), [&](const Unit& a, const Unit& b) { return Distance2(hero, a) < Distance2(hero, b); });
	return out;
}

auto Item(const Unit& unit) -> ItemDetails {
	ItemDetails i;
	i.unit = unit;
	if (!unit || unit.kind != Kind::Item) {
		return i;
	}
	i.place = unit.mode <= 6 ? static_cast<ItemPlace>(unit.mode) : ItemPlace::Unknown;
	uintptr_t data = 0;
	uint32_t quality = 0, owner = 0, flags = 0, level = 0;
	if (Get(unit.address + UnitData, data) && data != 0) {
		if (Get(data + ItemQuality, quality) && quality <= 9) {
			i.quality = static_cast<int>(quality);
		}
		if (Get(data + ItemOwner, owner)) {
			i.ownerId = owner;
		}
		if (Get(data + ItemFlags, flags)) {
			i.flags = flags;
		}
		if (Get(data + ItemLevel, level) && level <= 120) {
			i.itemLevel = static_cast<int>(level);
		}
	}
	return i;
}

auto GroundItems(int radius) -> std::vector<ItemDetails> {
	std::vector<ItemDetails> out;
	const Unit hero = Hero();
	if (!hero) {
		return out;
	}
	for (const Unit& u : GameState::Units(Kind::Item)) {
		if ((u.mode == 3 || u.mode == 5) && Near(hero, u, radius)) {
			out.push_back(Item(u));
		}
	}
	std::sort(out.begin(), out.end(), [&](const ItemDetails& a, const ItemDetails& b) { return Distance2(hero, a.unit) < Distance2(hero, b.unit); });
	return out;
}


// ---- States ---------------------------------------------------------------------------------
// The classic game's stat list, widened to 64 bits (matches what we checked: the unit's own stats
// at list + 0x30, an 8-byte pointer then an 8-byte count). Each timed state has its own list that
// points back at the unit (+0x08) and is linked from the unit's list. mapped, unchecked.
namespace {

constexpr std::size_t UnitStatList   = 0x88;
constexpr std::size_t ListUnit       = 0x08;  // pointer: the unit the list belongs to
constexpr std::size_t ListState      = 0x1C;  // uint32: states.txt row
constexpr std::size_t ListEndFrame   = 0x20;  // uint32: game frame it ends on
constexpr std::size_t ListSkill      = 0x24;  // uint32: skills.txt row
constexpr std::size_t ListSkillLevel = 0x28;  // uint32
constexpr std::size_t ListStats      = 0x30;  // pointer + count: the list's own stats
constexpr int         MaxStates      = 1200;  // states.txt has fewer rows than this

auto LooksPointer(uint64_t v) -> bool {
	return v >= 0x10000 && v < 0x7FFFFFFFFFFF && (v & 7) == 0;
}

// The state lists linked from the unit's own list, found by following pointers (two hops) to
// blocks that point back at the unit. Robust to the exact link offsets, which we haven't checked.
auto StateLists(const Unit& unit, uintptr_t& main) -> std::vector<uintptr_t> {
	std::vector<uintptr_t> found;
	main = 0;
	if (!unit || !Get(unit.address + UnitStatList, main) || !LooksPointer(main)) {
		return found;
	}
	std::set<uintptr_t> seen { main };
	std::vector<uintptr_t> todo { main };
	for (int hop = 0; hop < 3 && !todo.empty(); ++hop) {
		std::vector<uintptr_t> next;
		for (const uintptr_t block : todo) {
			uint64_t words[0x100 / 8] {};
			if (!Get(block, words)) {
				continue;
			}
			for (std::size_t i = 7; i < std::size(words); ++i) {  // past the header and own stats
				const uintptr_t p = static_cast<uintptr_t>(words[i]);
				if (!LooksPointer(p) || seen.count(p) != 0) {
					continue;
				}
				uintptr_t owner = 0;
				uint32_t state = 0;
				if (Get(p + ListUnit, owner) && owner == unit.address && Get(p + ListState, state) && state > 0 && state < MaxStates) {
					seen.insert(p);
					found.push_back(p);
					next.push_back(p);
				}
			}
		}
		todo = std::move(next);
		if (found.size() > 64) {
			break;
		}
	}
	return found;
}

auto ListStatsOf(uintptr_t list) -> std::vector<GameState::StatEntry> {
	std::vector<GameState::StatEntry> out;
	uint64_t at = 0, count = 0;
	if (!Get(list + ListStats, at) || !Get(list + ListStats + 8, count) || !LooksPointer(at) || count == 0 || count > 64) {
		return out;
	}
	for (uint64_t i = 0; i < count; ++i) {  // 16 bytes each on our build: param, stat, value
		uint32_t param = 0, id = 0;
		int32_t value = 0;
		if (Get(at + i * 16, param) && Get(at + i * 16 + 4, id) && Get(at + i * 16 + 8, value)) {
			out.push_back({ static_cast<uint16_t>(id), static_cast<uint16_t>(param), value });
		}
	}
	return out;
}

std::mutex g_stateLock;
std::map<std::pair<uint32_t, int>, std::chrono::steady_clock::time_point> g_stateSince;  // (unit, state) -> first seen

}  // namespace

auto States(const Unit& unit) -> std::vector<StateDetails> {
	std::vector<StateDetails> out;
	uintptr_t main = 0;
	std::set<int> have;
	for (const uintptr_t list : StateLists(unit, main)) {
		StateDetails d;
		uint32_t v = 0;
		d.state = Get(list + ListState, v) ? static_cast<int>(v) : -1;
		if (have.count(d.state) != 0) {
			continue;  // one entry per state
		}
		have.insert(d.state);
		d.skill = Get(list + ListSkill, v) && v < 0x10000 ? static_cast<int>(v) : -1;
		d.skillLevel = Get(list + ListSkillLevel, v) && v < 256 ? static_cast<int>(v) : -1;
		d.endFrame = Get(list + ListEndFrame, v) ? v : 0;
		d.stats = ListStatsOf(list);
		out.push_back(std::move(d));
	}
	const auto now = std::chrono::steady_clock::now();
	const std::lock_guard<std::mutex> guard(g_stateLock);
	for (auto it = g_stateSince.begin(); it != g_stateSince.end();) {
		const bool still = it->first.first != unit.id || have.count(it->first.second) != 0;
		it = still ? std::next(it) : g_stateSince.erase(it);
	}
	for (StateDetails& d : out) {
		auto [it, added] = g_stateSince.try_emplace({ unit.id, d.state }, now);
		d.activeSeconds = std::chrono::duration<double>(now - it->second).count();
	}
	return out;
}

auto HasState(const Unit& unit, int state) -> bool {
	for (const StateDetails& d : States(unit)) {
		if (d.state == state) {
			return true;
		}
	}
	return false;
}

auto StatesReport(const Unit& unit) -> std::string {
	uintptr_t main = 0;
	const std::vector<uintptr_t> lists = StateLists(unit, main);
	char line[200];
	std::snprintf(line, sizeof(line), "stat list %llx, %zu state lists", static_cast<unsigned long long>(main), lists.size());
	std::string text = line;
	for (const uintptr_t list : lists) {
		uint32_t w[12] {};
		Get(list + 0x10, w);
		std::snprintf(line, sizeof(line), "; %llx: words10..3c", static_cast<unsigned long long>(list - main));
		text += line;
		for (const uint32_t x : w) {
			text += " " + std::to_string(x);
		}
	}
	// The state bit flags: the map tools read them near list + 0xAC8; print set bits nearby.
	for (const std::size_t at : { 0xAC8, 0xAE8, 0xB08 }) {
		uint32_t flags[8] {};
		if (!main || !Get(main + at, flags)) {
			continue;
		}
		std::snprintf(line, sizeof(line), "; flags @%zx:", at);
		text += line;
		for (int i = 0; i < 8 * 32; ++i) {
			if (flags[i / 32] & (1u << (i % 32))) {
				text += " " + std::to_string(i);
			}
		}
	}
	return text;
}

namespace {

struct Seen {
	Unit unit;
	bool fresh = false;  // seen in this poll
};

std::mutex                                       g_pollLock;
bool                                             g_polled = false;
std::map<std::pair<uint32_t, uint32_t>, Seen>    g_seen;  // (kind, unit number) -> last seen
uint32_t                                         g_heroId = 0;
uint32_t                                         g_heroMode = 0;
int                                              g_heroArea = -1, g_heroLife = -1;
std::set<int>                                    g_heroStates;

}  // namespace

auto Poll() -> std::vector<Event> {
	const std::lock_guard<std::mutex> guard(g_pollLock);
	std::vector<Event> events;
	const Unit hero = Hero();
	if (!hero) {
		g_polled = false;
		g_seen.clear();
		return events;
	}
	const bool first = !g_polled || hero.id != g_heroId;
	const int area = GameState::Level(hero);
	const int life = Stat(hero, StatId::Life, 0, false).value_or(-1);
	if (!first) {
		if (!DeadMode(Kind::Player, g_heroMode) && Dead(hero)) {
			events.push_back({ EventType::HeroDied, hero, static_cast<int>(g_heroMode), static_cast<int>(hero.mode) });
		}
		if (area != g_heroArea && area >= 0) {
			events.push_back({ EventType::HeroAreaChanged, hero, g_heroArea, area });
		}
		if (life >= 0 && g_heroLife >= 0 && life < g_heroLife) {
			events.push_back({ EventType::HeroHit, hero, g_heroLife, life });
		}
	}
	std::set<int> states;
	for (const StateDetails& d : States(hero)) {
		states.insert(d.state);
	}
	if (!first) {
		for (const int st : states) {
			if (g_heroStates.count(st) == 0) {
				events.push_back({ EventType::HeroStateOn, hero, 0, st });
			}
		}
		for (const int st : g_heroStates) {
			if (states.count(st) == 0) {
				events.push_back({ EventType::HeroStateOff, hero, st, 0 });
			}
		}
	}
	g_heroStates = std::move(states);
	g_heroId = hero.id, g_heroMode = hero.mode, g_heroArea = area >= 0 ? area : g_heroArea, g_heroLife = life;
	for (auto& [key, seen] : g_seen) {
		seen.fresh = false;
	}
	for (const Kind kind : { Kind::Monster, Kind::Object, Kind::Item }) {
		for (const Unit& u : GameState::Units(kind)) {
			const auto key = std::make_pair(static_cast<uint32_t>(kind), u.id);
			auto it = g_seen.find(key);
			if (it == g_seen.end()) {
				if (!first) {
					events.push_back({ EventType::UnitAppeared, u, 0, static_cast<int>(u.mode) });
				}
				g_seen[key] = { u, true };
				continue;
			}
			const Unit& was = it->second.unit;
			if (!first && was.mode != u.mode) {
				if (kind == Kind::Monster && !Dead(was) && Dead(u)) {
					events.push_back({ EventType::MonsterDied, u, static_cast<int>(was.mode), static_cast<int>(u.mode) });
				} else if (kind == Kind::Object) {
					events.push_back({ EventType::ObjectUsed, u, static_cast<int>(was.mode), static_cast<int>(u.mode) });
				}
			}
			it->second = { u, true };
		}
	}
	for (auto it = g_seen.begin(); it != g_seen.end();) {
		if (!it->second.fresh) {
			if (!first) {
				events.push_back({ EventType::UnitGone, it->second.unit, 0, 0 });
			}
			it = g_seen.erase(it);
		} else {
			++it;
		}
	}
	g_polled = true;
	return events;
}

auto EventName(EventType type) -> const char* {
	switch (type) {
		case EventType::UnitAppeared: return "appeared";
		case EventType::UnitGone: return "gone";
		case EventType::MonsterDied: return "monster died";
		case EventType::ObjectUsed: return "object used";
		case EventType::HeroDied: return "hero died";
		case EventType::HeroAreaChanged: return "hero changed area";
		case EventType::HeroHit: return "hero hit";
		case EventType::HeroStateOn: return "hero state on";
		case EventType::HeroStateOff: return "hero state off";
	}
	return "event";
}

}  // namespace Game
