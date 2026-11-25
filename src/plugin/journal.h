// Darkest Diablo — the stash's rune journal: a small window beside the stash that lists the
// runewords a rune is part of, in order of level (no game code here; see tests/camp_tests.cpp).
// The runes and runewords come from the game's own tables (tools/gen_journal.py).
#pragma once

#include "stash.h"
#include "journaldata.h"

#include <iterator>

namespace Journal {

inline constexpr int RuneCount = static_cast<int>(std::size(Runes));
inline constexpr int W = 660, H = 1180, PageSize = 11;
inline constexpr char TinyFont[] = "18";

// "r22" -> 21; -1 for anything that is not a rune.
inline auto RuneIndex(const std::string& code) -> int {
	if (code.size() != 3 || code[0] != 'r' || code[1] < '0' || code[1] > '9' || code[2] < '0' || code[2] > '9') {
		return -1;
	}
	const int n = (code[1] - '0') * 10 + (code[2] - '0');
	return n >= 1 && n <= RuneCount ? n - 1 : -1;
}

inline auto WordsWith(int rune) -> std::vector<const WordData*> {
	std::vector<const WordData*> out;
	for (const auto& word : Words) {
		if (std::find(word.runes, word.runes + word.count, rune) != word.runes + word.count) {
			out.push_back(&word);
		}
	}
	return out;
}

inline auto PageCount(int rune) -> int {
	return (std::max)(1, (static_cast<int>(WordsWith(rune).size()) + PageSize - 1) / PageSize);
}

inline auto LevelText(int level) -> std::string {
	return level > 0 ? "Required level " + std::to_string(level) : "No required level";
}

// A rune's picture; a click looks it up.
inline auto Picture(const std::string& name, int rune, int x, int y, double scale) -> std::string {
	char rect[96] {};
	std::snprintf(rect, sizeof(rect), "{\"x\":%d,\"y\":%d,\"scale\":%.2f}", x, y, scale);
	return "{\"type\":\"ButtonWidget\",\"name\":\"" + name + "\",\"fields\":{\"rect\":" + rect + ",\"filename\":\"items\\\\misc\\\\rune\\\\" +
		Camp::Lower(Runes[rune].name) + "_rune\",\"normalFrame\":0,\"pressedFrame\":0,\"hoveredFrame\":0,\"disabledFrame\":0,\"tooltipString\":\"" +
		Runes[rune].name + " Rune\",\"onClickMessage\":\"" + Stash::ActionMessage + "jrune-" + std::to_string(rune) + "\"}}";
}

// A runeword's name; its tooltip tells what the runeword does. The game writes a tooltip's lines
// from the bottom up unless told otherwise, and shows color codes as plain letters.
inline auto NameButton(const std::string& name, int x, int y, const WordData& word) -> std::string {
	char rect[96] {};
	std::snprintf(rect, sizeof(rect), "{\"x\":%d,\"y\":%d,\"scale\":%.2f}", x, y, 0.66);
	return "{\"type\":\"ButtonWidget\",\"name\":\"" + name + "\",\"fields\":{\"rect\":" + rect +
		",\"filename\":\"FrontEnd\\\\HD\\\\Final\\\\FrontEnd_ButtonSmall\",\"pressedFrame\":0,\"disabledFrame\":2,\"hoveredFrame\":3,\"textString\":\"" + word.name +
		"\",\"pointSize\":\"$SmallFontSize\",\"textColor\":\"" + Stash::Ui::TextGold + "\",\"tooltipString\":\"" + word.tip +
		"\",\"tooltipStyle\":{\"fontStyle\":{\"fontColor\":\"$FontColorLightBlue\",\"pointSize\":26,\"options\":{\"newlineHandling\":\"standard\"}},\"backgroundColor\":[0,0,0,0.93]},\"onClickMessage\":\"" + Stash::ActionMessage + "jnone\"}}";
}

// `rune` -1: none chosen yet.
inline auto Layout(int rune, int page) -> std::string {
	using namespace Stash::Ui;
	std::string out = Catcher(W, H) + "," + Box("Edge", { 0, 0, W, H }, Bronze) + "," + Box("Sheet", { 4, 4, W - 8, H - 8 }, "[0.035,0.03,0.028,1.0]");
	const auto add = [&](const std::string& widget) { out += "," + widget; };
	add(Text("Title", { 16, 8, 300, 46 }, "Rune Journal", TextGold, "$MediumLargeFontSize", "left"));
	add("{\"type\":\"ButtonWidget\",\"name\":\"Minimize\",\"fields\":{\"rect\":{\"x\":504,\"y\":12,\"scale\":0.38},\"filename\":\"FrontEnd\\\\HD\\\\Final\\\\FrontEnd_ButtonSmall\","
		"\"pressedFrame\":1,\"disabledFrame\":2,\"hoveredFrame\":3,\"textString\":\"Minimize\",\"pointSize\":\"$SmallFontSize\",\"textColor\":\"$FontColorWhite\",\"onClickMessage\":\"" +
		std::string(Stash::ActionMessage) + "journal\"}}");
	const Stash::Ui::Rect frame { 1400, -651 + Stash::SideY + 2 * Stash::SidePitch, W, H };
	if (rune < 0 || rune >= RuneCount) {
		add(Text("Hint1", { 16, 90, W - 32, 34 }, "Click a rune in the Upgrades tab,", TextBeige, "$SmallFontSize"));
		add(Text("Hint2", { 16, 124, W - 32, 34 }, "on a shared page or in the bag.", TextBeige, "$SmallFontSize"));
		return Panel(frame, "$LeftPanelAnchor", 8550, out);
	}
	page = std::clamp(page, 0, PageCount(rune) - 1);
	const auto words = WordsWith(rune);
	const RuneData& data = Runes[rune];
	add(Box("ChosenEdge", { 16, 62, 60, 60 }, Gold));
	add(Box("ChosenBox", { 18, 64, 56, 56 }, "[0.08,0.075,0.07,1.0]"));
	add(Picture("Chosen", rune, 18, 64, 0.57));
	add(Text("ChosenName", { 88, 60, 556, 34 }, std::string(data.name) + " Rune", TextGold, "$MediumLargeFontSize", "left"));
	add(Text("ChosenLevel", { 88, 94, 556, 28 }, LevelText(data.level) + ", in " + std::to_string(words.size()) + " runewords", TextBeige, TinyFont, "left"));
	int top = 130;
	const auto effects = [&](const char* label, const std::string& text) {  // one effect per line, split at ';'
		size_t from = 0;
		for (int i = 0; from <= text.size(); ++i) {
			const size_t end = (std::min)(text.find(';', from), text.size());
			const std::string n = std::to_string(top);
			if (i == 0) {
				add(Text("EffectOf" + n, { 16, top, 120, 24 }, label, TextBeige, TinyFont, "left"));
			}
			add(Text("Effect" + n, { 140, top, 510, 24 }, text.substr(from, end - from), "$FontColorLightBlue", TinyFont, "left"));
			top += 25;
			from = end + 1;
		}
	};
	effects("Weapon", data.weapon);
	effects("Armor", data.armor);
	effects("Shield", data.shield);
	add(Box("Rule", { 16, top + 6, W - 32, 3 }, Bronze));
	top += 18;
	for (int i = 0; i < PageSize && page * PageSize + i < static_cast<int>(words.size()); ++i) {
		const WordData& word = *words[static_cast<size_t>(page * PageSize + i)];
		const std::string n = std::to_string(i);
		const int y = top + i * 76;
		add(Text("Level" + n, { 12, y + 6, 46, 46 }, std::to_string(word.level), TextWhite, "$MediumLargeFontSize"));
		add(NameButton("Word" + n, 60, y, word));
		for (int k = 0; k < word.count; ++k) {
			const std::string id = n + "_" + std::to_string(k);
			const int x = 312 + k * 42;
			const bool chosen = word.runes[k] == rune;
			add(Box("WordEdge" + id, { x, y, 40, 40 }, chosen ? Gold : "[0.2,0.18,0.15,1.0]"));
			add(Box("WordBox" + id, { x + 2, y + 2, 36, 36 }, chosen ? "[0.25,0.19,0.07,1.0]" : "[0.08,0.075,0.07,1.0]"));
			add(Picture("WordRune" + id, word.runes[k], x + 2, y + 2, 0.37));
		}
		add(Text("Types" + n, { 312, y + 42, 340, 26 }, word.types, TextBeige, TinyFont, "left"));
	}
	if (const int pages = PageCount(rune); pages > 1) {
		const int y = H - 46;
		add(Button("Previous", 110, y, 0.38, "Back", page > 0 ? TextWhite : TextGrey, "jpage-" + std::to_string((std::max)(0, page - 1)), {}, "$SmallFontSize"));
		add(Text("Pages", { 260, y, 140, 36 }, std::to_string(page + 1) + " of " + std::to_string(pages), TextBeige, "$SmallFontSize"));
		add(Button("Next", 410, y, 0.38, "Next", page + 1 < pages ? TextWhite : TextGrey, "jpage-" + std::to_string((std::min)(pages - 1, page + 1)), {}, "$SmallFontSize"));
	}
	return Panel(frame, "$LeftPanelAnchor", 8550, out);
}

}
