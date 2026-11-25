// Tests for the camp rules (src/plugin/camp.h). No game needed.
//   g++ -std=c++20 -I src/plugin tests/camp_tests.cpp -o build/camp_tests && build/camp_tests
// tools\build-plugin.cmd builds and runs them too.
#include "camp.h"
#include "route.h"
#include "stash.h"
#include "potions.h"
#include "routepages.h"
#include "journal.h"
#include "stashfile.h"

#include <cstdlib>
#include <iostream>

using namespace Camp;

static int failures = 0;
#define CHECK(condition) \
	do { \
		if (!(condition)) { \
			++failures; \
			std::cerr << __FILE__ << ":" << __LINE__ << ": failed: " #condition "\n"; \
		} \
	} while (false)

static const int Paladin = ClassFromName("Paladin");
static const int Sorceress = ClassFromName("Sorceress");
static const int Druid = ClassFromName("Druid");

// A fake save: the header the game writes, with class, level and the hardcore / dead flags.
static void WriteSave(const fs::path& file, int cls, int level, bool hardcore = true, bool dead = false) {
	std::string bytes(64, '\0');
	bytes[0] = 0x55; bytes[1] = static_cast<char>(0xAA); bytes[2] = 0x55; bytes[3] = static_cast<char>(0xAA);
	bytes[36] = static_cast<char>((hardcore ? 0x04 : 0) | (dead ? 0x08 : 0) | 0x20);
	bytes[40] = static_cast<char>(cls);
	bytes[43] = static_cast<char>(level);
	WriteFile(file, bytes);
}

static auto TempDir(const char* name) -> fs::path {
	const fs::path dir = fs::temp_directory_path() / "darkestdiablo-camp-tests" / name;
	fs::remove_all(dir);
	fs::create_directories(dir);
	return dir;
}

static void NewCampOffersEveryClassThenLocksTheRest() {
	std::mt19937 rng(1);
	State camp;
	CHECK(Creatable(camp, rng).size() == ClassCount);
	Adopt(camp, "Burr", Paladin, 1);
	CHECK(camp.started);
	CHECK(Creatable(camp, rng).empty());  // nobody else can be made until a traveler comes
	CHECK(camp.LockedClasses().size() == ClassCount - 1);
}

static void TravelerOffersThreeLockedClasses() {
	State camp;
	Adopt(camp, "Burr", Paladin, 5);
	for (unsigned seed = 0; seed < 200; ++seed) {
		std::mt19937 rng(seed);
		const auto offer = RollOffer(camp, rng);
		CHECK(offer.size() == 3);
		CHECK(!State::Has(offer, Paladin));
		CHECK(offer[0] != offer[1] && offer[1] != offer[2] && offer[0] != offer[2]);
	}
	std::mt19937 rng(7);
	camp.offer = RollOffer(camp, rng);
	const int pick = camp.offer[1];
	CHECK(!Recruit(camp, Paladin));  // not on offer
	CHECK(Recruit(camp, pick));
	CHECK(camp.offer.empty());
	CHECK(Creatable(camp, rng) == std::vector<int> { pick });
	CHECK(!State::Has(RollOffer(camp, rng), pick));  // a recruit is not offered again
	Adopt(camp, "Xade", pick, 1);
	CHECK(Creatable(camp, rng).empty());
	CHECK(camp.heroes.size() == 2);
}

static void OfferShrinksWhenFewClassesAreLocked() {
	std::mt19937 rng(3);
	State camp;
	for (int c = 0; c < ClassCount - 2; ++c) {
		Adopt(camp, "Hero" + std::to_string(c), c, 1);
	}
	CHECK(RollOffer(camp, rng).size() == 2);
	Adopt(camp, "Six", ClassCount - 2, 1);
	Adopt(camp, "Seven", ClassCount - 1, 1);
	CHECK(RollOffer(camp, rng).empty());
}

static void FallenClassIsAlwaysOffered() {
	State camp;
	Adopt(camp, "Burr", Paladin, 20);
	Adopt(camp, "Xade", Sorceress, 3);
	MarkFallen(camp, "burr");
	CHECK(camp.heroes.size() == 1 && camp.Locked(Paladin));
	for (unsigned seed = 0; seed < 200; ++seed) {
		std::mt19937 rng(seed);
		const auto offer = RollOffer(camp, rng);
		CHECK(offer.size() == 3 && offer[0] == Paladin);
	}
	std::mt19937 rng(5);
	camp.offer = RollOffer(camp, rng);
	CHECK(Recruit(camp, Paladin));
	CHECK(camp.fallen.empty());  // offered once; after that it is a locked class like any other
}

static void EmptyFireGetsATraveler() {
	std::mt19937 rng(9);
	State camp;
	Adopt(camp, "Burr", Paladin, 20);
	MarkFallen(camp, "Burr");
	const auto offer = Creatable(camp, rng);
	CHECK(offer.size() == 3 && offer[0] == Paladin);
	CHECK(Creatable(camp, rng) == offer);  // the same offer until answered
	Adopt(camp, "Edan", Druid == offer[1] ? offer[1] : offer[2], 1);
	CHECK(camp.offer.empty() && Creatable(camp, rng).empty());
}

static void CampFileRoundTrip() {
	State camp;
	Adopt(camp, "Burr", Paladin, 12);
	camp.recruits = { Sorceress };
	camp.fallen = { Druid };
	camp.offer = { Druid, 0, 2 };
	camp.difficulty = 2;
	const State back = Parse(Serialize(camp));
	CHECK(back.difficulty == 2 && Parse(Serialize(State {})).difficulty == 0 && Parse("difficulty 7\n").difficulty == 2);
	CHECK(back.started && back.heroes.size() == 1 && back.heroes[0].name == "Burr" && back.heroes[0].level == 12);
	CHECK(back.recruits == camp.recruits && back.fallen == camp.fallen && back.offer == camp.offer);
	CHECK(Summary(back) == "Paladin 12 - 1 recruit waiting");
	CHECK(Summary(State {}) == "Empty - start a new camp");
	CHECK(!Parse("garbage\nhero Nobody 3 x\n").started);
	const auto text = DelveText(back);
	CHECK(text[0].second == "12 Paladin" && text[1].second.empty() && text[8].second == "1 recruit waiting");
	CHECK(text[10].second == "Hell" && text[9].second == "$FontColorRed" && text[11].second == "Continue");
	const auto empty = DelveText(State {});
	CHECK(empty[0].second.empty() && empty[10].second.empty() && empty[11].second == "Start New");
}

static void SlotsKeepTheirOwnHeroesAndStash() {
	const fs::path live = TempDir("slots");
	WriteFile(live / "Settings.json", "{}");
	WriteFile(live / "Starter - Pal.fltr", "filter");
	WriteSave(live / "Burr.d2s", Paladin, 12);
	WriteFile(live / "Burr.ctlo", "x");
	WriteFile(live / "SharedStashHardCoreV2.d2i", "slot one stash");
	CHECK(ActiveSlot(live) == 1);
	std::string error;
	CHECK(SwitchSlot(live, 2, error));
	CHECK(ActiveSlot(live) == 2);
	CHECK(HeroNames(live).empty());
	CHECK(fs::exists(live / "Settings.json") && fs::exists(live / "Starter - Pal.fltr"));  // shared by all slots
	CHECK(!fs::exists(live / "SharedStashHardCoreV2.d2i"));
	WriteSave(live / "Xade.d2s", Sorceress, 1);
	WriteFile(live / "SharedStashHardCoreV2.d2i", "slot two stash");
	CHECK(SwitchSlot(live, 1, error));
	CHECK(HeroNames(live) == std::vector<std::string> { "Burr" });
	CHECK(ReadFile(live / "SharedStashHardCoreV2.d2i") == "slot one stash");
	CHECK(fs::exists(live / "Burr.ctlo"));
	CHECK(SwitchSlot(live, 2, error));
	CHECK(HeroNames(live) == std::vector<std::string> { "Xade" });
	CHECK(ReadFile(live / "SharedStashHardCoreV2.d2i") == "slot two stash");
	CHECK(!SwitchSlot(live, 4, error));
}

static void DeletingASaveEmptiesOnlyThatSave() {
	const fs::path live = TempDir("slots-delete");
	WriteFile(live / "Settings.json", "{}");
	WriteSave(live / "Burr.d2s", Paladin, 12);
	WriteFile(live / "SharedStashHardCoreV2.d2i", "slot one stash");
	State one;
	Adopt(one, "Burr", Paladin, 12);
	one.started = true;
	SaveState(live, 1, one);
	std::string error;
	CHECK(SwitchSlot(live, 2, error));
	WriteSave(live / "Xade.d2s", Sorceress, 1);
	State two;
	Adopt(two, "Xade", Sorceress, 1);
	two.started = true;
	SaveState(live, 2, two);
	// A parked save: slot 1 goes, the live slot 2 is untouched.
	CHECK(DeleteSlot(live, 1, "t1", error));
	CHECK(!LoadState(live, 1).started);
	CHECK(HeroNames(live) == std::vector<std::string> { "Xade" });
	CHECK(fs::exists(live / "camp" / "trash" / "slot1-t1" / "camp" / "files" / "Burr.d2s"));  // kept, not erased
	CHECK(SwitchSlot(live, 1, error));
	CHECK(HeroNames(live).empty() && !fs::exists(live / "SharedStashHardCoreV2.d2i"));
	// The live save: its files leave the live folder; settings stay.
	CHECK(SwitchSlot(live, 2, error));
	CHECK(DeleteSlot(live, 2, "t2", error));
	CHECK(HeroNames(live).empty() && fs::exists(live / "Settings.json"));
	CHECK(!LoadState(live, 2).started && ActiveSlot(live) == 2);
	CHECK(fs::exists(live / "camp" / "trash" / "slot2-t2" / "live" / "Xade.d2s"));
	CHECK(!DeleteSlot(live, 9, "t3", error));
}

static void PendingChangesWaitForTheNextStart() {
	const fs::path live = TempDir("slots-pending");
	WriteFile(live / "Settings.json", "{}");
	WriteSave(live / "Burr.d2s", Paladin, 12);
	WriteFile(live / "SharedStashHardCoreV2.d2i", "slot one stash");
	std::string error;
	CHECK(SwitchSlot(live, 2, error));
	WriteSave(live / "Xade.d2s", Sorceress, 1);
	WriteFile(live / "SharedStashHardCoreV2.d2i", "slot two stash");
	CHECK(ApplyPending(live).empty());  // nothing waiting: nothing happens
	// Noted while the game runs: nothing moves yet.
	Pending pending;
	pending.open = 1;
	CHECK(WritePending(live, pending));
	CHECK(ActiveSlot(live) == 2 && ReadFile(live / "SharedStashHardCoreV2.d2i") == "slot two stash");
	CHECK(ReadPending(live).open == 1 && ReadPending(live).deletes.empty());
	// The next start makes the change, once.
	CHECK(ApplyPending(live).size() == 1);
	CHECK(ActiveSlot(live) == 1 && ReadFile(live / "SharedStashHardCoreV2.d2i") == "slot one stash");
	CHECK(!fs::exists(PendingFile(live)));
	CHECK(ApplyPending(live).empty() && ActiveSlot(live) == 1);
	// Deleting the live save waits too, and a new save there starts without its stash.
	pending = {};
	pending.deletes.emplace_back(1, "t1");
	CHECK(WritePending(live, pending));
	CHECK(HeroNames(live) == std::vector<std::string> { "Burr" });
	ApplyPending(live);
	CHECK(HeroNames(live).empty() && !fs::exists(live / "SharedStashHardCoreV2.d2i") && ActiveSlot(live) == 1);
	CHECK(fs::exists(live / "camp" / "trash" / "slot1-t1" / "live" / "SharedStashHardCoreV2.d2i"));
	CHECK(fs::exists(live / "Settings.json"));
	// Bad lines are ignored.
	WriteFile(PendingFile(live), "open 7\ndelete 0 x\nnonsense\n");
	CHECK(ReadPending(live).Empty());
	ApplyPending(live);
	CHECK(ActiveSlot(live) == 1 && !fs::exists(PendingFile(live)));
}

static void SwitchingSlotNeverLosesFilesOnFailure() {
	const fs::path live = TempDir("slots-clash");
	WriteSave(live / "Burr.d2s", Paladin, 12);
	WriteFile(SlotDir(live, 1) / "files" / "Burr.d2s", "left over");  // should never be there while slot 1 is live
	std::string error;
	CHECK(!SwitchSlot(live, 2, error));
	CHECK(!error.empty());
	CHECK(ActiveSlot(live) == 1 && fs::exists(live / "Burr.d2s"));
}

static void ReconcileFollowsTheSaveFiles() {
	const fs::path live = TempDir("reconcile");
	State camp;
	// A hero was just made on the hero screen: the camp learns its class from the pick.
	WriteSave(live / "Burr.d2s", Paladin, 1);
	Reconcile(live, 1, camp, { .newClass = Paladin });
	CHECK(camp.started && camp.heroes.size() == 1 && camp.heroes[0].cls == Paladin);
	// It levels up.
	WriteSave(live / "Burr.d2s", Paladin, 9);
	Reconcile(live, 1, camp, {});
	CHECK(camp.heroes[0].level == 9);
	Reconcile(live, 1, camp, { .leveledName = "Burr", .leveledTo = 10 });
	CHECK(camp.heroes[0].level == 10);
	// A second Paladin can't join.
	WriteSave(live / "Twin.d2s", Paladin, 1);
	Reconcile(live, 1, camp, {});
	CHECK(camp.heroes.size() == 1);
	fs::remove(live / "Twin.d2s");
	// A recruit is made.
	camp.recruits = { Sorceress };
	WriteSave(live / "Xade.d2s", Sorceress, 1);
	WriteFile(live / "Xade.map", "map");
	Reconcile(live, 1, camp, { .newClass = Sorceress });
	CHECK(camp.heroes.size() == 2 && camp.recruits.empty());
	// The Sorceress dies (hardcore flag in the save): buried, locked, remembered as fallen.
	WriteSave(live / "Xade.d2s", Sorceress, 4, true, true);
	Reconcile(live, 1, camp, {});
	CHECK(camp.heroes.size() == 1 && camp.fallen == std::vector<int> { Sorceress });
	CHECK(!fs::exists(live / "Xade.d2s") && !fs::exists(live / "Xade.map"));
	bool buried = false;
	for (const auto& entry : fs::recursive_directory_iterator(live / "camp" / "graveyard" / "slot1")) {
		buried = buried || entry.path().filename() == "Xade.d2s";
	}
	CHECK(buried);
	CHECK(fs::exists(live / "Burr.d2s"));
	// The Paladin dies in a softcore save: the game's death report is enough.
	WriteSave(live / "Burr.d2s", Paladin, 12, false, false);
	Reconcile(live, 1, camp, { .diedName = "Burr" });
	CHECK(camp.heroes.empty() && camp.fallen == (std::vector<int> { Paladin, Sorceress }));
	CHECK(HeroNames(live).empty());
	// A deleted save counts as a death: the class is fallen and comes first in the next offer.
	WriteSave(live / "Edan.d2s", Druid, 2);
	Reconcile(live, 1, camp, { .newClass = Druid });
	fs::remove(live / "Edan.d2s");
	Reconcile(live, 1, camp, {});
	CHECK(camp.heroes.empty() && camp.fallen.front() == Druid);
	// The bench: every hero but the one to play waits outside the live folder, and is not
	// taken for deleted while it waits.
	Camp::State two;
	WriteSave(live / "Kara.d2s", 6, 3);
	Reconcile(live, 1, two, { .newClass = 6 });
	WriteSave(live / "Kord.d2s", 4, 4);
	two.recruits.push_back(4);
	Reconcile(live, 1, two, {});
	two.HeroNamed("Kara")->gear = "4:ktr 3:lea";
	std::string error;
	CHECK(Bench(live, 1, two, "Kord", error));
	CHECK(HeroNames(live) == (std::vector<std::string> { "Kord" }));
	Reconcile(live, 1, two, {});
	CHECK(two.heroes.size() == 2);
	CHECK(Unbench(live, 1) == 0);
	CHECK(HeroNames(live).size() == 2);
	CHECK(Parse(Serialize(two)).HeroNamed("Kara")->gear == "4:ktr 3:lea");
}

// ---- The route rules (src/plugin/route.h) --------------------------------------------------

static void RouteUnlocksTierByTier() {
	Route::Progress p;
	const auto& den = *Route::StopById("den");
	const auto& burial = *Route::StopById("burial");
	const auto& inifuss = *Route::StopById("inifuss");
	const auto& crypt = *Route::StopById("crypt");
	CHECK(p.CurrentTier() == 0);
	CHECK(p.CanEnter(den));
	CHECK(!p.CanEnter(burial));                   // can't skip ahead
	CHECK(!p.CanEnter(inifuss));
	CHECK(p.RecordClear(den));
	CHECK(!p.CanEnter(den));                      // a main-line stop is cleared once, then locks
	CHECK(!p.RecordClear(den));                   // ... and a stray clear doesn't count
	CHECK(p.Clears(den) == 1);
	CHECK(p.CanEnter(burial));
	CHECK(p.RecordClear(burial));
	CHECK(p.CurrentTier() == 2);                  // fork 1
	CHECK(p.CanEnter(inifuss));
	CHECK(p.CanEnter(crypt));                     // fork 1 has three built options
	CHECK(!p.CanEnter(*Route::StopById("andariel")));  // the last tier: not yet
}

static void LaterActsFollowInOrder() {
	Route::Progress p;
	for (const auto& stop : Route::Stops) {
		if (Route::ActOfTier(stop.tier) <= 2) {
			p.clears[stop.id] = 1;
		}
	}
	CHECK(p.CurrentTier() == 11);                  // Act III: Khalim's three parts, any order
	CHECK(Route::ActOfTier(11) == 3 && Route::ActOfTier(15) == 4 && Route::ActOfTier(24) == 5);
	CHECK(p.CanEnter(*Route::StopById("spider")) && p.CanEnter(*Route::StopById("ksewers")));
	CHECK(p.RecordClear(*Route::StopById("spider")));
	CHECK(!p.CanEnter(*Route::StopById("spider")));  // each part once
	CHECK(p.RecordClear(*Route::StopById("flayer")));
	CHECK(p.CurrentTier() == 11);
	CHECK(p.RecordClear(*Route::StopById("ksewers")));
	CHECK(p.CurrentTier() == 12 && p.CanEnter(*Route::StopById("lowerkurast")));
	CHECK(Route::StopByTravelLevel(39) == Route::StopById("cows"));
	CHECK(Route::StopByArenaLevel(132) == Route::StopById("baal"));
	CHECK(Route::TownOfAct(5) == 109);
	p.flags["hratli"] = 1;
	CHECK(p.Flag("hratli") == 1 && p.Flag("goldenbird") == 0);
}

static void ActTwoFollowsAndariel() {
	Route::Progress p;
	for (const char* id : { "den", "burial", "inifuss", "crypt", "tristram", "pit", "malus", "andariel" }) {
		p.clears[id] = 1;
	}
	const auto& halls = *Route::StopById("halls");
	CHECK(p.CurrentTier() == 6);                  // Act II opens after Andariel
	CHECK(p.CanEnter(halls));
	CHECK(!p.CanEnter(*Route::StopById("sewers")));
	CHECK(p.RecordClear(halls));
	CHECK(p.CanEnter(*Route::StopById("sewers")));
	CHECK(p.CanEnter(*Route::StopById("stony")));
	CHECK(Route::ActOfTier(halls.tier) == 2);
	// Tier 8: both forced stops, in either order; each once, then the next tier.
	p.clears["sewers"] = 1;
	p.clears["stony"] = 1;
	CHECK(p.CurrentTier() == 8);
	CHECK(Route::IsAllOf(8));
	p.clears["clawviper"] = 1;
	CHECK(p.CurrentTier() == 8);                  // the Maggot Lair is still to do
	p.clears["maggot"] = 1;
	CHECK(p.CurrentTier() == 9);
}

static void ForkGivesTwoPicksSameOneAllowed() {
	Route::Progress p;
	p.clears["den"] = 1;
	p.clears["burial"] = 1;
	const auto& inifuss = *Route::StopById("inifuss");
	CHECK(p.RecordClear(inifuss));
	CHECK(p.CanEnter(inifuss));                   // the same option a second time: its replay
	CHECK(p.RecordClear(inifuss));
	CHECK(p.Clears(inifuss) == 2);
	CHECK(!p.CanEnter(inifuss));                  // two picks: the fork locks
	CHECK(p.CurrentTier() == 3);                  // Tristram is next
	CHECK(!p.RecordClear(inifuss));
}

static void ARunMustBeFinishedAndResumes() {
	Route::Progress p;
	p.clears["den"] = 1;
	p.clears["burial"] = 1;
	const auto& mausoleum = *Route::StopById("mausoleum");
	const auto& crypt = *Route::StopById("crypt");
	CHECK(p.StartRun(mausoleum) == 0);            // a new run
	p.NoteWaves(mausoleum, 3);
	CHECK(!p.CanEnter(crypt));                    // the pick is spent: no switching
	CHECK(p.CanEnter(mausoleum));
	CHECK(p.StartRun(mausoleum) == 3);            // coming back resumes after wave 3
	const fs::path dir = TempDir("run");
	const fs::path file = dir / "hero.txt";
	CHECK(Route::Save(file, p));
	const auto loaded = Route::Load(file);
	CHECK(loaded.running == "mausoleum" && loaded.runWaves == 3);
	CHECK(p.RecordClear(mausoleum));              // clearing ends the run
	CHECK(p.running.empty() && p.CanEnter(crypt));
	CHECK(p.StartRun(mausoleum) == 0);            // the same option again: a fresh run
}

static void RouteProgressRoundTripAndBadLines() {
	const fs::path dir = TempDir("route");
	const fs::path file = Route::ProgressFile(dir, 2, "Kara");
	CHECK(file == dir / "camp" / "slot2" / "progress" / "Kara.txt");
	Route::Progress p;
	p.clears["den"] = 1;
	p.clears["inifuss"] = 2;
	CHECK(Route::Save(file, p));
	CHECK(Route::Load(file).clears == p.clears);
	WriteFile(file, "clear den 1\nclear nowhere 3\nclear burial -2\nnonsense\n");
	const auto loaded = Route::Load(file);
	CHECK(loaded.clears.size() == 1 && loaded.Clears(*Route::StopById("den")) == 1);
	CHECK(Route::Load(dir / "missing.txt").CurrentTier() == 0);   // no file: a fresh hero
}

static void EveryBuiltStopIsFoundByItsLevels() {
	for (const auto& stop : Route::Stops) {
		if (stop.travelLevel >= 0) {
			CHECK(Route::StopByTravelLevel(stop.travelLevel) == &stop);
			CHECK(Route::StopByArenaLevel(stop.arenaLevel) == &stop);
		}
	}
	CHECK(Route::StopByArenaLevel(1) == nullptr);   // town is not a stop
}

// The start of a real save of this game build (version 105): an Assassin of level 4.
static void ReadsThisGameBuildsSaves() {
	const fs::path dir = fs::temp_directory_path() / "camp_tests_v105";
	fs::remove_all(dir);
	fs::create_directories(dir);
	unsigned char head[48] = { 0x55, 0xAA, 0x55, 0xAA, 0x69, 0, 0, 0, 0x5D, 0x05, 0, 0, 0xB6, 0x82, 0x63, 0x94,
		0, 0, 0, 0, 0, 0, 0, 0, 0x06, 0x10, 0x1E, 0x04, 0, 0, 0, 0,
		0xD5, 0xD3, 0xFC, 0x6A, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0xFF, 0xFF, 0, 0 };
	std::ofstream(dir / "Stabbu.d2s", std::ios::binary).write(reinterpret_cast<const char*>(head), sizeof(head));
	const SaveInfo info = ReadSave(dir / "Stabbu.d2s");
	CHECK(info.ok && info.cls == 6 && info.level == 4 && !info.hardcore && !info.dead);
	fs::remove_all(dir);
}

static auto ActDone(int lastAct) -> Route::Progress {
	Route::Progress p;
	for (const auto& stop : Route::Stops) {
		if (Route::ActOfTier(stop.tier) <= lastAct) {
			p.clears[stop.id] = Route::IsFork(stop.tier) ? Route::PicksPerFork : 1;
		}
	}
	return p;
}

static void StashPagesArePaidInParts() {
	Stash::State s;
	CHECK(s.Pages() == 1 && s.Owing() == 50000);
	CHECK(!s.Pay(30000) && s.fund == 30000 && s.Owing() == 20000);
	CHECK(s.Pay(999999) && s.tier == 1 && s.fund == 0);  // never more than is owed, nothing carried over
	long long last = 0;
	for (const long long price : Stash::TierPrice) {
		CHECK(price > last);
		last = price;
	}
	while (s.tier < Stash::TierCount) {
		CHECK(s.Pay(s.Owing()));
	}
	CHECK(s.Pages() == Stash::PageCount && s.Owing() == 0 && !s.Pay(1));
	CHECK(Stash::Commas(50000) == "50,000" && Stash::Commas(2500000) == "2,500,000" && Stash::Commas(7) == "7" && Stash::Commas(100) == "100");
}

static void StashPointsComeOncePerClassAndAct() {
	for (int act = 1; act <= 5; ++act) {  // LastTierOfAct agrees with the route
		const int tier = Stash::LastTierOfAct[act - 1];
		CHECK(Route::ActOfTier(tier) == act && (tier + 1 == Route::TierCount || Route::ActOfTier(tier + 1) == act + 1));
	}
	Stash::State s;
	CHECK(Stash::CountActs(s, Paladin, Route::Progress {}) == 0);
	CHECK(Stash::CountActs(s, Paladin, ActDone(1)) == 1);
	CHECK(Stash::CountActs(s, Paladin, ActDone(1)) == 0);   // the same act again, even by a later Paladin
	CHECK(Stash::CountActs(s, Paladin, ActDone(3)) == 2);
	CHECK(Stash::CountActs(s, Druid, ActDone(1)) == 1);
	CHECK(Stash::CountActs(s, -1, ActDone(5)) == 0);
	CHECK(s.Earned() == 4 && s.Points() == 4);
}

static void StashTreeNeedsParentsAndPoints() {
	std::set<std::string> ids;
	int total = 0;
	for (const auto& node : Stash::Nodes) {
		CHECK(ids.insert(node.id).second);
		CHECK(node.parent[0] == '\0' || ids.count(node.parent) == 1);  // listed after its parent
		total += node.cost;
		for (const auto& other : Stash::Nodes) {  // none lies over another
			CHECK(&other == &node || other.row != node.row || other.column >= node.column + 1 || node.column >= other.column + 1);
		}
	}
	CHECK(total > 5 && total <= ClassCount * 5);  // one class is never enough, all classes always are
	Stash::State s;
	CHECK(s.BuyNode("tab") == Stash::Buy::NotEnoughPoints);
	Stash::CountActs(s, Paladin, ActDone(5));
	CHECK(s.BuyNode("gem1") == Stash::Buy::Locked);
	CHECK(s.BuyNode("tab") == Stash::Buy::Bought && s.BuyNode("tab") == Stash::Buy::AlreadyOwned);
	CHECK(s.BuyNode("gem1") == Stash::Buy::Bought && s.BuyNode("gem2") == Stash::Buy::Bought && s.BuyNode("gem3") == Stash::Buy::Bought);
	CHECK(s.Points() == 0 && s.BuyNode("rune1") == Stash::Buy::NotEnoughPoints && s.BuyNode("nonsense") == Stash::Buy::Unknown);
}

static void StashSlotsTakeWhatIsUnlocked() {
	std::set<std::string> codes;
	for (const auto& kind : Stash::Kinds()) {
		CHECK(codes.insert(kind.code).second);
		for (const char* need : kind.needs) {
			CHECK(Stash::FindNode(need) != nullptr);
		}
	}
	CHECK(codes.size() == 35 + 33);
	Stash::State s;
	const auto& chipped = *Stash::KindOf("gcw");
	const auto& skull = *Stash::KindOf("skc");
	CHECK(s.Room(chipped) == 0);
	s.nodes = { "tab", "gem1" };
	CHECK(s.Room(chipped) == Stash::SmallCap && s.Room(skull) == 0);
	s.nodes.insert("gemskull");
	CHECK(s.Room(skull) == Stash::SmallCap);
	s.Add("gcw", 20);
	CHECK(s.Room(chipped) == 0);
	s.nodes.insert("gemcap");
	CHECK(s.Room(chipped) == 79);
	CHECK(s.CutGems(0) == 0);  // no cutter yet
	s.nodes.insert("gemcut");
	CHECK(s.CutGems(0) == 6 && s.Count("gcw") == 2 && s.Count("gfw") == 6);
	CHECK(s.CutGems(1) == 0 && s.Count("gfw") == 6);  // the normal gems' slot is not unlocked: nothing is lost
	s.Add("gcw", -2);
	CHECK(s.mats.count("gcw") == 0);
}

static void StashFileRoundTripAndBadLines() {
	Stash::State s;
	s.tier = 2; s.fund = 1234; s.legacyDone = true; s.legacy = { 4 };
	s.acts = { { Druid, 1 }, { Paladin, 5 } }; s.nodes = { "tab", "rune1" }; s.mats = { { "r01", 7 } };
	const auto back = Stash::Parse(Stash::Serialize(s));
	CHECK(back.tier == 2 && back.fund == 1234 && back.legacyDone && back.legacy == s.legacy && back.acts == s.acts && back.nodes == s.nodes && back.mats == s.mats);
	CHECK(back.PageOpen(2) && !back.PageOpen(3) && back.PageOpen(4) && back.LastPage() == 5);
	const auto bad = Stash::Parse("tier 99\nfund -5\nact Nobody 1\nact Druid 9\nnode what\nmat zzz 4\nmat r01 -3\nlegacy 40\nnonsense\n");
	CHECK(bad.tier == Stash::TierCount && bad.fund == 0 && bad.acts.empty() && bad.nodes.empty() && bad.mats.empty() && bad.legacy.empty());
}

// "camp_tests layouts <folder>" writes the caravan's panels there, for tests/test_stash_layouts.py.
static void WriteStashLayouts(const fs::path& dir) {
	Stash::State fresh, full;
	full.tier = 3; full.fund = 120000;
	for (int c = 0; c < ClassCount; ++c) {
		Stash::CountActs(full, c, ActDone(5));
	}
	for (const auto& node : Stash::Nodes) {
		full.nodes.insert(node.id);
	}
	int n = 0;
	for (const auto& kind : Stash::Kinds()) {
		full.mats[kind.code] = n++ % 5 == 0 ? 0 : n;
	}
	WriteFile(dir / "tree_fresh.json", Stash::TreeLayout(fresh));
	WriteFile(dir / "tree_full.json", Stash::TreeLayout(full));
	full.nodes = { "tab", "gem1", "rune1", "potregen" };
	WriteFile(dir / "tree_part.json", Stash::TreeLayout(full));
	full.nodes.clear();
	for (const auto& node : Stash::Nodes) {
		full.nodes.insert(node.id);
	}
	WriteFile(dir / "tab_fresh.json", Stash::UpgradesLayout(fresh));
	WriteFile(dir / "tab_full.json", Stash::UpgradesLayout(full));
	WriteFile(dir / "wall_part.json", Stash::WallLayout(full, "Hero not found yet, try again"));
	WriteFile(dir / "wall_fresh.json", Stash::WallLayout(fresh));
	WriteFile(dir / "page.json", Stash::PageLayout(10, 10, 1234567));
	WriteFile(dir / "side.json", Stash::SideLayout());
	Potions::State potions;
	Potions::Owner owner;
	WriteFile(dir / "hud_new.json", Potions::HudLayout(potions, owner, -1));
	owner.nodes = full.nodes;
	owner.level = 40;
	for (const int kind : { Potions::Antidote, Potions::Thawing, Potions::Stamina }) {
		potions.Give(kind, owner);
	}
	potions.Fill(owner);
	for (int slot = 0; slot < Potions::SlotCount; ++slot) {
		WriteFile(dir / ("hud_pick" + std::to_string(slot) + ".json"), Potions::HudLayout(potions, owner, slot));
	}
	potions.Concoct(2, owner);
	WriteFile(dir / "hud_full.json", Potions::HudLayout(potions, owner, 3));
	potions.affixes = { { "antidote", "crypt" }, { "stamina", "pit" } };
	WriteFile(dir / "route1.json", Potions::WithAffixes(RouteMapLayout, 1, potions, { { "Fork1Crypt", "Crypt" } }));
	for (int rune = 0; rune < Journal::RuneCount; ++rune) {
		for (int page = 0; page < Journal::PageCount(rune); ++page) {
			WriteFile(dir / ("journal_" + std::to_string(rune) + "_" + std::to_string(page) + ".json"), Journal::Layout(rune, page));
		}
	}
}

static bool Near(double a, double b) { return a - b < 1e-6 && b - a < 1e-6; }

static void PotionChargesFillAndAreSpent() {
	using namespace Potions;
	Potions::State s;
	Owner o;
	CHECK(s.slots[0] == Heal && s.slots[1] == Mana && s.slots[2] == None && !s.Use(Heal, o));
	s.Fill(o);
	CHECK(Near(s.Charge(Heal), 50) && s.Uses(Mana) == 5 && s.Charge(Antidote) == 0);
	CHECK(s.Use(Heal, o) && Near(s.Charge(Heal), 40) && !s.Use(Antidote, o) && !s.Use(None, o));
	s.Tick(100, o);  // 0.1% of 50 a second
	CHECK(Near(s.Charge(Heal), 45) && Near(s.Charge(Mana), 50));
	s.Give(Antidote, o);
	CHECK(s.slots[2] == Antidote && Near(s.Charge(Antidote), 20));
	s.Use(Antidote, o);
	s.charge[Heal] = 0;
	s.Kill(Rank::Normal, 1, o);
	CHECK(Near(s.Charge(Heal), 0.5) && Near(s.Charge(Antidote), 10.375));
	s.Kill(Rank::Unique, 1, o);
	CHECK(Near(s.Charge(Heal), 3));
	o.level = 24;
	s.Kill(Rank::Champion, 1, o);  // far below the hero: 5%
	CHECK(Near(s.Charge(Heal), 3.075));
	CHECK(Near(LevelFactor(10, 15), 1) && Near(LevelFactor(10, 16), 0.81) && Near(LevelFactor(50, 100), 0.5) && Near(LevelFactor(50, 41), 0.24) && Near(LevelFactor(50, -1), 1));
	o.belt = 12;
	CHECK(Near(s.Capacity(Heal, o), 62) && Near(s.Capacity(Stamina, o), 32));
	o.nodes = { "potregen", "potregen2", "potheal" };
	CHECK(Near(s.Capacity(Heal, o), 74.5) && Near(s.Regen(o), 0.003));
	o.belt = 0;
	s.charge[Heal] = 74;  // a smaller belt: what no longer fits is lost at the next gain
	s.Tick(0, o);
	CHECK(Near(s.Charge(Heal), 62.5));
	CHECK(BeltCharges("lbl") == 4 && BeltCharges("tbl") == 8 && BeltCharges("uhc") == 12 && BeltCharges("rin") == 0);
}

static void PotionLoadoutSwapsAndGrades() {
	using namespace Potions;
	Potions::State s;
	Owner o;
	CHECK(s.Equip(3, Heal, o) && s.slots[3] == Heal && s.slots[0] == None);
	CHECK(s.Equip(1, Heal, o) && s.slots[1] == Heal && s.slots[3] == Mana);
	CHECK(s.Equip(1, Heal, o) && s.slots[1] == Heal);
	CHECK(!s.Equip(0, Thawing, o) && !s.Equip(4, Heal, o) && !s.Equip(0, Fire, o) && s.Equip(1, None, o) && s.slots[1] == None);
	CHECK(Name(Heal, s, o) == "Minor Healing Potion" && Name(Mana, s, o) == "Light Mana Potion");
	o.level = 54;
	CHECK(Name(Heal, s, o) == "Super Healing Potion" && Name(Mana, s, o) == "Super Mana Potion");
	o.nodes = { "potfire", "potgas" };
	CHECK(s.Equip(0, Fire, o) && std::string(PotFor(Fire, o).code) == "opl");
	o.nodes.insert({ "potstrong", "potmaster" });
	CHECK(std::string(PotFor(Fire, o).code) == "ops" && std::string(PotFor(Gas, o).code) == "gps");
	o.level = 13;
	CHECK(std::string(PotFor(Fire, o).code) == "opm" && std::string(PotFor(Gas, o).code) == "gpl");
	o.nodes = { "potanti", "potstam", "potthaw" };
	CHECK(s.Echoes(Heal, o).empty());  // only potions the hero has
	s.Give(Antidote, o); s.Give(Thawing, o); s.Give(Stamina, o);
	CHECK(s.Echoes(Heal, o) == std::vector<int>({ Antidote, Stamina }) && s.Echoes(Mana, o) == std::vector<int>({ Thawing }));
	CHECK(s.Echoes(Rejuv, o).size() == 3 && s.Echoes(Antidote, o).empty());
}

static void PotionsConcoctIntoOne() {
	using namespace Potions;
	Potions::State s;
	Owner o;
	o.belt = 10;
	s.Fill(o);
	s.charge[Mana] = 0;
	s.Concoct(1, o);
	CHECK(!s.Owns(Heal, o) && !s.Owns(Mana, o) && s.slots[0] == Rejuv && s.slots[1] == None);
	CHECK(Near(s.Capacity(Rejuv, o), 45) && Near(s.Charge(Rejuv), 22.5));  // 75% of 60, half full
	s.Concoct(1, o);
	CHECK(Near(s.Charge(Rejuv), 22.5));
	s.Concoct(2, o);
	CHECK(Near(s.Capacity(Rejuv, o), 30) && Near(s.Charge(Rejuv), 15) && Name(Rejuv, s, o) == "Full Rejuvenation Potion");
	o.nodes = { "potheal", "potmana" };
	CHECK(Near(s.Capacity(Rejuv, o), 42.5));  // both passives count
	Potions::State straight;
	straight.Concoct(2, o);  // the Act V concoction without the Act IV one
	CHECK(straight.blend == 2 && straight.Owns(Rejuv, o));
}

static void PotionAffixesLandOnOpenStopsOnce() {
	using namespace Potions;
	Owner o;
	int full = 0;
	for (unsigned seed = 0; seed < 2000; ++seed) {
		std::mt19937 rng(seed);
		Potions::State s;
		s.Roll(Route::Progress {}, rng, o);
		CHECK(s.affixes.count("antidote") && s.affixes.count("stamina") && s.affixes.count("thawing") && s.affixes.count("concoct"));
		CHECK(s.affixes["antidote"] != s.affixes["stamina"]);
		for (const auto& [id, stop] : s.affixes) {
			const auto* found = Route::StopById(stop);
			CHECK(found != nullptr && Route::ActOfTier(found->tier) == AffixById(id)->act);
			CHECK(stop != "andariel" && stop != "durance" && stop != "chaos" && stop != "baal");
		}
		full += s.affixes.count("full") ? 1 : 0;
		const auto before = s.affixes;
		s.Roll(Route::Progress {}, rng, o);
		CHECK(s.affixes == before);
	}
	CHECK(full > 60 && full < 140);  // 5%
	std::mt19937 rng(4);
	Potions::State old;
	old.Roll(ActDone(2), rng, o);  // past Act I: its potions are given
	CHECK(old.Owns(Antidote, o) && old.Owns(Stamina, o) && !old.Owns(Thawing, o) && old.affixes.count("thawing"));
	const std::string stop = old.affixes["concoct"];
	CHECK(old.Cleared("den", o) == nullptr);
	CHECK(std::string(old.Cleared(stop, o)->id) == "concoct" && old.blend == 1 && old.Cleared(stop, o) == nullptr);
}

static void PotionFileRoundTripAndBadLines() {
	using namespace Potions;
	Owner o;
	Potions::State s;
	s.rolled = true;
	s.Give(Antidote, o);
	s.charge[Heal] = 12.5;
	s.Equip(3, Antidote, o);
	s.affixes = { { "thawing", "spider" } };
	const auto back = Potions::Parse(Potions::Serialize(s));
	CHECK(back.rolled && back.own == s.own && back.slots == s.slots && Near(back.Charge(Heal), 12.5) && back.affixes == s.affixes && back.blend == 0);
	s.Concoct(1, o);
	const auto mixed = Potions::Parse(Potions::Serialize(s));
	CHECK(mixed.blend == 1 && mixed.own == s.own && mixed.slots == s.slots);
	const auto bad = Potions::Parse("blend 9\nown heal\nown what\ncharge heal nan\ncharge mana -4\nslot 1 heal\nslot 9 mana\nslot 2 rejuv\nslot 3 rejuv\naffix x den\naffix antidote nowhere\n");
	CHECK(bad.blend == 2 && !bad.Owns(Heal, o) && bad.Owns(Rejuv, o) && bad.slots[0] == None && bad.slots[1] == Rejuv && bad.slots[2] == None);
	CHECK(bad.affixes.empty() && bad.Charge(Heal) == 0 && bad.Charge(Mana) == 0);
}

static auto StashPage(int items, uint32_t gold, const std::string& body) -> std::string {
	std::string page = StashFile::BlankPage() + body;
	page[StashFile::HeaderSize + 2] = static_cast<char>(items);
	StashFile::SetNumber(page, 12, gold);
	StashFile::SetNumber(page, 16, static_cast<uint32_t>(page.size()));
	return page;
}

static void StashesJoinIntoOneFile() {
	const fs::path live = TempDir("stash-join");
	const auto save = [&](const std::string& a, uint32_t gold) {
		std::vector<std::string> pages(10, StashFile::BlankPage());
		pages[0] = StashPage(1, gold, a);
		pages[3] = StashPage(2, 0, a + " fourth \x55\xAA\x55\xAA inside");  // a page mark inside an item must not matter
		return StashFile::JoinPages(pages);
	};
	const std::string one = save("one", 500), three = save("three", 7);
	CHECK(WriteFile(live / "camp" / "active.txt", "3\n"));
	CHECK(WriteFile(live / StashName, three));
	CHECK(WriteFile(SlotDir(live, 1) / "files" / StashName, one));  // save 2 has none
	std::string log;
	CHECK(StashFile::Join(live, "t", log));
	CHECK(StashJoined(live) && StashFile::Join(live, "t2", log) && !fs::exists(live / "camp" / "stash-backup-t2"));
	const auto pages = StashFile::Pages(ReadFile(live / StashName));
	CHECK(pages && pages->size() == 30);
	CHECK(StashFile::ItemCount((*pages)[0]) == 1 && StashFile::ItemCount((*pages)[3]) == 2 && StashFile::ItemCount((*pages)[20]) == 1);
	CHECK((*pages)[0].find("one") != std::string::npos && (*pages)[23].find("three fourth") != std::string::npos);
	CHECK(StashFile::ItemCount((*pages)[10]) == 0 && StashFile::Gold((*pages)[0]) == 0 && StashFile::Gold((*pages)[20]) == 0);
	CHECK(StashFile::JoinedGold(live, 1) == 500 && StashFile::JoinedGold(live, 2) == 0 && StashFile::JoinedGold(live, 3) == 7);
	CHECK(ReadFile(live / "camp" / "stash-backup-t" / "slot1.d2i") == one && ReadFile(live / "camp" / "stash-backup-t" / "slot3.d2i") == three);
	// Changing saves leaves the stash where it is.
	const std::string file = ReadFile(live / StashName);
	std::string error;
	CHECK(SwitchSlot(live, 1, error) && ReadFile(live / StashName) == file);
	CHECK(SwitchSlot(live, 2, error) && ReadFile(live / StashName) == file);
	// Deleting: a copy, then blank pages, and only its own.
	CHECK(StashFile::CopyPages(live, 1, live / "copy.d2i") && StashFile::Pages(ReadFile(live / "copy.d2i"))->size() == 10);
	CHECK(StashFile::SetWipes(live, { 1, 3 }) && StashFile::Wipes(live) == std::set<int>({ 1, 3 }));
	CHECK(StashFile::SetWipes(live, {}) && StashFile::Wipes(live).empty());
	CHECK(StashFile::BlankPages(live, 1));
	StashFile::ForgetJoinedGold(live, 1);
	const auto after = StashFile::Pages(ReadFile(live / StashName));
	CHECK(after && after->size() == 30 && StashFile::ItemCount((*after)[0]) == 0 && (*after)[20] == (*pages)[20]);
	CHECK(StashJoined(live) && StashFile::JoinedGold(live, 1) == 0 && StashFile::JoinedGold(live, 3) == 7);
}

static void AStashThatCantBeReadIsLeftAlone() {
	const fs::path live = TempDir("stash-join-bad");
	const std::string good = StashFile::JoinPages(std::vector<std::string>(10, StashFile::BlankPage()));
	for (const std::string& bad : { std::string("nonsense"), good + "x", good.substr(0, good.size() - 1) }) {
		CHECK(WriteFile(live / StashName, bad));
		CHECK(WriteFile(SlotDir(live, 2) / "files" / StashName, good));
		std::string log;
		CHECK(!StashFile::Join(live, "t", log) && !log.empty() && !StashJoined(live));
		CHECK(ReadFile(live / StashName) == bad && ReadFile(SlotDir(live, 2) / "files" / StashName) == good);
	}
	// Stopped after the file was replaced: the next start finishes and does not join twice.
	const std::string whole = StashFile::JoinPages(std::vector<std::string>(30, StashPage(1, 0, "x")));
	CHECK(WriteFile(live / StashName, whole) && WriteFile(JoinedFile(live), "joining\ngold 1 9\n"));
	std::string log;
	CHECK(StashFile::Join(live, "t", log) && StashJoined(live) && ReadFile(live / StashName) == whole && StashFile::JoinedGold(live, 1) == 9);
	CHECK(SwitchSlot(live, 2, log) && ReadFile(live / StashName) == whole);  // a parked file left behind is not in the way
}

static void JournalListsARunesWordsByLevel() {
	CHECK(Journal::RuneIndex("r01") == 0 && Journal::RuneIndex("r33") == 32);
	CHECK(Journal::RuneIndex("r34") == -1 && Journal::RuneIndex("rvs") == -1 && Journal::RuneIndex("") == -1);
	for (int rune = 0; rune < Journal::RuneCount; ++rune) {
		const auto words = Journal::WordsWith(rune);
		CHECK(!words.empty());
		for (size_t i = 1; i < words.size(); ++i) {
			CHECK(words[i - 1]->level <= words[i]->level);
		}
	}
}

int main(int argc, char** argv) {
	if (argc == 3 && std::string(argv[1]) == "join") {  // joins the stashes of a COPY of a save folder
		std::string log;
		const bool ok = StashFile::Join(argv[2], "dry-run", log);
		std::cout << log << "\n";
		return ok ? 0 : 1;
	}
	if (argc == 3 && std::string(argv[1]) == "layouts") {
		WriteStashLayouts(argv[2]);
		return 0;
	}
	StashPagesArePaidInParts();
	StashPointsComeOncePerClassAndAct();
	StashTreeNeedsParentsAndPoints();
	StashSlotsTakeWhatIsUnlocked();
	StashFileRoundTripAndBadLines();
	StashesJoinIntoOneFile();
	AStashThatCantBeReadIsLeftAlone();
	JournalListsARunesWordsByLevel();
	PotionChargesFillAndAreSpent();
	PotionLoadoutSwapsAndGrades();
	PotionsConcoctIntoOne();
	PotionAffixesLandOnOpenStopsOnce();
	PotionFileRoundTripAndBadLines();
	RouteUnlocksTierByTier();
	ForkGivesTwoPicksSameOneAllowed();
	ActTwoFollowsAndariel();
	LaterActsFollowInOrder();
	ARunMustBeFinishedAndResumes();
	RouteProgressRoundTripAndBadLines();
	EveryBuiltStopIsFoundByItsLevels();
	ReadsThisGameBuildsSaves();
	NewCampOffersEveryClassThenLocksTheRest();
	TravelerOffersThreeLockedClasses();
	OfferShrinksWhenFewClassesAreLocked();
	FallenClassIsAlwaysOffered();
	EmptyFireGetsATraveler();
	CampFileRoundTrip();
	SlotsKeepTheirOwnHeroesAndStash();
	DeletingASaveEmptiesOnlyThatSave();
	SwitchingSlotNeverLosesFilesOnFailure();
	PendingChangesWaitForTheNextStart();
	ReconcileFollowsTheSaveFiles();
	if (failures != 0) {
		std::cerr << failures << " check(s) failed\n";
		return 1;
	}
	std::cout << "camp tests passed\n";
	return 0;
}
