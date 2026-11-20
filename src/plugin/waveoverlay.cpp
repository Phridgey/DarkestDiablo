// The wave counter at the top of the screen. See waveoverlay.h.

#include "waveoverlay.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace WaveOverlay {
namespace {

// hudpanelhd.json's root is named "HUDPanel" (type HUDPanelHD); both names are tried, and the
// one that works is remembered.
constexpr const char* HudPanelNames[] = { "HUDPanel", "HUDPanelHD", "hudpanelhd" };
std::atomic<int>      g_hudName { -1 };

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
constexpr int64_t RefreshMs      = 1000;   // re-show what should be up, in case the HUD was remade

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

// UI thread. Returns false if the HUD isn't there (not in a game).
auto SetVisible(const D2RL::PluginContext* context, D2RL::Widgets::WidgetHandle panel, const std::string& name, bool visible) noexcept -> bool {
	D2RL::Widgets::WidgetHandle handle = D2RL::Widgets::InvalidHandle;
	return g_widgets->findWidget(context, panel, name.c_str(), &handle) == D2RL::Widgets::Result::Success &&
		g_widgets->setWidgetVisible(context, handle, visible) == D2RL::Widgets::Result::Success;
}

State g_shown;               // UI thread only: what is on screen now
bool  g_loggedMissing = false;
bool  g_loggedFirstApply = false;
uint32_t g_loggedFindCodes = 0;  // bit per findPanel result already logged

// UI thread. Finds the HUD panel under any of its names; logs each new failure code once.
auto FindHud(const D2RL::PluginContext* context, D2RL::Widgets::WidgetHandle* panel) noexcept -> bool {
	const int known = g_hudName.load();
	if (known >= 0 && g_widgets->findPanel(context, HudPanelNames[known], panel) == D2RL::Widgets::Result::Success) {
		return true;
	}
	for (int i = 0; i < static_cast<int>(sizeof(HudPanelNames) / sizeof(HudPanelNames[0])); ++i) {
		const auto result = g_widgets->findPanel(context, HudPanelNames[i], panel);
		if (result == D2RL::Widgets::Result::Success) {
			if (g_hudName.exchange(i) != i) {
				char message[96] {};
				std::snprintf(message, sizeof(message), "Wave counter: found the HUD panel as \"%s\".", HudPanelNames[i]);
				context->LogInfo(message);
			}
			return true;
		}
		const uint32_t bit = 1U << (static_cast<uint32_t>(result) & 31U);
		if ((g_loggedFindCodes & bit) == 0) {
			g_loggedFindCodes |= bit;
			char message[96] {};
			std::snprintf(message, sizeof(message), "Wave counter: findPanel(\"%s\") gave code %d.", HudPanelNames[i], static_cast<int>(result));
			context->LogInfo(message);
		}
	}
	return false;
}

void __cdecl Apply(const D2RL::PluginContext* context, void*) noexcept {
	g_queued = false;
	if (!g_loggedFirstApply) {
		g_loggedFirstApply = true;
		context->LogInfo("Wave counter: first update on the UI thread.");
	}
	D2RL::Widgets::WidgetHandle panel = D2RL::Widgets::InvalidHandle;
	if (!FindHud(context, &panel)) {
		return;  // not in a game; try again next time
	}
	try {
		if (g_hideAll.exchange(false)) {
			int found = 0;
			for (int n = 1; n <= MaxWaves; ++n) {
				for (int k = 1; k <= n; ++k) {
					found += SetVisible(context, panel, TitleName(k * 100 + n), false) ? 1 : 0;
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
			char message[128] {};
			std::snprintf(message, sizeof(message), "Wave counter: hid %d of %d wave widgets in the HUD.", found, MaxWaves * (MaxWaves + 1) / 2);
			if (found == 0) {
				context->LogWarn(message);
			} else if (!g_loggedMissing) {
				g_loggedMissing = true;  // once per run is enough when it works
				context->LogInfo(message);
			}
		}
		const State want = Desired();
		// Hide what changed, then show what is wanted (again, so a remade HUD gets it back).
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

}  // namespace

auto Load(const D2RL::PluginContext* context) noexcept -> bool {
	if (context->QueryService(&g_widgets) != D2RL::ServiceQueryResult::Success || !D2RL::HasWidgetServiceField(g_widgets, D2RL::WidgetServiceRequiredSize) ||
		context->QueryService(&g_threads) != D2RL::ServiceQueryResult::Success || !D2RL::HasThreadServiceField(g_threads, D2RL::ThreadServiceRequiredSize)) {
		context->LogWarn("Wave counter: widget or thread service missing; no wave counter on screen.");
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

void OnHudOpened(const D2RL::PluginContext* context) noexcept {
	g_hideAll = true;
	if (g_widgets != nullptr && g_threads != nullptr) {
		Apply(context, nullptr);  // the UI-message listener runs on the UI thread
	}
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
