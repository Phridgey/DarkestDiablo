// The game-state API: what the mod can know about the running game, at any moment, from any
// thread. Built on the live unit list (gamestate.h). READ ONLY: nothing here writes to the game
// or changes its code (CLAUDE.md); frozen build only. Reference: docs/game-state-api.md.
//
//   Game::Hero()                         the hero (unit), Game::HeroInfo() everything about it
//   Game::Stat(unit, Game::StatId::...)  any stat of a player or monster
//   Game::Monsters(), Monster(unit)      monsters near the hero, champion/unique/minion, life
//   Game::Objects(), Items()             map objects (fire, chests, shrines...) and items
//   Game::World()                        area, act, difficulty
//   Game::Poll()                         what changed since the last call (hero died, object
//                                        used, monster died, unit appeared/left, area changed)
//
// Values marked "checked" in docs/game-state-api.md were compared with the game; the others
// come from the community's maps of the game (MapAssist, D2RMH) and dd-state prints them for
// checking.
#pragma once

#include "gamestate.h"

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
	BlockSpeed = 102, CastSpeed = 105,
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

// ---- The world ---------------------------------------------------------------------------

struct WorldDetails {
	int area = -1;        // levels.txt Id the hero stands in (checked: 1 = Rogue Encampment)
	int act = -1;         // 0 = Act I
	int difficulty = -1;  // 0 Normal, 1 Nightmare, 2 Hell
};
auto World() -> WorldDetails;

// ---- Monsters ----------------------------------------------------------------------------

struct MonsterDetails {
	Unit unit;
	bool alive = false;
	bool champion = false, unique = false, superUnique = false, minion = false, possessed = false, ghostly = false;
	int  superUniqueId = -1;        // superuniques.txt row, when superUnique
	std::vector<int> enchantments;  // monumod.txt rows (extra fast, fire enchanted...)
	int  life = -1, maxLife = -1;
};
auto Monster(const Unit& unit) -> MonsterDetails;

// Monsters within `radius` sub-tiles of the hero (all loaded ones when radius < 0), nearest first.
auto Monsters(int radius = 60, bool aliveOnly = true) -> std::vector<MonsterDetails>;

// ---- Objects -----------------------------------------------------------------------------

// Objects within `radius` sub-tiles of the hero, nearest first; row = objects.txt row or AnyRow.
// unit.mode: 0 idle, 1 being used, 2 used/open (chests, shrines, doors).
auto Objects(int radius = 60, uint32_t row = GameState::AnyRow) -> std::vector<Unit>;

// ---- Items -------------------------------------------------------------------------------

enum class ItemPlace : int { Stored = 0, Equipped = 1, Belt = 2, Ground = 3, Cursor = 4, Dropping = 5, Socketed = 6, Unknown = -1 };

struct ItemDetails {
	Unit      unit;          // unit.row: the item's row across weapons/armor/misc.txt
	ItemPlace place = ItemPlace::Unknown;  // from the item's mode
	int       quality = -1;  // 1 low, 2 normal, 3 superior, 4 magic, 5 set, 6 rare, 7 unique, 8 crafted
	uint32_t  ownerId = 0;   // the unit number of who holds it (0 on the ground)
	int       itemLevel = -1;
	uint32_t  flags = 0;     // 0x10 identified, 0x800 socketed, 0x400000 ethereal (classic flags)
};
auto Item(const Unit& unit) -> ItemDetails;

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
};

struct Event {
	EventType type;
	Unit      unit;      // the unit now (or as last seen, for UnitGone)
	int       before = 0;
	int       after = 0; // mode, area or life before/after, by type
};

// What changed since the last call (the first call only takes a snapshot). Call it regularly,
// e.g. every 100-250 ms, from one place; Campfire's "dd-state watch" logs it.
auto Poll() -> std::vector<Event>;

auto EventName(EventType type) -> const char*;

}  // namespace Game
