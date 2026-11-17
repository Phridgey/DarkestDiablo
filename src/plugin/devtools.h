// Darkest Diablo — the GM window: cheats for testing the mod. See docs/gm-tools.md.
//
// Open it with "gm" (or "/gm") in the console (Ctrl + `), or with its key (Ctrl + G by default,
// changeable in the game's Controls menu). It has: unlimited life, mana and stamina switches,
// a trip to town and back, item spawning, and one button per registered event.
//
// Adding an event (a traveler turning up, a future ambush, ...) takes one call before
// DevTools::Load: DevTools::RegisterEvent. The window grows a button for it and
// "gm event <id>" runs it from the console. Nothing else has to change.
#pragma once

#include <D2RLPlugin/api.h>

namespace DevTools {

// Runs on the UI thread, in a game.
using EventCallback = void(__cdecl*)(const D2RL::PluginContext* context) noexcept;

struct Event {
	const char*   id;       // short, lowercase, no spaces: "traveler"
	const char*   label;    // the button's text: "A traveler joins the camp"
	const char*   console;  // a console command that triggers it ("dd-traveler"), or nullptr
	EventCallback run;      // or a function to call; used when console is nullptr
};

// Adds an event to the window. Call before Load; the strings must stay valid (use literals).
// Returns false when the list is full or the event has no id, label or way to run.
auto RegisterEvent(const Event& event) noexcept -> bool;

// Call once from D2RLoaderLoadPlugin, after every RegisterEvent. Returns false only if the
// console command could not be added; missing optional services just switch the window off.
auto Load(const D2RL::PluginContext* context) noexcept -> bool;

// Give every UI message to this. Returns true when the game must not see the message.
auto OnUiMessage(const D2RL::PluginContext* context, const char* target, const char* command, const char* text) noexcept -> bool;

// Give GameJoined, LocalPlayerReady, LevelChanged and GameLeft to this.
void OnGameplayEvent(const D2RL::PluginContext* context, const D2RL::Lifecycle::GameplayEvent* event) noexcept;

}
