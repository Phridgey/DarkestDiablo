// Live game state: every player, monster, object (the town fire, chests, shrines, doors...),
// missile, item and map tile the game has loaded, read straight from the game's unit list.
// READ ONLY: nothing here writes to game memory or changes game code (CLAUDE.md). Reads use
// Windows' ReadProcessMemory on our own process, which reports a failed read instead of crashing.
// Every value read and where it comes from is in docs/game-memory.md.
//
// How the unit list is found (once per game start, a few milliseconds):
//   1. The game code that looks units up reads them from "list start + slot * 8". That
//      instruction holds the list's distance from the game module's start. The reader looks for
//      such instructions in the game's code and keeps the address whose contents look like a
//      unit list. This is how the community's map tools (MapAssist, D2RMH) find it, so it works
//      on any PC and after the loader is updated.
//   2. If that finds nothing: the place saved by an earlier game (darkestdiablo-unitlist.txt
//      next to the plugin), then the place found on our own PCs.
// It is off unless D2R.exe is the frozen build in D2R_VERSION (GameMemory::BuildMatches).
//
// Use from any thread. Every call reads fresh values; nothing is cached but the list's place.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace GameState {

// The game's unit kinds; the number is also the kind's block in the unit list.
enum class Kind : uint32_t { Player = 0, Monster = 1, Object = 2, Missile = 3, Item = 4, Tile = 5 };

struct Unit {
	uintptr_t address = 0;    // 0: no unit
	Kind      kind = Kind::Player;
	uint32_t  row = 0;        // class (players), monstats.txt / objects.txt / missiles.txt row
	uint32_t  id = 0;         // the game's number for this unit
	uint32_t  mode = 0;       // what it is doing (monsters: 0 dying, 12 dead; objects: 0 idle, 1 opening, 2 open)
	int       x = 0, y = 0;   // world sub-tiles, as "location player" prints them
	bool      placed = false; // x, y read (false for items in an inventory)
	std::string name;         // players only: the hero's name

	explicit operator bool() const { return address != 0; }
	auto Alive() const -> bool { return kind != Kind::Monster || (mode != 0 && mode != 12); }
};

// True once the unit list is found. The first call (in game, with units loaded) looks for it;
// later calls are free. At the menus there are no units, so it stays false until a game starts.
auto Ready() -> bool;

// Looks for the unit list again (after a failure, or for the dd-units test command).
auto Find() -> bool;

// One line on how the unit list was found, for the log.
auto Status() -> std::string;

// Every unit of one kind (empty when the list isn't found).
auto Units(Kind kind) -> std::vector<Unit>;

// Units of one kind and row (e.g. Kind::Object, 39: every town fire loaded).
auto Units(Kind kind, uint32_t row) -> std::vector<Unit>;

// The hero. With a name, the player unit of that name; without, the first player unit.
auto Player(const std::string& name = {}) -> Unit;

// The unit of one kind (and row, unless row is AnyRow) nearest to x, y. No unit: Unit().
constexpr uint32_t AnyRow = 0xFFFFFFFF;
auto Nearest(Kind kind, uint32_t row, int x, int y) -> Unit;

// Reads a unit again (its mode and position now). Gone: Unit().
auto Refresh(const Unit& unit) -> Unit;

// Hit points now (players and monsters), or -1 when unknown.
auto Life(const Unit& unit) -> int;

// What the unit's stat list holds, for the log when Life is -1 (dd-units).
auto StatsReport(const Unit& unit) -> std::string;

// The levels.txt Id of the level the unit stands in, or -1 when unknown (checked in town: 1).
auto Level(const Unit& unit) -> int;

// Used by the slow search in gamememory.cpp when it finds the list its own way.
void Adopt(uintptr_t list, std::size_t next);

// The kind's name for logs and the console ("player", "monster", ...).
auto KindName(Kind kind) -> const char*;

}  // namespace GameState
