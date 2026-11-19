// Reads a few game values the D2RLoader SDK doesn't offer: the hero's position and the town
// fire's position (for the traveler's portal). READ ONLY: nothing here writes to game memory or
// changes game code (CLAUDE.md). Reads use Windows' ReadProcessMemory on our own process, which
// reports a failed read instead of crashing. Everything read and where it comes from is listed
// in docs/game-memory.md. The feature is off unless D2R.exe is the frozen build in D2R_VERSION.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace GameMemory {

struct Pos {
	int  x  = 0;
	int  y  = 0;
	bool ok = false;
};

// True when D2R.exe is build 95032 (D2R_VERSION 3.4.95032; stored as 3.4.29496.0). Checked once.
auto BuildMatches() -> bool;

// The D2R.exe version found, for the log ("3.4.0.95032", or "unknown").
auto BuildFound() -> std::string;

// Looks for the hero (by name) and the town fire (objects.txt row 39) in the game's memory, on
// a background thread. Takes a few seconds; the results are kept until the next scan.
void StartScan(const std::string& heroName);

// True while a scan runs.
auto Scanning() -> bool;

// The hero's position now (read fresh from the unit the last scan found).
auto Hero() -> Pos;

// The hero's unit found by the last scan (0: none yet), for gamestate.h readers.
auto HeroUnit() -> uintptr_t;

// The town fire's position (found by the last scan; it never moves).
auto Fire() -> Pos;

// The town's stash chest nearest the fire (objects.txt row 267), found with the fire.
auto Stash() -> Pos;

// The hero's magic find (itemstatcost.txt stat 80) with gear counted, read through GameState's
// stat lists (gamestate.h). `where` says why when ok is false.
struct Stat {
	int         value = 0;
	bool        ok    = false;
	std::string where;
};
auto HeroMagicFind() -> Stat;

// A few lines about the last scan for the plugin log.
auto Report() -> std::string;

}  // namespace GameMemory
