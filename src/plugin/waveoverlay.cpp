// The wave counter at the top of the screen. See waveoverlay.h.

#include "waveoverlay.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace WaveOverlay {
namespace {

// The counter is a small panel of our own at the top centre (darkestdiablo/WaveCounter), opened
// while something is to be shown and closed otherwise. It has no click catcher, so the game
// below it should still take clicks. Its layout is made here (Layout) and registered as a
// resource; every text in it is a widget the plugin shows or hides. (The game's own HUD panel
// can't be used: WidgetService gives "Unavailable" for "HUDPanel", 0.15.4.)
constexpr char PanelLocalId[] = "WaveCounter";
constexpr char PanelName[]    = "darkestdiablo/WaveCounter";
constexpr char LayoutPath[]   = "data/global/ui/layouts/darkestdiablo/WaveCounterhd.json";

const D2RL::PanelService*          g_panels = nullptr;
D2RL::Panels::RegistrationHandle   g_panel  = D2RL::Panels::InvalidHandle;
std::string                        g_layout;      // must outlive the resource registration
bool                               g_open = false; // UI thread only

const D2RL::WidgetService* g_widgets = nullptr;
const D2RL::ThreadService* g_threads = nullptr;

enum class Mode : int { Hidden = 0, Waves = 1, Cleared = 2, Demo = 3 };

std::atomic<int>     g_mode { 0 };
std::atomic<int>     g_wave { 0 };
std::atomic<int>     g_total { 0 };
std::atomic<int64_t> g_nextWaveAt { -1 };  // steady-clock ms when the next wave spawns, -1 = none
std::atomic<int64_t> g_since { 0 };        // steady-clock ms when Cleared() or Demo() began
std::atomic<bool>    g_hideAll { true };    // hide every counter widget at the next update
std::atomic<int>     g_applied { 0 };      // the state last put on screen (Encode), set on the UI thread
std::atomic<bool>    g_queued { false };   // an update is waiting for the UI thread

constexpr int64_t ClearedShowMs  = 5000;
constexpr int     DemoWaves      = 5;
constexpr int64_t DemoIntervalMs = 10000;
constexpr int64_t RefreshMs      = 1000;   // re-show what should be up, in case the panel was reopened

auto NowMs() noexcept -> int64_t {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// What should be on screen. title: 0 none, -1 "Arena cleared", else wave * 100 + total.
// sub: 0 none, -1 "Final wave", 1..5 the countdown.
struct State {
	int title = 0;
	int sub   = 0;
};

auto Encode(State s) noexcept -> int {
	return (s.title + 1) * 100 + (s.sub + 1);
}

void WaveState(State& out, int wave, int total, int64_t msToNext) noexcept {
	if (total < 1 || total > MaxWaves) {
		return;  // no widgets for this many waves
	}
	const int shown = wave < 1 ? 1 : (wave > total ? total : wave);
	out.title = shown * 100 + total;
	if (msToNext >= 0 && msToNext <= CountdownSeconds * 1000) {
		const int seconds = static_cast<int>((msToNext + 999) / 1000);  // 4.2 s left shows 5
		out.sub = seconds < 1 ? 1 : seconds;
	} else if (msToNext < 0 && wave >= total) {
		out.sub = -1;
	}
}

auto Desired() noexcept -> State {
	State out;
	const int64_t now = NowMs();
	switch (static_cast<Mode>(g_mode.load())) {
	case Mode::Waves: {
		const int64_t at = g_nextWaveAt.load();
		WaveState(out, g_wave.load(), g_total.load(), at < 0 ? -1 : (at > now ? at - now : 0));
		break;
	}
	case Mode::Cleared:
		if (now - g_since.load() > ClearedShowMs) {
			g_mode = static_cast<int>(Mode::Hidden);
		} else {
			out.title = -1;
		}
		break;
	case Mode::Demo: {
		const int64_t t = now - g_since.load() + DemoIntervalMs - 1000;  // wave 1 a second after start
		const int wave = static_cast<int>(t / DemoIntervalMs);
		if (wave > DemoWaves) {  // past the last wave: show the clear for a few seconds
			if (t - (DemoWaves + 1) * DemoIntervalMs > ClearedShowMs) {
				g_mode = static_cast<int>(Mode::Hidden);
			} else {
				out.title = -1;
			}
		} else {
			WaveState(out, wave, DemoWaves, wave < DemoWaves ? (wave + 1) * DemoIntervalMs - t : -1);
		}
		break;
	}
	default:
		break;
	}
	return out;
}

auto TitleName(int title) -> std::string {
	return title == -1 ? "DDCleared" : "DDWave" + std::to_string(title / 100) + "of" + std::to_string(title % 100);
}

auto SubName(int sub) -> std::string {
	return sub == -1 ? "DDFinal" : "DDNext" + std::to_string(sub);
}

// UI thread. Returns false if the widget wasn't found or couldn't be changed.
auto SetVisible(const D2RL::PluginContext* context, D2RL::Widgets::WidgetHandle panel, const std::string& name, bool visible) noexcept -> bool {
	D2RL::Widgets::WidgetHandle handle = D2RL::Widgets::InvalidHandle;
	return g_widgets->findWidget(context, panel, name.c_str(), &handle) == D2RL::Widgets::Result::Success &&
		g_widgets->setWidgetVisible(context, handle, visible) == D2RL::Widgets::Result::Success;
}

State g_shown;               // UI thread only: what is on screen now
bool  g_loggedMissing = false;
bool  g_loggedFirstApply = false;
bool  g_loggedOpenFailure = false;

void __cdecl Apply(const D2RL::PluginContext* context, void*) noexcept {
	g_queued = false;
	const State want = Desired();
	if (want.title == 0) {  // nothing to show: close the panel
		if (g_open) {
			(void)g_panels->closePanel(context, g_panel);
			g_open = false;
		}
		g_shown = State {};
		g_applied = Encode(want);
		g_hideAll = false;  // a closed panel is hidden anyway; opening it hides its texts again
		return;
	}
	if (g_open) {  // the game closes every panel when it leaves a game or loads: check
		D2RL::Panels::PanelInfo info { .structSize = D2RL::Panels::PanelInfoSize };
		if (g_panels->getPanelInfo(context, g_panel, &info) == D2RL::Panels::Result::Success && info.presentationState != D2RL::Panels::PresentationState::Open) {
			g_open = false;
		}
	}
	if (!g_open) {
		const auto result = g_panels->openPanel(context, g_panel);
		if (result != D2RL::Panels::Result::Success) {
			if (!g_loggedOpenFailure) {
				g_loggedOpenFailure = true;
				char message[96] {};
				std::snprintf(message, sizeof(message), "Wave counter: opening the panel gave code %d.", static_cast<int>(result));
				context->LogWarn(message);
			}
			return;  // not in a game yet; try again next time
		}
		g_open = true;
		g_hideAll = true;  // a freshly opened panel shows every text: hide them before it draws
	}
	D2RL::Widgets::WidgetHandle panel = D2RL::Widgets::InvalidHandle;
	const auto found = g_widgets->findPanel(context, PanelName, &panel);
	if (found != D2RL::Widgets::Result::Success) {
		if (!g_loggedMissing) {
			g_loggedMissing = true;
			char message[96] {};
			std::snprintf(message, sizeof(message), "Wave counter: findPanel gave code %d.", static_cast<int>(found));
			context->LogWarn(message);
		}
		return;
	}
	try {
		if (g_hideAll.exchange(false)) {
			int hidden = 0;
			for (int n = 1; n <= MaxWaves; ++n) {
				for (int k = 1; k <= n; ++k) {
					hidden += SetVisible(context, panel, TitleName(k * 100 + n), false) ? 1 : 0;
				}
			}
			for (int s = -1; s <= CountdownSeconds; ++s) {
				if (s != 0) {
					SetVisible(context, panel, SubName(s), false);
				}
			}
			SetVisible(context, panel, "DDCleared", false);
			SetVisible(context, panel, "DDPlateShort", false);
			SetVisible(context, panel, "DDPlateTall", false);
			g_shown = State {};
			if (!g_loggedFirstApply) {
				g_loggedFirstApply = true;
				char message[128] {};
				std::snprintf(message, sizeof(message), "Wave counter: panel open, hid %d of %d wave widgets.", hidden, MaxWaves * (MaxWaves + 1) / 2);
				context->LogInfo(message);
			}
		}
		// Hide what changed, then show what is wanted (again, so a reopened panel gets it back).
		if (g_shown.title != 0 && g_shown.title != want.title) {
			SetVisible(context, panel, TitleName(g_shown.title), false);
		}
		if (g_shown.sub != 0 && g_shown.sub != want.sub) {
			SetVisible(context, panel, SubName(g_shown.sub), false);
		}
		const bool tall = want.title != 0 && want.sub != 0;
		const bool shortPlate = want.title != 0 && want.sub == 0;
		SetVisible(context, panel, "DDPlateTall", tall);
		SetVisible(context, panel, "DDPlateShort", shortPlate);
		if (want.title != 0) {
			SetVisible(context, panel, TitleName(want.title), true);
		}
		if (want.sub != 0) {
			SetVisible(context, panel, SubName(want.sub), true);
		}
		g_shown = want;
		g_applied = Encode(want);
	} catch (...) {
	}
}

// Its own thread: about 5 times a second, asks the UI thread to update when the wanted state
// changed, and once a second while something is up.
void Ticker(const D2RL::PluginContext* context) noexcept {
	int64_t lastRefresh = 0;
	for (;;) {
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		const State want = Desired();
		const int64_t now = NowMs();
		const bool changed = Encode(want) != g_applied.load() || g_hideAll.load();
		const bool refresh = want.title != 0 && now - lastRefresh >= RefreshMs;
		if ((changed || refresh) && !g_queued.exchange(true)) {
			lastRefresh = now;
			const auto result = g_threads->runOnUiThread(context, Apply, nullptr);
			if (result != D2RL::Threads::Result::Success) {
				g_queued = false;
				static bool logged = false;
				if (!logged) {
					logged = true;
					char message[96] {};
					std::snprintf(message, sizeof(message), "Wave counter: runOnUiThread gave code %d.", static_cast<int>(result));
					context->LogWarn(message);
				}
			}
		}
	}
}

auto Text(const std::string& name, int y, int height, const std::string& text, const char* color, const char* size) -> std::string {
	return "{\"type\":\"TextBoxWidget\",\"name\":\"" + name + "\",\"fields\":{\"rect\":{\"x\":-700,\"y\":" + std::to_string(y) +
		",\"width\":1400,\"height\":" + std::to_string(height) + "},\"text\":\"" + text + "\",\"style\":{\"fontColor\":\"" + color +
		"\",\"pointSize\":\"" + size + "\",\"alignment\":{\"h\":\"center\",\"v\":\"center\"},\"dropShadow\":\"$DefaultDropShadow\"}}}";
}

auto Plate(const char* name, int height) -> std::string {
	return std::string("{\"type\":\"NineTileImageWidget\",\"name\":\"") + name + "\",\"fields\":{\"filename\":\"PANEL/TooltipBorder/TooltipBorder\","
		"\"backgroundColor\":[0.0,0.0,0.0,0.7],\"tileScale\":1.0,\"rect\":{\"x\":-340,\"y\":-12,\"width\":680,\"height\":" + std::to_string(height) + "}}}";
}

// The panel: anchored at the top centre of the screen (layout units: the screen is 2160 high),
// 40 units down. Plates first, so the texts draw over them.
auto Layout() -> std::string {
	std::string kids = Plate("DDPlateShort", 124) + "," + Plate("DDPlateTall", 196);
	for (int n = 1; n <= MaxWaves; ++n) {
		for (int k = 1; k <= n; ++k) {
			kids += "," + Text(TitleName(k * 100 + n), 0, 100, "Wave " + std::to_string(k) + " / " + std::to_string(n), "$FontColorGoldYellow", "$ExtraLargeFontSize");
		}
	}
	for (int s = 1; s <= CountdownSeconds; ++s) {
		kids += "," + Text(SubName(s), 96, 76, "Next wave in " + std::to_string(s), s <= 3 ? "$FontColorRed" : "$FontColorWhite", "$LargeFontSize");
	}
	kids += "," + Text("DDFinal", 96, 76, "Final wave: clear the arena", "$FontColorWhite", "$LargeFontSize");
	kids += "," + Text("DDCleared", 0, 100, "Arena cleared", "$FontColorGoldYellow", "$ExtraLargeFontSize");
	return std::string("{\"type\":\"Panel\",\"name\":\"") + PanelName + "\",\"fields\":{\"anchor\":{\"x\":0.5,\"y\":0.0},"
		"\"rect\":{\"x\":0,\"y\":40},\"priority\":100},\"children\":[" + kids + "]}";
}

}  // namespace

auto Load(const D2RL::PluginContext* context) noexcept -> bool {
	if (context->QueryService(&g_widgets) != D2RL::ServiceQueryResult::Success || !D2RL::HasWidgetServiceField(g_widgets, D2RL::WidgetServiceRequiredSize) ||
		context->QueryService(&g_threads) != D2RL::ServiceQueryResult::Success || !D2RL::HasThreadServiceField(g_threads, D2RL::ThreadServiceRequiredSize)) {
		context->LogWarn("Wave counter: widget or thread service missing; no wave counter on screen.");
		return true;
	}
	const D2RL::ResourceService* resources = nullptr;
	if (context->QueryService(&resources) != D2RL::ServiceQueryResult::Success || !D2RL::HasResourceServiceField(resources, D2RL::ResourceServiceRequiredSize) ||
		context->QueryService(&g_panels) != D2RL::ServiceQueryResult::Success || !D2RL::HasPanelServiceField(g_panels, D2RL::PanelServiceRequiredSize)) {
		context->LogWarn("Wave counter: panel services missing; no wave counter on screen.");
		g_widgets = nullptr;
		return true;
	}
	try {
		g_layout = Layout();
	} catch (...) {
		g_widgets = nullptr;
		return true;
	}
	const D2RL::Resources::ResourceRegistration resource {
		.structSize = D2RL::Resources::ResourceRegistrationSize,
		.path       = LayoutPath,
		.bytes      = g_layout.data(),
		.byteCount  = static_cast<uint32_t>(g_layout.size()),
	};
	D2RL::Resources::RegistrationHandle resourceHandle = D2RL::Resources::InvalidHandle;
	const D2RL::Panels::PanelRegistration registration {
		.structSize = D2RL::Panels::PanelRegistrationSize,
		.flags      = D2RL::Panels::PanelFlags::None,
		.localId    = PanelLocalId,
	};
	if (resources->registerResource(context, &resource, &resourceHandle) != D2RL::Resources::Result::Success ||
		g_panels->registerPanel(context, &registration, &g_panel) != D2RL::Panels::Result::Success) {
		context->LogWarn("Wave counter: could not register its panel; no wave counter on screen.");
		g_widgets = nullptr;
		return true;
	}
	try {
		std::thread(Ticker, context).detach();
	} catch (...) {
		context->LogWarn("Wave counter: could not start its timer.");
		return true;
	}
	context->LogInfo("Wave counter: ready.");
	return true;
}

void OnPlayerReady() noexcept {
	g_hideAll = true;
}



void SetWaves(int wave, int total, int nextWaveInMs) noexcept {
	g_wave       = wave;
	g_total      = total;
	g_nextWaveAt = nextWaveInMs < 0 ? -1 : NowMs() + nextWaveInMs;
	g_mode       = static_cast<int>(Mode::Waves);
}

void Cleared() noexcept {
	g_since = NowMs();
	g_mode  = static_cast<int>(Mode::Cleared);
}

void Hide() noexcept {
	g_mode = static_cast<int>(Mode::Hidden);
}

void Demo() noexcept {
	g_since = NowMs();
	g_mode  = static_cast<int>(Mode::Demo);
}

}  // namespace WaveOverlay
