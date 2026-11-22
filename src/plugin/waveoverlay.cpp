// The wave counter. See waveoverlay.h.

#include "waveoverlay.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace WaveOverlay {
namespace {

const D2RL::PanelService*    g_panels    = nullptr;
const D2RL::ResourceService* g_resources = nullptr;
const D2RL::ThreadService*   g_threads   = nullptr;

// Where the text sits, in layout units (the screen is 2160 high): centred, below the enemy
// health bar at the top of the screen.
constexpr int TitleY = 190;
constexpr int SubY   = 290;

// One piece of text = one panel. Index into g_pieces.
struct Piece {
	std::string                      localId;  // e.g. "WaveK3"; the panel is darkestdiablo/<localId>
	std::string                      layout;   // its JSON; must outlive the resource registration
	D2RL::Panels::RegistrationHandle panel = D2RL::Panels::InvalidHandle;  // Invalid: not registered
	bool                             open  = false;  // UI thread only
};
std::vector<Piece> g_pieces;
int g_waveK[MaxWaves + 1] {};          // piece index of "Wave k", by k
int g_waveN[MaxWaves + 1] {};          // piece index of "/ n", by n
int g_next[CountdownSeconds + 1] {};   // piece index of "Next wave in s", by s
int g_clear = -1;                      // "Arena cleared"

enum class Mode : int { Hidden = 0, Waves = 1, Cleared = 2, Demo = 3 };

std::atomic<int>     g_mode { 0 };
std::atomic<int>     g_wave { 0 };
std::atomic<int>     g_total { 0 };
std::atomic<int64_t> g_nextWaveAt { -1 };  // steady-clock ms when the next wave spawns, -1 = none
std::atomic<int64_t> g_since { 0 };        // steady-clock ms when Cleared() or Demo() began
std::atomic<bool>    g_recheck { false };   // look at which panels the game really has open
std::atomic<int>     g_applied { -1 };     // the state last put on screen (Encode)
std::atomic<bool>    g_queued { false };   // an update is waiting for the UI thread

constexpr int64_t ClearedShowMs  = 5000;
constexpr int     DemoWaves      = 5;
constexpr int64_t DemoIntervalMs = 10000;
constexpr int64_t RecheckMs      = 1000;   // while something is up, check once a second that it still is

auto NowMs() noexcept -> int64_t {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// What should be on screen: wave (0 none) of total, the countdown second (0 none), cleared.
struct State {
	int  wave    = 0;
	int  total   = 0;
	int  seconds = 0;
	bool cleared = false;
};

auto Encode(const State& s) noexcept -> int {
	return ((s.wave * 16 + s.total) * 8 + s.seconds) * 2 + (s.cleared ? 1 : 0);
}

void WaveState(State& out, int wave, int total, int64_t msToNext) noexcept {
	out.wave  = wave < 1 ? 1 : (wave > total ? total : wave);
	out.total = total;
	if (msToNext >= 0 && msToNext <= CountdownSeconds * 1000) {
		const int seconds = static_cast<int>((msToNext + 999) / 1000);  // 4.2 s left shows 5
		out.seconds = seconds < 1 ? 1 : seconds;
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
			out.cleared = true;
		}
		break;
	case Mode::Demo: {
		const int64_t t = now - g_since.load() + DemoIntervalMs - 1000;  // wave 1 a second after start
		const int wave = static_cast<int>(t / DemoIntervalMs);
		if (wave > DemoWaves) {  // past the last wave: show the clear for a few seconds
			if (t - (DemoWaves + 1) * DemoIntervalMs > ClearedShowMs) {
				g_mode = static_cast<int>(Mode::Hidden);
			} else {
				out.cleared = true;
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

// The pieces that make up a state.
auto Wanted(const State& s) -> std::vector<bool> {
	std::vector<bool> want(g_pieces.size(), false);
	if (s.cleared) {
		want[static_cast<size_t>(g_clear)] = true;
		return want;
	}
	if (s.wave >= 1 && s.wave <= MaxWaves) {
		want[static_cast<size_t>(g_waveK[s.wave])] = true;
		if (s.total >= 1 && s.total <= MaxWaves) {
			want[static_cast<size_t>(g_waveN[s.total])] = true;
		}
	}
	if (s.seconds >= 1 && s.seconds <= CountdownSeconds) {
		want[static_cast<size_t>(g_next[s.seconds])] = true;
	}
	return want;
}

bool g_loggedFailure = false;  // UI thread only

void __cdecl Apply(const D2RL::PluginContext* context, void*) noexcept {
	g_queued = false;
	try {
		const State state = Desired();
		const std::vector<bool> want = Wanted(state);
		const bool recheck = g_recheck.exchange(false);
		for (size_t i = 0; i < g_pieces.size(); ++i) {
			Piece& piece = g_pieces[i];
			if (recheck && piece.open) {  // the game closes panels on its own (loading, leaving)
				D2RL::Panels::PanelInfo info { .structSize = D2RL::Panels::PanelInfoSize };
				if (g_panels->getPanelInfo(context, piece.panel, &info) == D2RL::Panels::Result::Success &&
					info.presentationState != D2RL::Panels::PresentationState::Open) {
					piece.open = false;
				}
			}
			if (piece.open && !want[i]) {
				(void)g_panels->closePanel(context, piece.panel);
				piece.open = false;
			}
		}
		for (size_t i = 0; i < g_pieces.size(); ++i) {
			Piece& piece = g_pieces[i];
			if (want[i] && !piece.open && piece.panel != D2RL::Panels::InvalidHandle) {
				const auto result = g_panels->openPanel(context, piece.panel);
				if (result == D2RL::Panels::Result::Success) {
					piece.open = true;
				} else if (!g_loggedFailure) {
					g_loggedFailure = true;
					char message[128] {};
					std::snprintf(message, sizeof(message), "Wave counter: opening %s gave code %d.", piece.localId.c_str(), static_cast<int>(result));
					context->LogWarn(message);
				}
			}
		}
		g_applied = Encode(state);
	} catch (...) {
	}
}

// Its own thread: about 5 times a second, asks the UI thread to update when the wanted state
// changed, and once a second while something is up (in case the game closed a panel).
void Ticker(const D2RL::PluginContext* context) noexcept {
	int64_t lastCheck = 0;
	for (;;) {
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		const State want = Desired();
		const int64_t now = NowMs();
		const bool shown = want.wave != 0 || want.cleared;
		if (shown && now - lastCheck >= RecheckMs) {
			lastCheck = now;
			g_recheck = true;
		}
		if ((Encode(want) != g_applied.load() || g_recheck.load()) && !g_queued.exchange(true)) {
			if (g_threads->runOnUiThread(context, Apply, nullptr) != D2RL::Threads::Result::Success) {
				g_queued = false;
			}
		}
	}
}

// A panel holding one line of text. `h` is "left", "center" or "right": the text box is 1400
// wide, starting at x (from the screen's centre).
auto PieceLayout(const std::string& localId, int x, int y, const char* h, const std::string& text, const char* color, const char* size) -> std::string {
	return "{\"type\":\"Panel\",\"name\":\"darkestdiablo/" + localId + "\",\"fields\":{\"anchor\":{\"x\":0.5,\"y\":0.0},\"priority\":100},"
		"\"children\":[{\"type\":\"TextBoxWidget\",\"name\":\"Text\",\"fields\":{\"rect\":{\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) +
		",\"width\":1400,\"height\":100},\"text\":\"" + text + "\",\"style\":{\"fontColor\":\"" + color + "\",\"pointSize\":\"" + size +
		"\",\"alignment\":{\"h\":\"" + h + "\",\"v\":\"center\"},\"dropShadow\":\"$DefaultDropShadow\"}}}]}";
}

void Add(const std::string& localId, std::string layout) {
	Piece piece;
	piece.localId = localId;
	piece.layout  = std::move(layout);
	g_pieces.push_back(std::move(piece));
}

// "Wave 3" ends just left of the centre and "/ 6" starts just right of it, so together they read
// "Wave 3 / 6" centred. Made in the order they are registered: most needed first, so if the
// 64-panel limit is reached only the least needed are left out.
constexpr int TotalsByNeed[MaxWaves] = { 4, 5, 6, 3, 8, 1, 2, 7 };  // arena wave counts, most common first

void MakePieces() {
	g_pieces.reserve(MaxWaves * 2 + CountdownSeconds + 1);
	for (int k = 1; k <= MaxWaves; ++k) {
		const std::string id = "WaveK" + std::to_string(k);
		g_waveK[k] = static_cast<int>(g_pieces.size());
		Add(id, PieceLayout(id, -1400 - 12, TitleY, "right", "Wave " + std::to_string(k), "$FontColorGoldYellow", "$ExtraLargeFontSize"));
	}
	for (int s = 1; s <= CountdownSeconds; ++s) {
		const std::string id = "WaveNext" + std::to_string(s);
		g_next[s] = static_cast<int>(g_pieces.size());
		Add(id, PieceLayout(id, -700, SubY, "center", "Next wave in " + std::to_string(s), s <= 3 ? "$FontColorRed" : "$FontColorWhite", "$LargeFontSize"));
	}
	for (const int n : TotalsByNeed) {
		const std::string id = "WaveN" + std::to_string(n);
		g_waveN[n] = static_cast<int>(g_pieces.size());
		Add(id, PieceLayout(id, 12, TitleY, "left", "/ " + std::to_string(n), "$FontColorGoldYellow", "$ExtraLargeFontSize"));
	}
	g_clear = static_cast<int>(g_pieces.size());
	Add("WaveCleared", PieceLayout("WaveCleared", -700, TitleY, "center", "Arena cleared", "$FontColorGoldYellow", "$ExtraLargeFontSize"));
}

}  // namespace

auto Load(const D2RL::PluginContext* context) noexcept -> bool {
	if (context->QueryService(&g_threads) != D2RL::ServiceQueryResult::Success || !D2RL::HasThreadServiceField(g_threads, D2RL::ThreadServiceRequiredSize) ||
		context->QueryService(&g_resources) != D2RL::ServiceQueryResult::Success || !D2RL::HasResourceServiceField(g_resources, D2RL::ResourceServiceRequiredSize) ||
		context->QueryService(&g_panels) != D2RL::ServiceQueryResult::Success || !D2RL::HasPanelServiceField(g_panels, D2RL::PanelServiceRequiredSize)) {
		context->LogWarn("Wave counter: panel, resource or thread service missing; no wave counter on screen.");
		return true;
	}
	try {
		MakePieces();
		int registered = 0;
		for (Piece& piece : g_pieces) {
			const std::string path = "data/global/ui/layouts/darkestdiablo/" + piece.localId + "hd.json";
			const D2RL::Resources::ResourceRegistration resource {
				.structSize = D2RL::Resources::ResourceRegistrationSize,
				.path       = path.c_str(),
				.bytes      = piece.layout.data(),
				.byteCount  = static_cast<uint32_t>(piece.layout.size()),
			};
			D2RL::Resources::RegistrationHandle resourceHandle = D2RL::Resources::InvalidHandle;
			const D2RL::Panels::PanelRegistration registration {
				.structSize = D2RL::Panels::PanelRegistrationSize,
				.flags      = D2RL::Panels::PanelFlags::None,
				.localId    = piece.localId.c_str(),
			};
			if (g_resources->registerResource(context, &resource, &resourceHandle) != D2RL::Resources::Result::Success ||
				g_panels->registerPanel(context, &registration, &piece.panel) != D2RL::Panels::Result::Success) {
				piece.panel = D2RL::Panels::InvalidHandle;
				if (resourceHandle != D2RL::Resources::InvalidHandle) {
					(void)g_resources->unregisterResource(context, resourceHandle);
				}
				break;  // most likely the 64-panel limit: the rest won't fit either
			}
			++registered;
		}
		if (registered == 0) {
			g_pieces.clear();
			context->LogWarn("Wave counter: could not register any text panel; no wave counter on screen.");
			return true;
		}
		std::thread(Ticker, context).detach();
		char message[160] {};
		std::snprintf(message, sizeof(message), "Wave counter: ready (%d of %d text panels%s).", registered, static_cast<int>(g_pieces.size()),
			registered < static_cast<int>(g_pieces.size()) ? "; the rest hit the plugin's panel limit" : "");
		(registered < static_cast<int>(g_pieces.size()) ? context->LogWarn(message) : context->LogInfo(message));
	} catch (...) {
		g_pieces.clear();
		context->LogWarn("Wave counter: could not set up; no wave counter on screen.");
	}
	return true;
}

void OnPlayerReady() noexcept {
	g_recheck = true;
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
