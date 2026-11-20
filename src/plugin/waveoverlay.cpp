// The wave counter at the top of the screen. See waveoverlay.h.

#include "waveoverlay.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace WaveOverlay {
namespace {

const D2RL::OverlayService* g_overlay = nullptr;

enum class Mode : int { Hidden = 0, Waves = 1, Cleared = 2, Demo = 3 };

std::atomic<int>     g_mode { 0 };
std::atomic<int>     g_wave { 0 };
std::atomic<int>     g_total { 0 };
std::atomic<int64_t> g_nextWaveAt { -1 };  // steady-clock ms when the next wave spawns, -1 = none
std::atomic<int64_t> g_since { 0 };        // steady-clock ms when Cleared() or Demo() began

constexpr int64_t ClearedShowMs  = 5000;
constexpr int     DemoWaves      = 5;
constexpr int64_t DemoIntervalMs = 10000;

auto NowMs() noexcept -> int64_t {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Painter {
	const D2RL::PluginContext*  context;
	const D2RL::Overlay::Frame* frame;

	auto Measure(const char* text, float size) const noexcept -> float {
		const D2RL::Overlay::MeasureTextRequest request {
			.structSize = D2RL::Overlay::MeasureTextRequestSize,
			.canvas     = frame->canvas,
			.textSize   = size,
			.text       = text,
			.textLength = static_cast<uint32_t>(std::strlen(text)),
		};
		D2RL::Overlay::TextMetrics metrics { .structSize = D2RL::Overlay::TextMetricsSize };
		if (g_overlay->measureText(context, &request, &metrics) != D2RL::Overlay::Result::Success) {
			return size * 0.5F * static_cast<float>(std::strlen(text));  // a rough guess
		}
		return metrics.width;
	}

	void Text(const char* text, float centreX, float top, float size, D2RL::Overlay::Color color) const noexcept {
		const float left = centreX - Measure(text, size) * 0.5F;
		const float shadow = size / 16.0F;
		for (int pass = 0; pass < 2; ++pass) {  // a dark shadow first, so it reads on bright ground
			const D2RL::Overlay::TextRequest request {
				.structSize = D2RL::Overlay::TextRequestSize,
				.canvas     = frame->canvas,
				.position   = { left + (pass == 0 ? shadow : 0.0F), top + (pass == 0 ? shadow : 0.0F) },
				.color      = pass == 0 ? D2RL::Overlay::Color { 0.0F, 0.0F, 0.0F, color.alpha * 0.9F } : color,
				.textSize   = size,
				.text       = text,
				.textLength = static_cast<uint32_t>(std::strlen(text)),
			};
			(void)g_overlay->drawText(context, &request);
		}
	}

	void Box(float centreX, float top, float width, float height, float alpha) const noexcept {
		const D2RL::Overlay::FilledRectangleRequest fill {
			.structSize = D2RL::Overlay::FilledRectangleRequestSize,
			.canvas     = frame->canvas,
			.rect       = { centreX - width * 0.5F, top, centreX + width * 0.5F, top + height },
			.color      = { 0.03F, 0.02F, 0.02F, 0.72F * alpha },
			.rounding   = height * 0.18F,
		};
		(void)g_overlay->drawFilledRectangle(context, &fill);
		const D2RL::Overlay::RectangleRequest edge {
			.structSize = D2RL::Overlay::RectangleRequestSize,
			.canvas     = frame->canvas,
			.rect       = fill.rect,
			.color      = { 0.55F, 0.45F, 0.25F, 0.8F * alpha },  // dull gold, like the game's frames
			.rounding   = fill.rounding,
			.thickness  = 1.5F,
		};
		(void)g_overlay->drawRectangle(context, &edge);
	}
};

constexpr D2RL::Overlay::Color Gold  { 0.85F, 0.75F, 0.48F, 1.0F };
constexpr D2RL::Overlay::Color White { 0.95F, 0.95F, 0.92F, 1.0F };
constexpr D2RL::Overlay::Color Red   { 0.95F, 0.25F, 0.18F, 1.0F };

// What to show this frame: the title line and, if any, the line under it.
struct Lines {
	char  title[64] {};
	char  sub[64] {};
	bool  urgent = false;  // the last seconds: the countdown turns red
	float alpha  = 1.0F;
};

void WaveLines(Lines& out, int wave, int total, int64_t msToNext) noexcept {
	if (wave <= 0) {
		std::snprintf(out.title, sizeof(out.title), "Wave 1 / %d", total);
	} else {
		std::snprintf(out.title, sizeof(out.title), "Wave %d / %d", wave, total);
	}
	if (msToNext >= 0 && msToNext <= CountdownSeconds * 1000) {
		const int seconds = static_cast<int>((msToNext + 999) / 1000);  // 4.2 s left shows 5
		std::snprintf(out.sub, sizeof(out.sub), wave <= 0 ? "Starts in %d" : "Next wave in %d", seconds < 1 ? 1 : seconds);
		out.urgent = seconds <= 3;
	} else if (msToNext < 0 && wave >= total && total > 0) {
		std::snprintf(out.sub, sizeof(out.sub), "Final wave: clear the arena");
	}
}

auto Compute(Lines& out) noexcept -> bool {
	const int64_t now = NowMs();
	switch (static_cast<Mode>(g_mode.load())) {
	case Mode::Waves: {
		const int64_t at = g_nextWaveAt.load();
		WaveLines(out, g_wave.load(), g_total.load(), at < 0 ? -1 : (at > now ? at - now : 0));
		return true;
	}
	case Mode::Cleared: {
		const int64_t shown = now - g_since.load();
		if (shown > ClearedShowMs) {
			g_mode = static_cast<int>(Mode::Hidden);
			return false;
		}
		std::snprintf(out.title, sizeof(out.title), "Arena cleared");
		const int64_t fade = ClearedShowMs - shown;  // fade out over the last second
		out.alpha = fade < 1000 ? static_cast<float>(fade) / 1000.0F : 1.0F;
		return true;
	}
	case Mode::Demo: {
		const int64_t t = now - g_since.load() + DemoIntervalMs - 1000;  // wave 1 a second after start
		const int wave = static_cast<int>(t / DemoIntervalMs);
		if (wave > DemoWaves) {  // ran past the last wave: show the clear for a few seconds
			const int64_t shown = t - (DemoWaves + 1) * DemoIntervalMs;
			if (shown > ClearedShowMs) {
				g_mode = static_cast<int>(Mode::Hidden);
				return false;
			}
			std::snprintf(out.title, sizeof(out.title), "Arena cleared");
			return true;
		}
		WaveLines(out, wave, DemoWaves, wave < DemoWaves ? (wave + 1) * DemoIntervalMs - t : -1);
		return true;
	}
	default:
		return false;
	}
}

void __cdecl Draw(const D2RL::PluginContext* context, const D2RL::Overlay::Frame* frame, void*) noexcept {
	if (frame == nullptr || g_overlay == nullptr) {
		return;
	}
	Lines lines;
	if (!Compute(lines)) {
		return;
	}
	const Painter paint { context, frame };
	const float scale   = frame->screenHeight / 1080.0F;  // sizes below are for a 1080p screen
	const float titleSz = 34.0F * scale;
	const float subSz   = 24.0F * scale;
	const float centreX = frame->screenWidth * 0.5F;
	const float top     = 18.0F * scale;
	const float pad     = 14.0F * scale;
	const bool  hasSub  = lines.sub[0] != '\0';
	float width = paint.Measure(lines.title, titleSz);
	if (hasSub) {
		const float subWidth = paint.Measure(lines.sub, subSz);
		width = subWidth > width ? subWidth : width;
	}
	const float height = pad * 2.0F + titleSz + (hasSub ? subSz + 6.0F * scale : 0.0F);
	paint.Box(centreX, top, width + pad * 3.0F, height, lines.alpha);
	D2RL::Overlay::Color title = Gold;
	title.alpha = lines.alpha;
	paint.Text(lines.title, centreX, top + pad, titleSz, title);
	if (hasSub) {
		paint.Text(lines.sub, centreX, top + pad + titleSz + 6.0F * scale, subSz, lines.urgent ? Red : White);
	}
}

}  // namespace

auto Load(const D2RL::PluginContext* context) noexcept -> bool {
	if (context->QueryService(&g_overlay) != D2RL::ServiceQueryResult::Success || !D2RL::HasOverlayServiceField(g_overlay, D2RL::OverlayServiceRequiredSize)) {
		g_overlay = nullptr;
		context->LogWarn("Wave counter: overlay service missing; no wave counter on screen.");
		return true;
	}
	const D2RL::Overlay::CallbackRegistration registration {
		.structSize = D2RL::Overlay::CallbackRegistrationSize,
		.phase      = D2RL::Overlay::Phase::AfterGameUi,
		.priority   = 0,
		.callback   = Draw,
	};
	D2RL::Overlay::CallbackHandle handle = D2RL::Overlay::InvalidCallbackHandle;
	const auto result = g_overlay->registerFrameCallback(context, &registration, &handle);
	if (result != D2RL::Overlay::Result::Success) {
		char message[96] {};
		std::snprintf(message, sizeof(message), "Wave counter: could not register the overlay (code %d).", static_cast<int>(result));
		context->LogWarn(message);
		g_overlay = nullptr;
		return true;  // the mod still works without it
	}
	context->LogInfo("Wave counter: overlay ready.");
	return true;
}

void SetWaves(int wave, int total, int nextWaveInMs) noexcept {
	g_wave        = wave;
	g_total       = total;
	g_nextWaveAt  = nextWaveInMs < 0 ? -1 : NowMs() + nextWaveInMs;
	g_mode        = static_cast<int>(Mode::Waves);
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
