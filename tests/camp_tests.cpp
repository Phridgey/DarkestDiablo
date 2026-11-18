// Tests for the camp rules (src/plugin/camp.h). No game needed.
//   g++ -std=c++20 -I src/plugin tests/camp_tests.cpp -o build/camp_tests && build/camp_tests
// tools\build-plugin.cmd builds and runs them too.
#include "camp.h"
#include "route.h"

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
	CHECK(!p.CanEnter(crypt));                    // not built yet: never enterable
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

int main() {
	RouteUnlocksTierByTier();
	ForkGivesTwoPicksSameOneAllowed();
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
	ReconcileFollowsTheSaveFiles();
	if (failures != 0) {
		std::cerr << failures << " check(s) failed\n";
		return 1;
	}
	std::cout << "camp tests passed\n";
	return 0;
}
