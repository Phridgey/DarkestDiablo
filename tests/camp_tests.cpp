// Tests for the camp rules (src/plugin/camp.h). No game needed.
//   g++ -std=c++20 -I src/plugin tests/camp_tests.cpp -o build/camp_tests && build/camp_tests
// tools\build-plugin.cmd builds and runs them too.
#include "camp.h"

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
	const State back = Parse(Serialize(camp));
	CHECK(back.started && back.heroes.size() == 1 && back.heroes[0].name == "Burr" && back.heroes[0].level == 12);
	CHECK(back.recruits == camp.recruits && back.fallen == camp.fallen && back.offer == camp.offer);
	CHECK(Summary(back) == "Paladin 12 - 1 recruit waiting");
	CHECK(Summary(State {}) == "Empty - start a new camp");
	CHECK(!Parse("garbage\nhero Nobody 3 x\n").started);
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
	// A save deleted by hand leaves the camp without counting as fallen.
	WriteSave(live / "Edan.d2s", Druid, 2);
	Reconcile(live, 1, camp, { .newClass = Druid });
	fs::remove(live / "Edan.d2s");
	Reconcile(live, 1, camp, {});
	CHECK(camp.heroes.empty() && !State::Has(camp.fallen, Druid));
}

int main() {
	NewCampOffersEveryClassThenLocksTheRest();
	TravelerOffersThreeLockedClasses();
	OfferShrinksWhenFewClassesAreLocked();
	FallenClassIsAlwaysOffered();
	EmptyFireGetsATraveler();
	CampFileRoundTrip();
	SlotsKeepTheirOwnHeroesAndStash();
	SwitchingSlotNeverLosesFilesOnFailure();
	ReconcileFollowsTheSaveFiles();
	if (failures != 0) {
		std::cerr << failures << " check(s) failed\n";
		return 1;
	}
	std::cout << "camp tests passed\n";
	return 0;
}
