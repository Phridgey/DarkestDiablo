// Mercenary full gear. The game decides what a mercenary may wear in two copies of one check (the
// server's, which the equip message and the save load both reach, and the panel's): any body armor
// (item type 3, "tors"), any helm (37), and the mercenary's own weapon types. So a ring is refused
// ("I can't use that") even with the slot shown (056_merc_slots.py). Each copy's body armor test
// is "mov edx,3 / mov rcx,rdi / call ItemIsType"; the plugin changes its 3 to our item type
// "merc" (997_merc_gear_type.py), a parent type of body armor, rings, amulets, gloves, boots and
// belts, so the same test now passes for all six. Nothing else in the check changes.
//
// Found and done as PD2 Resurrected does it (MIT; synpoox/pd2-resurrected, merc_rules.cpp, #1627):
// the two byte patterns below, searched in the game's code in memory, and the edit through
// D2RLoader's MutationService (both or neither, the original bytes checked first).
//
// Agreed by both of us on 2026-11-23 as the one exception to "never change game code".
// Without the change the game's save load drops every ring, amulet, glove, belt and boot a
// mercenary wears (PD2's notes), and the next save makes that permanent. So when the change can't
// be made, the plugin says so in a message box and offers to close the game.

#include "mercgear.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace MercGear {
namespace {

// The bytes before "mov edx,3" pin each copy; ?? = any byte. The "mov" is at offset 10 in both.
constexpr char ServerPattern[] = "45 84 f6 4c 8b 74 24 50 74 31 ba 03 00 00 00 48 8b cf e8 ?? ?? ?? ?? 85 c0 75 0f 8d 50 25 48 8b cf e8";
constexpr char ClientPattern[] = "3b c3 74 04 32 db eb 02 b3 01 ba 03 00 00 00 48 8b cf e8 ?? ?? ?? ?? 85 c0 0f 85 ?? ?? ?? ?? 8d 50 25 48 8b cf e8";
constexpr size_t MovOffset = 10;
constexpr uint8_t ArmorTest[] = { 0xba, 0x03, 0x00, 0x00, 0x00 };  // mov edx,3

struct Pattern { std::vector<uint8_t> bytes; std::vector<bool> any; };

auto Parse(const char* text) -> Pattern {
	Pattern p;
	for (const char* s = text; *s != '\0';) {
		if (*s == ' ') {
			++s;
			continue;
		}
		if (s[0] == '?') {
			p.bytes.push_back(0);
			p.any.push_back(true);
		} else {
			p.bytes.push_back(static_cast<uint8_t>(std::strtoul(std::string(s, 2).c_str(), nullptr, 16)));
			p.any.push_back(false);
		}
		s += 2;
	}
	return p;
}

// The game's code section (.text) in memory, from its PE headers.
auto CodeSection(uintptr_t base, const uint8_t** start, size_t* size) -> bool {
	if (base == 0) {
		return false;
	}
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
		return false;
	}
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
	const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
	for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
		if (std::memcmp(section->Name, ".text", 5) == 0) {
			*start = reinterpret_cast<const uint8_t*>(base + section->VirtualAddress);
			*size = section->Misc.VirtualSize;
			return true;
		}
	}
	return false;
}

// Every match of the pattern; the caller wants exactly one.
auto Find(const uint8_t* code, size_t size, const Pattern& p) -> std::vector<size_t> {
	std::vector<size_t> found;
	const size_t n = p.bytes.size();
	for (size_t i = 0; i + n <= size; ++i) {
		size_t k = 0;
		while (k < n && (p.any[k] || code[i + k] == p.bytes[k])) {
			++k;
		}
		if (k == n) {
			found.push_back(i);
		}
	}
	return found;
}

auto Patch(const D2RL::PluginContext* context, int mercType, char* message, size_t size) noexcept -> bool {
	if (mercType <= 0 || mercType > 255) {
		std::snprintf(message, size, "no usable \"merc\" item type number (%d)", mercType);
		return false;
	}
	const uint8_t* code = nullptr;
	size_t codeSize = 0;
	if (!CodeSection(context->exeBase, &code, &codeSize)) {
		std::snprintf(message, size, "the game's code section was not found");
		return false;
	}
	uint64_t rva[2] {};
	const char* patterns[2] = { ServerPattern, ClientPattern };
	const char* names[2] = { "server", "panel" };
	for (int k = 0; k < 2; ++k) {
		const auto found = Find(code, codeSize, Parse(patterns[k]));
		if (found.size() != 1) {
			std::snprintf(message, size, "the %s check matched %d times (want 1)", names[k], static_cast<int>(found.size()));
			return false;
		}
		rva[k] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(code) + found[0] + MovOffset - context->exeBase);
	}
	const D2RL::MutationService* mutations = nullptr;
	D2RL::Mutations::TransactionHandle transaction = D2RL::Mutations::InvalidTransactionHandle;
	if (context->QueryService(&mutations) != D2RL::ServiceQueryResult::Success ||
		!D2RL::HasMutationServiceField(mutations, D2RL::MutationServiceRequiredSize) ||
		mutations->beginTransaction(context, &transaction) != D2RL::Mutations::Result::Success) {
		std::snprintf(message, size, "D2RLoader's mutation service is missing");
		return false;
	}
	const uint8_t mercTest[] = { 0xba, static_cast<uint8_t>(mercType), 0x00, 0x00, 0x00 };  // mov edx,<merc>
	bool staged = true;
	for (int k = 0; k < 2; ++k) {
		const D2RL::Mutations::BytePatchRequest patch {
			.structSize   = D2RL::Mutations::BytePatchRequestSize,
			.rva          = rva[k],
			.expected     = ArmorTest,
			.expectedSize = sizeof(ArmorTest),
			.bytes        = mercTest,
			.size         = sizeof(mercTest),
		};
		D2RL::Mutations::OperationHandle operation = D2RL::Mutations::InvalidOperationHandle;
		staged = staged && mutations->stageBytePatch(context, transaction, &patch, &operation) == D2RL::Mutations::Result::Success;
	}
	D2RL::Mutations::CommitResult result { .structSize = D2RL::Mutations::CommitResultSize };
	if (!staged || mutations->commit(context, transaction, &result) != D2RL::Mutations::Result::Success) {
		mutations->cancelTransaction(context, transaction);
		std::snprintf(message, size, "the change could not be made (staging or commit failed)");
		return false;
	}
	std::snprintf(message, size, "on (server check at 0x%llx, panel check at 0x%llx, item type %d)",
		static_cast<unsigned long long>(rva[0]), static_cast<unsigned long long>(rva[1]), mercType);
	return true;
}

}  // namespace

auto Install(const D2RL::PluginContext* context, int mercType) noexcept -> bool {
	char reason[200] {};
	bool on = false;
	try {
		on = Patch(context, mercType, reason, sizeof(reason));
	} catch (...) {
		std::snprintf(reason, sizeof(reason), "unexpected error");
	}
	char message[256] {};
	std::snprintf(message, sizeof(message), "Merc gear: %s.", reason);
	if (on) {
		context->LogInfo(message);
		return true;
	}
	context->LogWarn(message);
	const int answer = MessageBoxA(nullptr,
		"Darkest Diablo could not let mercenaries wear rings, amulets, gloves, belts and boots in this game.\n\n"
		"A mercenary already wearing any of those would lose them when its hero loads, for good once the game saves.\n\n"
		"Close the game now? (Yes is safest if any mercenary wears such items.)",
		"Darkest Diablo", MB_YESNO | MB_ICONWARNING | MB_TOPMOST);
	if (answer == IDYES) {
		TerminateProcess(GetCurrentProcess(), 1);
	}
	return false;
}

}  // namespace MercGear
