// See gameapi.h and docs/game-state-api.md. READ ONLY.
#include "gameapi.h"
#include <atomic>

#include "gamememory.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <optional>
#include <string>
#include <chrono>
#include <cstdio>
#include <set>
#include <atomic>
#include <cmath>
#include <deque>
#include <functional>
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
constexpr std::size_t ItemBodySlot    = 0x54;  // uint8: where it is worn (equipped items)
constexpr std::size_t ItemPage        = 0x55;  // uint8: 0 inventory, 3 cube, 4 stash (stored items)
constexpr std::size_t UnitSkillsGuess = 0x100; // pointer: the skill list (map tools); checked by shape
constexpr std::size_t SkillNext       = 0x08;  // skill -> the next skill of the list
constexpr std::size_t SkillHardPoints = 0x40;  // uint32: points put in it (map tools; see SkillsReport)
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

auto Pointer(uintptr_t p) -> bool {
	return p >= 0x10000 && p < 0x7FFFFFFF0000ULL && (p & 7) == 0;
}

// ---- The skill list, found by its shape --------------------------------------------------
// The hero's unit points at a skill list: { first skill, left skill, right skill, used skill }.
// Each skill points at its skills.txt record (which starts with the skill's row number) and at
// the next skill. The map tools put the list at unit + 0x100; our build moved other things
// (the stat list), so every pointer in the unit's first 0x200 bytes is tried, 0x100 first. The
// list must hold Attack (row 0), have no repeats, and hold the left and right skills.

struct SkillChain {
	std::vector<uintptr_t> nodes;
	std::vector<int>       ids;
};

auto ReadSkillChain(uintptr_t first, SkillChain& chain) -> bool {
	for (uintptr_t skill = first; skill != 0;) {
		uintptr_t txt = 0, next = 0;
		uint16_t id = 0;
		if (!Pointer(skill) || chain.nodes.size() >= 400 || !Get(skill, txt) || !Pointer(txt) || !Get(txt, id) || id >= 2048 ||
			!Get(skill + SkillNext, next) || std::find(chain.ids.begin(), chain.ids.end(), id) != chain.ids.end()) {
			return false;
		}
		chain.nodes.push_back(skill);
		chain.ids.push_back(id);
		skill = next;
	}
	return !chain.nodes.empty();
}

auto LooksLikeSkillList(uintptr_t list, SkillChain& chain) -> bool {
	std::array<uintptr_t, 3> head {};
	if (!Pointer(list) || !Get(list, head) || !ReadSkillChain(head[0], chain) || chain.ids.size() < 3 ||
		std::find(chain.ids.begin(), chain.ids.end(), 0) == chain.ids.end()) {
		return false;
	}
	for (const uintptr_t hand : { head[1], head[2] }) {
		if (hand != 0 && std::find(chain.nodes.begin(), chain.nodes.end(), hand) == chain.nodes.end()) {
			return false;
		}
	}
	return true;
}

std::atomic<int> g_skillsAt { -1 };  // where in the unit the skill list pointer sits, once found

auto FindSkillList(const Unit& hero, SkillChain& chain, uintptr_t& list) -> bool {
	const auto tryAt = [&](std::size_t offset) {
		chain = {};
		return Get(hero.address + offset, list) && LooksLikeSkillList(list, chain);
	};
	if (const int at = g_skillsAt.load(); at >= 0 && tryAt(static_cast<std::size_t>(at))) {
		return true;
	}
	if (tryAt(UnitSkillsGuess)) {
		g_skillsAt = static_cast<int>(UnitSkillsGuess);
		return true;
	}
	for (std::size_t offset = 0x18; offset < 0x200; offset += 8) {
		if (offset != UnitSkillsGuess && tryAt(offset)) {
			g_skillsAt = static_cast<int>(offset);
			return true;
		}
	}
	return false;
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

auto PlayerModeName(uint32_t mode) -> const char* {
	static const char* const names[] = { "dying", "idle", "walk", "run", "hit", "town idle", "town walk", "attack", "attack", "block",
		"cast", "throw", "kick", "skill", "skill", "skill", "skill", "dead", "sequence", "knocked back" };
	return mode < std::size(names) ? names[mode] : "unknown";
}

auto InCombatMode(uint32_t mode) -> bool {
	return mode == 4 || (mode >= 7 && mode <= 16) || mode == 19;
}

static std::atomic<uint32_t> g_notEnemies[16] {};  // row + 1; 0: free
void NotAnEnemy(uint32_t row) {
	for (auto& slot : g_notEnemies) {
		uint32_t free = 0;
		if (slot.compare_exchange_strong(free, row + 1) || free == row + 1) {
			return;
		}
	}
}

auto ClassOf(uint32_t row) -> MonsterClass {
	for (const auto& slot : g_notEnemies) {
		if (slot.load() == row + 1) {
			return MonsterClass::Critter;
		}
	}
	return row < std::size(Tables::MonsterClasses) ? Tables::MonsterClasses[row] : MonsterClass::Enemy;
}

auto ClassName(MonsterClass c) -> const char* {
	switch (c) {
		case MonsterClass::Enemy: return "enemy";
		case MonsterClass::Npc: return "npc";
		case MonsterClass::Mercenary: return "mercenary";
		case MonsterClass::Summon: return "summon";
		case MonsterClass::Critter: return "critter";
	}
	return "monster";
}

auto KindOf(uint32_t row) -> ObjectKind {
	return row < std::size(Tables::ObjectKinds) ? Tables::ObjectKinds[row] : ObjectKind::Other;
}

auto KindName(ObjectKind k) -> const char* {
	switch (k) {
		case ObjectKind::Other: return "other";
		case ObjectKind::Shrine: return "shrine";
		case ObjectKind::Chest: return "chest";
		case ObjectKind::Waypoint: return "waypoint";
		case ObjectKind::Portal: return "portal";
		case ObjectKind::Door: return "door";
		case ObjectKind::Well: return "well";
		case ObjectKind::Stash: return "stash";
		case ObjectKind::Fire: return "fire";
	}
	return "object";
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

auto HeroSkills() -> SkillSet {
	SkillSet set;
	const Unit hero = Hero();
	SkillChain chain;
	uintptr_t list = 0;
	if (!hero || !FindSkillList(hero, chain, list)) {
		return set;
	}
	set.found = true;
	for (std::size_t i = 0; i < chain.nodes.size(); ++i) {
		uint32_t points = 0;
		SkillDetails skill;
		skill.id = chain.ids[i];
		if (Get(chain.nodes[i] + SkillHardPoints, points) && points <= 99) {
			skill.baseLevel = static_cast<int>(points);
		}
		set.skills.push_back(skill);
	}
	std::array<uintptr_t, 3> head {};
	if (Get(list, head)) {
		for (std::size_t i = 0; i < chain.nodes.size(); ++i) {
			set.left = chain.nodes[i] == head[1] ? chain.ids[i] : set.left;
			set.right = chain.nodes[i] == head[2] ? chain.ids[i] : set.right;
		}
	}
	return set;
}

auto SkillsReport() -> std::string {
	const Unit hero = Hero();
	SkillChain chain;
	uintptr_t list = 0;
	if (!hero) {
		return "Skills: no hero.";
	}
	if (!FindSkillList(hero, chain, list)) {
		return "Skills: no skill list found in the hero's first 0x200 bytes.";
	}
	std::array<uintptr_t, 3> head {};
	Get(list, head);
	char text[160];
	std::snprintf(text, sizeof(text), "Skills: list at hero + 0x%X, %zu skills. Each skill row: its 4-byte numbers at +0x20..+0x5C (-1: large):",
		g_skillsAt.load(), chain.nodes.size());
	std::string out = text;
	for (std::size_t i = 0; i < chain.nodes.size() && i < 40; ++i) {
		std::array<uint32_t, 16> raw {};
		Get(chain.nodes[i] + 0x20, raw);
		out += "\n  skill " + std::to_string(chain.ids[i]) + (chain.nodes[i] == head[1] ? " (left)" : "") +
		       (chain.nodes[i] == head[2] ? " (right)" : "") + ":";
		for (const uint32_t v : raw) {
			out += " " + std::to_string(v > 0xFFFF ? -1 : static_cast<int>(v));
		}
	}
	return out;
}

auto Players() -> std::vector<PlayerDetails> {
	std::vector<PlayerDetails> out;
	const Unit hero = Hero();
	for (const Unit& u : GameState::Units(Kind::Player)) {
		if (!u.placed) {
			continue;  // the game's copies of a player
		}
		const bool isHero = hero && u.address == hero.address;
		const auto same = std::find_if(out.begin(), out.end(), [&](const PlayerDetails& p) { return p.unit.id == u.id; });
		if (same != out.end()) {
			if (!isHero) {
				continue;
			}
			out.erase(same);
		}
		PlayerDetails p;
		p.unit = u;
		p.name = u.name;
		p.classId = static_cast<int>(u.row);
		p.level = GameState::CharacterLevel(u);
		p.hero = isHero;
		p.dead = Dead(u);
		p.corpse = p.dead && !isHero;
		out.push_back(std::move(p));
	}
	return out;
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
	m.cls = ClassOf(unit.row);
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

auto Enemies(int radius) -> std::vector<MonsterDetails> {
	std::vector<MonsterDetails> out = Monsters(radius, true);
	out.erase(std::remove_if(out.begin(), out.end(), [](const MonsterDetails& m) { return m.cls != MonsterClass::Enemy; }), out.end());
	return out;
}

auto Mercenary() -> std::optional<MonsterDetails> {
	std::optional<MonsterDetails> best;
	for (const MonsterDetails& m : Monsters(-1, false)) {  // nearest first; a live one wins
		if (m.cls == MonsterClass::Mercenary && (!best || (m.alive && !best->alive))) {
			best = m;
		}
	}
	return best;
}

auto Objects(int radius, ObjectKind kind) -> std::vector<Unit> {
	std::vector<Unit> out = Objects(radius, GameState::AnyRow);
	out.erase(std::remove_if(out.begin(), out.end(), [&](const Unit& u) { return KindOf(u.row) != kind; }), out.end());
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
		uint8_t slot = 0, page = 0;
		if (i.place == ItemPlace::Equipped && Get(data + ItemBodySlot, slot) && slot <= 12) {
			i.bodySlot = slot;
		}
		if (i.place == ItemPlace::Stored && Get(data + ItemPage, page) && page <= 4) {
			i.page = page;
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

auto HeroItems() -> std::vector<ItemDetails> {
	std::vector<ItemDetails> out;
	const Unit hero = Hero();
	if (!hero) {
		return out;
	}
	for (const Unit& u : GameState::Units(Kind::Item)) {
		if (u.mode != 3 && u.mode != 5) {  // not on the ground or falling
			ItemDetails i = Item(u);
			if (i.ownerId == hero.id) {
				out.push_back(i);
			}
		}
	}
	return out;
}

auto HeroItems(ItemPlace place) -> std::vector<ItemDetails> {
	std::vector<ItemDetails> out = HeroItems();
	out.erase(std::remove_if(out.begin(), out.end(), [&](const ItemDetails& i) { return i.place != place; }), out.end());
	return out;
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
int                                              g_heroLevel = -1, g_heroGold = -1, g_rightSkill = -1;
std::set<int>                                    g_heroStates;

}  // namespace

// Compares the game with the previous call: the one place events are made (Update() below).
static auto Diff() -> std::vector<Event> {
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
	const int level = Stat(hero, StatId::Level, 0, false).value_or(-1);
	const int gold = Stat(hero, StatId::Gold, 0, false).value_or(0);
	const int right = HeroSkills().right;
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
		if (life >= 0 && g_heroLife >= 0 && life > g_heroLife) {
			events.push_back({ EventType::HeroHealed, hero, g_heroLife, life });
		}
		if (level > 0 && g_heroLevel > 0 && level > g_heroLevel) {
			events.push_back({ EventType::HeroLevelUp, hero, g_heroLevel, level });
		}
		if (g_heroGold >= 0 && gold != g_heroGold) {
			events.push_back({ EventType::HeroGoldChanged, hero, g_heroGold, gold });
		}
		if (right >= 0 && g_rightSkill >= 0 && right != g_rightSkill) {
			events.push_back({ EventType::SkillChanged, hero, g_rightSkill, right });
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
	g_heroLevel = level, g_heroGold = gold, g_rightSkill = right >= 0 ? right : g_rightSkill;
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
					const EventType died = ClassOf(u.row) == MonsterClass::Mercenary ? EventType::MercDied : EventType::MonsterDied;
					events.push_back({ died, u, static_cast<int>(was.mode), static_cast<int>(u.mode) });
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
		case EventType::HeroHealed: return "hero healed";
		case EventType::HeroLevelUp: return "hero level up";
		case EventType::HeroGoldChanged: return "gold changed";
		case EventType::SkillChanged: return "right skill changed";
		case EventType::MercDied: return "mercenary died";
	}
	return "event";
}

// ---- The shared event feed and the run counters -----------------------------------------

namespace {

using Clock = std::chrono::steady_clock;

struct Subscriber {
	int           id;
	EventCallback callback;
	void*         userData;
};

std::mutex              g_feedLock;
std::vector<Subscriber> g_subscribers;
int                     g_nextId = 1;
std::deque<Event>       g_pollQueue;           // what Poll() hands out next
bool                    g_pollUsed = false;    // Poll() was called at least once
constexpr std::size_t   PollQueueLimit = 4096; // a Poll() caller that stopped calling can't grow it forever
std::atomic<int64_t>    g_lastTick { 0 };      // Clock ticks of the last Tick(), 0: never

std::mutex              g_countLock;
CounterSet              g_counts;
Clock::time_point       g_runStart = Clock::now(), g_stopStart = Clock::now();
uint32_t                g_countHero = 0;       // the hero the counters are primed for, 0: not yet
Clock::time_point       g_primeUntil;          // items seen before this aren't "found" (they load over a moment)
long long               g_goldTotal = -1;      // gold carried + stashed at the last tick
std::set<uint32_t>      g_knownItems;          // the hero's item unit numbers seen this run
std::set<uint32_t>      g_usedObjects;         // shrines and chests already counted this run

auto IsBoss(uint32_t row) -> bool {
	return std::binary_search(std::begin(Tables::BossRows), std::end(Tables::BossRows), static_cast<uint16_t>(row));
}

void Add(const std::function<void(Tally&)>& change) {
	change(g_counts.run);
	change(g_counts.stop);
}

// Counts one event (g_countLock held).
void CountEvent(const Event& e) {
	switch (e.type) {
		case EventType::MonsterDied: {
			if (ClassOf(e.unit.row) != MonsterClass::Enemy) {
				break;
			}
			const MonsterDetails m = Monster(e.unit);
			Add([&](Tally& t) {
				++t.kills;
				if (IsBoss(e.unit.row)) {
					++t.killsBoss;
				} else if (m.unique || m.superUnique) {
					++t.killsUnique;
				} else if (m.champion) {
					++t.killsChampion;
				} else {
					++t.killsNormal;
				}
			});
			break;
		}
		case EventType::MercDied: Add([](Tally& t) { ++t.mercDeaths; }); break;
		case EventType::HeroDied: Add([](Tally& t) { ++t.heroDeaths; }); break;
		case EventType::HeroLevelUp: Add([&](Tally& t) { t.levelsGained += e.after - e.before; }); break;
		case EventType::ObjectUsed: {
			const ObjectKind kind = KindOf(e.unit.row);
			if (e.before != 0 || e.after == 0 || (kind != ObjectKind::Shrine && kind != ObjectKind::Chest) || !g_usedObjects.insert(e.unit.id).second) {
				break;
			}
			Add([&](Tally& t) { ++(kind == ObjectKind::Shrine ? t.shrinesUsed : t.chestsOpened); });
			break;
		}
		case EventType::HeroAreaChanged:
			g_counts.stop = {};
			g_counts.stopArea = e.after;
			g_stopStart = Clock::now();
			break;
		default: break;
	}
}

// Gold and items: compared with the last tick (g_countLock held). The first moments after a
// reset (or a new hero) only take note of what the hero already has.
void CountBelongings(const Unit& hero) {
	const auto now = Clock::now();
	if (hero.id != g_countHero) {
		g_countHero = hero.id;
		g_primeUntil = now + std::chrono::seconds(2);
		g_goldTotal = -1;
		g_knownItems.clear();
		if (g_counts.stopArea < 0) {
			g_counts.stopArea = GameState::Level(hero);
		}
	}
	const bool priming = now < g_primeUntil;
	const long long gold = static_cast<long long>(Stat(hero, StatId::Gold, 0, false).value_or(0)) + Stat(hero, StatId::StashGold, 0, false).value_or(0);
	if (g_goldTotal >= 0 && gold > g_goldTotal && !priming) {
		Add([&](Tally& t) { t.goldGained += gold - g_goldTotal; });
	}
	g_goldTotal = gold;
	int found = 0;
	for (const ItemDetails& i : HeroItems()) {
		found += g_knownItems.insert(i.unit.id).second && !priming ? 1 : 0;
	}
	if (found > 0) {
		Add([&](Tally& t) { t.itemsFound += found; });
	}
}

// One look at the game: events, counters, then the subscribers and the Poll() queue.
void Update() {
	const std::vector<Event> events = Diff();
	{
		const std::lock_guard<std::mutex> guard(g_countLock);
		for (const Event& e : events) {
			CountEvent(e);
		}
		if (const Unit hero = Hero()) {
			CountBelongings(hero);
		}
	}
	if (events.empty()) {
		return;
	}
	std::vector<Subscriber> subscribers;
	{
		const std::lock_guard<std::mutex> guard(g_feedLock);
		subscribers = g_subscribers;
		if (g_pollUsed) {
			for (const Event& e : events) {
				if (g_pollQueue.size() >= PollQueueLimit) {
					g_pollQueue.pop_front();
				}
				g_pollQueue.push_back(e);
			}
		}
	}
	for (const Subscriber& s : subscribers) {
		for (const Event& e : events) {
			try {
				s.callback(e, s.userData);
			} catch (...) {
			}
		}
	}
}

auto Ticking() -> bool {
	const int64_t last = g_lastTick.load();
	return last != 0 && Clock::now().time_since_epoch().count() - last < std::chrono::duration_cast<Clock::duration>(std::chrono::seconds(2)).count();
}

}  // namespace

auto Subscribe(EventCallback callback, void* userData) -> int {
	if (callback == nullptr) {
		return 0;
	}
	const std::lock_guard<std::mutex> guard(g_feedLock);
	g_subscribers.push_back({ g_nextId, callback, userData });
	return g_nextId++;
}

void Unsubscribe(int id) {
	const std::lock_guard<std::mutex> guard(g_feedLock);
	g_subscribers.erase(std::remove_if(g_subscribers.begin(), g_subscribers.end(), [&](const Subscriber& s) { return s.id == id; }), g_subscribers.end());
}

void Tick() {
	g_lastTick = Clock::now().time_since_epoch().count();
	Update();
}

auto Poll() -> std::vector<Event> {
	{
		const std::lock_guard<std::mutex> guard(g_feedLock);
		g_pollUsed = true;
	}
	if (!Ticking()) {
		Update();
	}
	const std::lock_guard<std::mutex> guard(g_feedLock);
	std::vector<Event> out(g_pollQueue.begin(), g_pollQueue.end());
	g_pollQueue.clear();
	return out;
}

auto Counters() -> CounterSet {
	const std::lock_guard<std::mutex> guard(g_countLock);
	CounterSet c = g_counts;
	const auto now = Clock::now();
	c.run.seconds = std::chrono::duration<double>(now - g_runStart).count();
	c.stop.seconds = std::chrono::duration<double>(now - g_stopStart).count();
	return c;
}

void ResetCounters() {
	const std::lock_guard<std::mutex> guard(g_countLock);
	g_counts = {};
	g_runStart = g_stopStart = Clock::now();
	g_countHero = 0;  // the next tick takes note of the hero's gold and items again
	g_usedObjects.clear();
}

void ResetStop() {
	const std::lock_guard<std::mutex> guard(g_countLock);
	const int area = g_counts.stopArea;
	g_counts.stop = {};
	g_counts.stopArea = area;
	g_stopStart = Clock::now();
}

}  // namespace Game
