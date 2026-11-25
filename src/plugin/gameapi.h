// The game-state API: what the mod can know about the running game, at any moment, from any
// thread. Built on the live unit list (gamestate.h). READ ONLY: nothing here writes to the game
// or changes its code (CLAUDE.md); frozen build only. Reference: docs/game-state-api.md.
//
//   Game::Hero()                         the hero (unit), Game::HeroInfo() everything about it
//   Game::Stat(unit, Game::StatId::...)  any stat of a player or monster
//   Game::Monsters(), Monster(unit)      monsters near the hero, champion/unique/minion, life
//   Game::Objects(), Items()             map objects (fire, chests, shrines...) and items
//   Game::World()                        area, act, difficulty
//   Game::States(unit), HasState()       buffs and debuffs (states.txt rows), their stats and end
//   Game::HeroSkills()                   the hero's skills, left and right skill
//   Game::Mercenary(), Players()         the hired mercenary; every player and corpse
//   Game::HeroItems()                    what the hero wears and carries (stash, cube, belt)
//   Game::ClassOf(row), KindOf(row)      enemy / NPC / mercenary / summon / critter;
//                                        shrine / chest / waypoint / portal / door / well
//   Game::Subscribe(), Counters()        the shared event feed; run and stop counters (kills...)
//   Game::Poll()                         what changed since the last call (hero died, object
//                                        used, monster died, unit appeared/left, area changed)
//
// How to use it (and keep it current): docs/game-state-api.md. Values marked "checked" there were compared with the game; the others
// come from the community's maps of the game (MapAssist, D2RMH) and dd-state prints them for
// checking.
#pragma once

#include "gamestate.h"
#include "gametables.h"  // generated into build/plugin-gen by tools/gen_game_tables.py

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Game {

using GameState::Kind;
using GameState::Unit;

// itemstatcost.txt rows the mod is likely to want (any row works with Stat()).
namespace StatId {
enum : uint16_t {
	Strength = 0, Energy = 1, Dexterity = 2, Vitality = 3, StatPoints = 4, SkillPoints = 5,
	Life = 6, MaxLife = 7, Mana = 8, MaxMana = 9, Stamina = 10, MaxStamina = 11,  // these six in 256ths
	Level = 12, Experience = 13, Gold = 14, StashGold = 15,
	Defense = 31, DamageReduced = 34, MagicDamageReduced = 35, DamageResist = 36, MagicResist = 37,
	FireResist = 39, MaxFireResist = 40, LightningResist = 41, MaxLightningResist = 42,
	ColdResist = 43, MaxColdResist = 44, PoisonResist = 45, MaxPoisonResist = 46,
	GoldFind = 79, MagicFind = 80, AttackSpeed = 93, MoveSpeed = 96, HitRecovery = 99,
	BlockSpeed = 102, CastSpeed = 105, VelocityPercent = 67, AttackRate = 68,
};
}

// A stat's value now. withItems: with items, skills and auras added (what the character screen
// shows); false: the unit's own. Life, mana and stamina come back whole (not in 256ths).
// No such stat: nothing.
auto Stat(const Unit& unit, uint16_t stat, uint16_t param = 0, bool withItems = true) -> std::optional<int>;

// ---- The hero ----------------------------------------------------------------------------

// The SDK's own number for the local player (GameplayEvent::playerId). Campfire passes it on;
// with it Hero() is exact, without it Hero() uses the Campfire's town search.
void SetLocalPlayerId(uint32_t id);

// The hero now. No game or not found: Unit().
auto Hero() -> Unit;

struct HeroDetails {
	Unit        unit;
	std::string name;
	int         classId = -1;   // 0 Amazon, 1 Sorceress, 2 Necromancer, 3 Paladin, 4 Barbarian, 5 Druid, 6 Assassin
	int         level = -1;
	int         life = -1, maxLife = -1, mana = -1, maxMana = -1;
	long long   experience = -1;
	int         gold = -1, stashGold = -1;
	int         area = -1;      // levels.txt Id
	bool        dead = false;
};
auto HeroInfo() -> HeroDetails;

// A player's mode by name: 0 dying, 1 idle, 2 walk, 3 run, 4 hit, 5 town idle, 6 town walk,
// 7-8 attack, 9 block, 10 cast, 11 throw, 12 kick, 13-16 skill, 17 dead, 18 sequence, 19 knocked back.
auto PlayerModeName(uint32_t mode) -> const char*;

// True while the hero is fighting: attacking, casting, blocking, kicking, hit or knocked back.
auto InCombatMode(uint32_t mode) -> bool;

// ---- Skills ------------------------------------------------------------------------------

struct SkillDetails {
	int id = -1;         // skills.txt row (0 Attack, 36 Fire Bolt...)
	int baseLevel = -1;  // points the hero put in it (not counting items); -1 unknown
};

struct SkillSet {
	std::vector<SkillDetails> skills;  // every skill the hero has, the basic ones (Attack, Throw...) too
	int left = -1, right = -1;         // the skill on each mouse button (skills.txt rows)
	bool found = false;                // false: the skill list wasn't found on this build
};

// The hero's skills. The skill list is found by its shape the first time (docs/game-state-api.md).
auto HeroSkills() -> SkillSet;

// The skill list of one copy of a player (GameWrite::Copies): each skill's row and where it lies, and
// where the list keeps its right-button skill. Not found: nothing. For gamewrite.cpp.
struct SkillPlaces {
	std::vector<std::pair<int, uintptr_t>> skills;
	uintptr_t rightAt = 0;  // address of the pointer
	uintptr_t right = 0;    // what it points at
	auto Find(int row) const -> uintptr_t;  // 0: the copy has no such skill
};
auto SkillsOf(uintptr_t copy) -> std::optional<SkillPlaces>;

// Research for "dd-state skills": where the skill list was found and each skill's raw numbers.
auto SkillsReport() -> std::string;

// ---- Players -----------------------------------------------------------------------------

struct PlayerDetails {
	Unit        unit;
	std::string name;
	int         classId = -1;
	int         level = -1;
	bool        hero = false;    // the local player
	bool        dead = false;
	bool        corpse = false;  // a dead player unit that is not the hero (the body left after a death)
};

// Every player unit loaded: the hero, its corpse, other players in a multiplayer game.
auto Players() -> std::vector<PlayerDetails>;

// ---- Buffs and debuffs (states) -----------------------------------------------------------

// One state on a unit (states.txt row): a buff, debuff, curse or aura effect. Timed states
// (Burst of Speed, Slowed, curses, poison...) each carry their own stat list with the stats they
// give and the game frame they end on (25 frames a second). mapped: the layout is the classic
// game's, unchecked on D2R; "dd-state states" prints it for checking.
struct StateDetails {
	int      state = -1;        // states.txt row
	int      skill = -1;        // skills.txt row that gave it, -1 unknown
	int      skillLevel = -1;
	uint32_t endFrame = 0;      // game frame it ends on; 0: no end (auras, permanent)
	double   activeSeconds = 0; // how long it has been on (seen by Poll/States calls)
	std::vector<GameState::StatEntry> stats;  // what it adds (e.g. velocitypercent -40)
};

// The unit's states now (hero by default).
auto States(const Unit& unit) -> std::vector<StateDetails>;
auto HasState(const Unit& unit, int state) -> bool;

// Research for "dd-state states": raw lines on what the unit's stat list links to.
auto StatesReport(const Unit& unit) -> std::string;

// ---- The world ---------------------------------------------------------------------------

struct WorldDetails {
	int area = -1;        // levels.txt Id the hero stands in (checked: 1 = Rogue Encampment)
	int act = -1;         // 0 = Act I
	int difficulty = -1;  // 0 Normal, 1 Nightmare, 2 Hell
};
auto World() -> WorldDetails;

// ---- Monsters ----------------------------------------------------------------------------

// From monstats.txt (gametables.h): Enemy, Npc (town people, guards), Mercenary (hired
// fighters), Summon (golems, skeletons, wolves, valkyrie...), Critter (rats, birds, cows).
auto ClassOf(uint32_t monsterRow) -> MonsterClass;
auto ClassName(MonsterClass c) -> const char*;

struct MonsterDetails {
	Unit unit;
	MonsterClass cls = MonsterClass::Enemy;
	bool alive = false;
	bool champion = false, unique = false, superUnique = false, minion = false, possessed = false, ghostly = false;
	int  superUniqueId = -1;        // superuniques.txt row, when superUnique
	std::vector<int> enchantments;  // monumod.txt rows (extra fast, fire enchanted...)
	int  life = -1, maxLife = -1;
};
auto Monster(const Unit& unit) -> MonsterDetails;

// Monsters within `radius` sub-tiles of the hero (all loaded ones when radius < 0), nearest first.
auto Monsters(int radius = 60, bool aliveOnly = true) -> std::vector<MonsterDetails>;

// Only the ones the hero fights (MonsterClass::Enemy), alive, nearest first.
auto Enemies(int radius = 60) -> std::vector<MonsterDetails>;

// The hero's mercenary (single player: the one hired fighter loaded), alive or dead. None: nothing.
auto Mercenary() -> std::optional<MonsterDetails>;

// ---- Objects -----------------------------------------------------------------------------

// Objects within `radius` sub-tiles of the hero, nearest first; row = objects.txt row or AnyRow.
// unit.mode: 0 idle, 1 being used, 2 used/open (chests, shrines, doors).
auto Objects(int radius = 60, uint32_t row = GameState::AnyRow) -> std::vector<Unit>;

// From objects.txt (gametables.h): shrine, chest (also urns, caskets, bookshelves), waypoint,
// portal (town portals, red portals), door, well, stash, town fire, other.
auto KindOf(uint32_t objectRow) -> ObjectKind;
auto KindName(ObjectKind k) -> const char*;

// Objects of one kind within `radius` sub-tiles of the hero, nearest first.
auto Objects(int radius, ObjectKind kind) -> std::vector<Unit>;

// True once a chest, shrine, door or well has been used (its mode is 2).
inline auto Used(const Unit& object) -> bool { return object.mode == 2; }

// ---- Items -------------------------------------------------------------------------------

enum class ItemPlace : int { Stored = 0, Equipped = 1, Belt = 2, Ground = 3, Cursor = 4, Dropping = 5, Socketed = 6, Unknown = -1 };

struct ItemDetails {
	Unit      unit;          // unit.row: the item's row across weapons/armor/misc.txt
	ItemPlace place = ItemPlace::Unknown;  // from the item's mode
	int       quality = -1;  // 1 low, 2 normal, 3 superior, 4 magic, 5 set, 6 rare, 7 unique, 8 crafted
	uint32_t  ownerId = 0;   // the unit number of who holds it (0 on the ground)
	int       itemLevel = -1;
	uint32_t  flags = 0;     // 0x10 identified, 0x800 socketed, 0x400000 ethereal (classic flags)
	int       bodySlot = -1; // equipped: 1 head, 2 amulet, 3 body, 4 right hand, 5 left hand, 6 right ring,
	                         // 7 left ring, 8 belt, 9 boots, 10 gloves, 11-12 weapon swap
	int       page = -1;     // stored: 0 inventory, 3 cube, 4 stash
	auto Identified() const -> bool { return (flags & 0x10) != 0; }
	auto Socketed() const -> bool { return (flags & 0x800) != 0; }
	auto Ethereal() const -> bool { return (flags & 0x400000) != 0; }
};
auto Item(const Unit& unit) -> ItemDetails;

// Everything the hero owns that is loaded: worn, in the inventory, cube, stash and belt, and on
// the cursor. With `place`, only that place (e.g. ItemPlace::Belt for the potions in the belt).
auto HeroItems() -> std::vector<ItemDetails>;
auto HeroItems(ItemPlace place) -> std::vector<ItemDetails>;

// Items on the ground within `radius` sub-tiles of the hero, nearest first.
auto GroundItems(int radius = 60) -> std::vector<ItemDetails>;

// ---- Changes -----------------------------------------------------------------------------

enum class EventType {
	UnitAppeared,    // a monster, object or item came into the loaded area
	UnitGone,        // it left (out of range, picked up, removed)
	MonsterDied,     // a monster's mode turned to dying or dead
	ObjectUsed,      // an object's mode changed (chest opened, shrine taken, door opened)
	HeroDied,        // the hero's mode turned to dying or dead
	HeroAreaChanged, // the hero stands in another levels.txt area
	HeroHit,         // the hero's life went down
	HeroStateOn,     // the hero gained a state (buff/debuff); after = states.txt row
	HeroStateOff,    // a state ended; before = states.txt row
	HeroHealed,      // the hero's life went up (before/after: life)
	HeroLevelUp,     // the hero's level went up (before/after: level)
	HeroGoldChanged, // the gold the hero carries changed (before/after: gold)
	SkillChanged,    // the right mouse skill changed (before/after: skills.txt rows)
	MercDied,        // the mercenary died (instead of MonsterDied)
};

struct Event {
	EventType type;
	Unit      unit;      // the unit now (or as last seen, for UnitGone)
	int       before = 0;
	int       after = 0; // mode, area or life before/after, by type
};

// ---- The shared event feed ---------------------------------------------------------------
// One place looks for changes: Game::Tick(), which the plugin calls every 250 ms on the game's
// UI thread from the moment it loads (campfire.cpp, TrackTick). Each tick compares the game
// with the last tick, updates the run counters (below) and hands the new events to every
// subscriber. Any number of features can subscribe; none of them miss events another one saw.

using EventCallback = void (*)(const Event& event, void* userData);

// Calls `callback` for every event from now on, on the UI thread, right after each tick.
// Keep it quick (no waiting, no file writes in a loop). Returns an id for Unsubscribe; 0 = refused.
auto Subscribe(EventCallback callback, void* userData = nullptr) -> int;
void Unsubscribe(int id);

// The tick itself. Only the plugin's timer calls it (campfire.cpp); features subscribe instead.
void Tick();

// Older way to read events, still working: everything that happened since the previous Poll()
// call. Only one place should call it, because it empties its queue. When the tick isn't
// running (e.g. a test without the plugin's timer), Poll() looks for changes itself.
auto Poll() -> std::vector<Event>;

auto EventName(EventType type) -> const char*;

// ---- Run counters ------------------------------------------------------------------------
// Kept by the API from the event feed, so every feature reads the same numbers. "Run" counts
// from ResetCounters() (the Campfire calls it when a game starts with a hero); "stop" counts
// from the last time the hero entered a new area (or ResetStop()).

struct Tally {
	int       kills = 0;          // enemies killed (MonsterClass::Enemy; not NPCs, summons, critters)
	int       killsNormal = 0;    // ... of them: normal monsters and minions
	int       killsChampion = 0;  // champions
	int       killsUnique = 0;    // uniques and super uniques (not bosses)
	int       killsBoss = 0;      // monstats.txt "boss" rows: act bosses, Blood Raven, Griswold...
	int       heroDeaths = 0;
	int       mercDeaths = 0;
	long long goldGained = 0;     // rises in gold carried + stashed (picked up, sold, rewards); spending doesn't subtract
	int       itemsFound = 0;     // items that newly belong to the hero (picked up, bought, rewards)
	int       levelsGained = 0;
	int       shrinesUsed = 0;
	int       chestsOpened = 0;   // chests, urns, caskets and other containers (ObjectKind::Chest)
	double    seconds = 0;        // wall-clock time since the reset (the game's pause menu still counts)
};

struct CounterSet {
	Tally run;
	Tally stop;
	int   stopArea = -1;  // levels.txt Id of the current stop
};

auto Counters() -> CounterSet;

// Starts the run (and the stop) from zero. Items the hero owns now don't count as found.
void ResetCounters();

// Starts the stop from zero (also happens by itself when the hero enters another area).
void ResetStop();

}  // namespace Game
