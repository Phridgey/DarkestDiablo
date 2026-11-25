// Tests for the camp rules (src/plugin/camp.h). No game needed.
//   g++ -std=c++20 -I src/plugin tests/camp_tests.cpp -o build/camp_tests && build/camp_tests
// tools\build-plugin.cmd builds and runs them too.
#include "ascend.h"
#include "legacy.h"
#include "camp.h"
#include "route.h"
#include "stash.h"
#include "journal.h"

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
	CHECK(codes.size() == 35 + 33 + 12);
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
	WriteFile(dir / "tree_fresh.json", Stash::TreeLayout(fresh, "Hero not found yet, try again"));
	WriteFile(dir / "tree_full.json", Stash::TreeLayout(full));
	full.nodes = { "tab", "gem1", "rune1", "pot1" };
	WriteFile(dir / "tree_part.json", Stash::TreeLayout(full));
	full.nodes.clear();
	for (const auto& node : Stash::Nodes) {
		full.nodes.insert(node.id);
	}
	WriteFile(dir / "tab_fresh.json", Stash::UpgradesLayout(fresh));
	WriteFile(dir / "tab_full.json", Stash::UpgradesLayout(full));
	WriteFile(dir / "side.json", Stash::SideLayout());
	for (int rune = 0; rune < Journal::RuneCount; ++rune) {
		for (int page = 0; page < Journal::PageCount(rune); ++page) {
			WriteFile(dir / ("journal_" + std::to_string(rune) + "_" + std::to_string(page) + ".json"), Journal::Layout(rune, page));
		}
	}
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

// Ascend (ascend.h): a made-up save around real item bytes (a sorceress's starting kit).
static auto AscendSave(const std::string& hex) -> Ascend::Bytes {
	Ascend::Bytes b(400, 0);
	b[0] = 0x55, b[1] = 0xAA, b[2] = 0x55, b[3] = 0xAA, b[4] = 105;
	for (size_t i = 0; i + 1 < hex.size(); i += 2) {
		b.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
	}
	Ascend::Checksum(b);
	return b;
}

static void AscendMovesItems() {
	using namespace Ascend;
	Shapes shapes;
	std::istringstream lines("isave hp1 m 0 0 1 1\nisave box m 0 0 2 2\nssave 83 3 3\n");
	shapes.Read(lines);
	const std::string potion = "1000a200150000cf4f232869b703c888aa738b166ce19ee18b01";
	const std::string cube = "1000800005c044ff3c80dfa64052842a0d9915d785447abdd5699a333afc89fe03";
	const std::string tail = "4a4d00006a666b6600";
	Bytes donor = AscendSave("4a4d0200" + potion + cube + tail), heir = AscendSave("4a4d0000" + tail);
	Ascend::Hero hero;
	std::string error;
	CHECK(ReadHero(donor, shapes, hero, error) && hero.items.size() == 2 && hero.items[0].loc == 2 && hero.items[1].code == "box");
	std::vector<Item> legacy = Ascend::Legacy(hero);
	CHECK(legacy.size() == 1 && legacy[0].code == "hp1");  // the Cube itself stays behind
	std::vector<int> pages;
	CHECK(Pack(legacy, 10, pages) == 1 && pages[0] == 0);
	CHECK(Inject(heir, legacy, shapes, error));
	Ascend::Hero after;
	CHECK(ReadHero(heir, shapes, after, error) && after.items.size() == 1 && after.items[0].loc == 0 && after.items[0].page == 5);
	// Only its place bits differ from the original.
	Bytes was = hero.items[0].bytes, is = after.items[0].bytes;
	for (size_t i = 0; i < was.size(); ++i) {
		if (i < 4 || i > 6) { CHECK(was[i] == is[i]); }
	}
	// A stash page in use, a cut-off file and an unknown item are refused, with the save untouched.
	const Bytes before = heir;
	CHECK(!Inject(heir, legacy, shapes, error) && heir == before);
	Bytes cut(donor.begin(), donor.end() - 5);
	CHECK(!ReadHero(cut, shapes, hero, error));
	shapes.items.erase("hp1");
	CHECK(!ReadHero(donor, shapes, hero, error));
	// More than a page holds goes to a second page, nothing overlaps.
	std::vector<Item> many(130, legacy[0]);
	CHECK(Pack(many, 10, pages) == 2);
}

static void AscendedItemsTravel() {
	namespace L = Legacy;
	const fs::path live = fs::temp_directory_path() / "dd_ascend_test";
	fs::remove_all(live);
	fs::create_directories(Camp::SlotDir(live, 1));
	Ascend::Shapes shapes;
	std::istringstream lines("isave hp1 m 0 0 1 1\nssave 83 3 3\n");
	shapes.Read(lines);
	const std::string potion = "1000a200150000cf4f232869b703c888aa738b166ce19ee18b01", tail = "4a4d00006a666b6600";
	L::WriteBytes(live / "Anna.d2s", AscendSave("4a4d0100" + potion + tail));
	L::WriteBytes(live / "Bert.d2s", AscendSave("4a4d0000" + tail));
	std::string who, error;
	CHECK(L::ReadFight(live, 1, who) == L::Fight::None);
	L::WriteFight(live, 1, L::Fight::Fighting, "Anna");
	CHECK(L::ReadFight(live, 1, who) == L::Fight::Fighting && who == "Anna");
	L::WriteFight(live, 1, L::Fight::Won, "Anna");
	CHECK(L::ReadFight(live, 1, who) == L::Fight::Won);
	CHECK(L::Keep(live, 1, "Anna", "7", error));
	L::WriteFight(live, 1, L::Fight::None, "");
	who.clear();
	CHECK(L::ReadFight(live, 1, who) == L::Fight::None);
	CHECK(!L::Send(live, 1, "Bert", { "Bert" }, shapes).empty());
	CHECK(L::Load(live, 1).CarriedBy("bert") != nullptr);
	Ascend::Hero bert;
	CHECK(Ascend::ReadHero(L::ReadBytes(live / "Bert.d2s"), shapes, bert, error) && bert.items.size() == 1);
	CHECK(L::Send(live, 1, "Bert", { "Bert" }, shapes).empty());  // never twice
	// Bert dies on the way: the items go with the next hero.
	L::WriteBytes(live / "Cleo.d2s", AscendSave("4a4d0000" + tail));
	CHECK(!L::Send(live, 1, "Cleo", { "Cleo" }, shapes).empty());
	L::State state = L::Load(live, 1);
	L::Delivered(state, "Cleo");
	CHECK(state.tabs.size() == 1 && state.tabs.begin()->second == "Anna" && state.CarriedBy("Cleo") == nullptr);
	L::Save(live, 1, state);
	CHECK(L::Send(live, 1, "Cleo", { "Cleo" }, shapes).empty());  // nothing left to send
	CHECK(L::Load(live, 1).estates[0].done);
	// Arrows: 2 open pages, a tab on page 11.
	CHECK(L::Presses(1, 1, 2, 11, state) == 1 && L::Presses(2, 1, 2, 11, state) == 9 && L::Presses(11, 1, 2, 11, state) == 0);
	CHECK(L::Presses(11, -1, 2, 11, state) == 9 && L::Presses(1, -1, 2, 11, state) == 0);
	// Take only: what was taken out can't go back.
	std::vector<int> known { 1, 2 };
	CHECK(L::Strangers(known, std::vector<int> { 1, 2, 3 }) == std::vector<int> { 3 });
	CHECK(L::Strangers(known, std::vector<int> { 1 }).empty());
	CHECK(L::Strangers(known, std::vector<int> { 1, 2 }) == std::vector<int> { 2 });
	fs::remove_all(live);
}

int main(int argc, char** argv) {
	if (argc == 3 && std::string(argv[1]) == "layouts") {
		WriteStashLayouts(argv[2]);
		return 0;
	}
	StashPagesArePaidInParts();
	StashPointsComeOncePerClassAndAct();
	StashTreeNeedsParentsAndPoints();
	StashSlotsTakeWhatIsUnlocked();
	StashFileRoundTripAndBadLines();
	JournalListsARunesWordsByLevel();
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
	AscendMovesItems();
	AscendedItemsTravel();
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
