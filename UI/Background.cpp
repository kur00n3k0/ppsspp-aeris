#include <algorithm>
#include <cmath>
#include "Common/GPU/thin3d.h"
#include "Common/Data/Text/I18n.h"
#include "Common/Math/geom2d.h"
#include "Common/Math/curves.h"
#include "Common/Data/Random/Rng.h"
#include "Common/Data/Color/RGBAUtil.h"
#include "Common/UI/Context.h"
#include "Common/UI/UI.h"
#include "Common/File/Path.h"
#include "Common/File/FileUtil.h"
#include "Common/TimeUtil.h"
#include "Common/Render/ManagedTexture.h"
#include "Common/System/System.h"
#include "Common/System/Display.h"
#include "Common/System/Request.h"
#include "Core/ConfigValues.h"
#include "Core/Config.h"
#include "Core/System.h"
#include "Core/Util/RecentFiles.h"
#include "UI/Background.h"
#include "UI/GameInfoCache.h"


#ifdef _MSC_VER
#pragma execution_character_set("utf-8")
#endif

static Draw::Texture *bgTexture;

class Animation {
public:
	virtual ~Animation() = default;
	virtual void Draw(UIContext &dc, double t, float alpha, Lin::Vec3 focus) = 0;
};

class MovingBackground : public Animation {
public:
	void Draw(UIContext &dc, double t, float alpha, Lin::Vec3 focus) override {
		if (!bgTexture)
			return;

		dc.Flush();
		dc.GetDrawContext()->BindTexture(0, bgTexture);
		Bounds bounds = dc.GetBounds();

		const float x = std::min(std::max(focus.x / bounds.w, 0.0f), 1.0f) * XFAC;
		const float y = std::min(std::max(focus.y / bounds.h, 0.0f), 1.0f) * YFAC;
		const float z = 1.0f + std::max(XFAC, YFAC) + (focus.z - 1.0f) * ZFAC;

		lastX_ = abs(x - lastX_) > 0.001f ? x * XSPEED + lastX_ * (1.0f - XSPEED) : x;
		lastY_ = abs(y - lastY_) > 0.001f ? y * YSPEED + lastY_ * (1.0f - YSPEED) : y;
		lastZ_ = abs(z - lastZ_) > 0.001f ? z * ZSPEED + lastZ_ * (1.0f - ZSPEED) : z;

		float u1 = lastX_ / lastZ_;
		float v1 = lastY_ / lastZ_;
		float u2 = (1.0f + lastX_) / lastZ_;
		float v2 = (1.0f + lastY_) / lastZ_;

		dc.Draw()->DrawTexRect(bounds, u1, v1, u2, v2, whiteAlpha(alpha));

		dc.Flush();
		dc.RebindTexture();
	}

private:
	static constexpr float XFAC = 0.3f;
	static constexpr float YFAC = 0.3f;
	static constexpr float ZFAC = 0.12f;
	static constexpr float XSPEED = 0.05f;
	static constexpr float YSPEED = 0.05f;
	static constexpr float ZSPEED = 0.1f;

	float lastX_ = 0.0f;
	float lastY_ = 0.0f;
	float lastZ_ = 1.0f + std::max(XFAC, YFAC);
};

class WaveAnimation : public Animation {
public:
	void Draw(UIContext &dc, double t, float alpha, Lin::Vec3 focus) override {
		const uint32_t color = colorAlpha(0xFFFFFFFF, alpha * 0.2f);
		const float speed = 1.0;

		Bounds bounds = dc.GetBounds();
		dc.Flush();
		dc.BeginNoTex();

		// 500 is enough for any resolution really. 24 * 500 = 12000 which fits handily in our UI vertex buffer (max 65536 per flush).
		const int steps = std::max(20, std::min((int)g_display.dp_xres, 500));
		float step = (float)g_display.dp_xres / (float)steps;
		t *= speed;

		for (int n = 0; n < steps; n++) {
			float x = (float)n * step;
			float nextX = (float)(n + 1) * step;
			float i = x * 1280 / bounds.w;

			float wave0 = sin(i * 0.005 + t * 0.8) * 0.05 + sin(i * 0.002 + t * 0.25) * 0.02 + sin(i * 0.001 + t * 0.3) * 0.03 + 0.625;
			float wave1 = sin(i * 0.0044 + t * 0.4) * 0.07 + sin(i * 0.003 + t * 0.1) * 0.02 + sin(i * 0.001 + t * 0.3) * 0.01 + 0.625;
			dc.Draw()->RectVGradient(x, wave0 * bounds.h, nextX, bounds.h, color, 0x00000000);
			dc.Draw()->RectVGradient(x, wave1 * bounds.h, nextX, bounds.h, color, 0x00000000);

			// Add some "antialiasing"
			dc.Draw()->RectVGradient(x, wave0 * bounds.h - 3.0f * g_display.pixel_in_dps_y, nextX, wave0 * bounds.h, 0x00000000, color);
			dc.Draw()->RectVGradient(x, wave1 * bounds.h - 3.0f * g_display.pixel_in_dps_y, nextX, wave1 * bounds.h, 0x00000000, color);
		}

		dc.Flush();
		dc.Begin();
	}
};

class FloatingSymbolsAnimation : public Animation {
public:
	FloatingSymbolsAnimation(bool is_colored) {
		this->is_colored = is_colored;
	}

	void Draw(UIContext &dc, double t, float alpha, Lin::Vec3 focus) override {
		float xres = dc.GetBounds().w;
		float yres = dc.GetBounds().h;

		dc.Flush();
		dc.Begin();
		if (last_xres != xres || last_yres != yres) {
			Regenerate(xres, yres);
		}

		for (size_t i = 0; i < base.size(); i++) {
			const float x = base[i].x + dc.GetBounds().x;
			const float y = base[i].y + dc.GetBounds().y + 40 * cosf(i * 7.2f + t * 1.3f);
			const float angle = sinf(i + t);
			int n = i & 3;
			Color color = is_colored ? colorAlpha(COLORS[n], alpha * 0.25f) : colorAlpha(DEFAULT_COLOR, alpha * 0.1f);
			ui_draw2d.DrawImageRotated(SYMBOLS[n], x, y, 1.0f, angle, color);
		}
		dc.Flush();
	}

private:
	static constexpr Color DEFAULT_COLOR = 0xC0FFFFFF;
	static constexpr Color COLORS[4] = {0xFFE3B56F, 0xFF615BFF, 0xFFAA88F3, 0xFFC2CC7A,}; // X O D A
	static const ImageID SYMBOLS[4];

	bool is_colored = false;
	std::vector<Point2D> base;
	float last_xres = 0;
	float last_yres = 0;

	void Regenerate(int xres, int yres) {
		int count = xres * yres / (120.0f * 120.0f);
		count = std::clamp(count, 10, 1000);
		base.resize(count);

		GMRng rng;
		rng.Init(time_now_d() * 100239);
		for (size_t i = 0; i < base.size(); i++) {
			base[i] = { rng.F() * xres, rng.F() * yres };
		}

		last_xres = xres;
		last_yres = yres;
	}
};

const ImageID FloatingSymbolsAnimation::SYMBOLS[4] = {
	ImageID("I_CROSS"),
	ImageID("I_CIRCLE"),
	ImageID("I_SQUARE"),
	ImageID("I_TRIANGLE"),
};

class RecentGamesAnimation : public Animation {
public:
	void Draw(UIContext &dc, double t, float alpha, Lin::Vec3 focus) override {
		if (lastIndex_ == nextIndex_) {
			CheckNext(dc, t);
		} else if (t > nextT_) {
			lastIndex_ = nextIndex_;
		}

		if (g_recentFiles.HasAny()) {
			std::shared_ptr<GameInfo> lastInfo = GetInfo(dc, lastIndex_);
			std::shared_ptr<GameInfo> nextInfo = GetInfo(dc, nextIndex_);
			dc.Flush();

			float lastAmount = Clamp((float)(nextT_ - t) * 1.0f / TRANSITION, 0.0f, 1.0f);
			DrawTex(dc, lastInfo, lastAmount * alpha * 0.2f);

			float nextAmount = lastAmount <= 0.0f ? 1.0f : 1.0f - lastAmount;
			DrawTex(dc, nextInfo, nextAmount * alpha * 0.2f);

			dc.RebindTexture();
		}
	}

private:
	void CheckNext(UIContext &dc, double t) {
		if (!g_recentFiles.HasAny()) {
			return;
		}

		std::vector<std::string> recents = g_recentFiles.GetRecentFiles();

		for (int index = lastIndex_ + 1; index != lastIndex_; ++index) {
			if (index < 0 || index >= (int)recents.size()) {
				if (lastIndex_ == -1)
					break;
				index = 0;
			}

			std::shared_ptr<GameInfo> ginfo = GetInfo(dc, index);
			if (ginfo && !ginfo->Ready(GameInfoFlags::PIC1)) {
				// Wait for it to load.  It might be the next one.
				break;
			}
			if (ginfo && ginfo->pic1.texture) {
				nextIndex_ = index;
				nextT_ = t + INTERVAL;
				break;
			}

			// Otherwise, keep going.  This skips games with no BG.
		}
	}

	static std::shared_ptr<GameInfo> GetInfo(UIContext &dc, int index) {
		if (index < 0) {
			return nullptr;
		}
		const auto recentIsos = g_recentFiles.GetRecentFiles();
		if (index >= (int)recentIsos.size())
			return std::shared_ptr<GameInfo>();
		return g_gameInfoCache->GetInfo(dc.GetDrawContext(), Path(recentIsos[index]), GameInfoFlags::PIC1);
	}

	static void DrawTex(UIContext &dc, std::shared_ptr<GameInfo> &ginfo, float amount) {
		if (!ginfo || amount <= 0.0f)
			return;
		GameInfoTex *pic = ginfo->GetPIC1();
		if (!pic)
			return;

		dc.GetDrawContext()->BindTexture(0, pic->texture);
		uint32_t color = whiteAlpha(amount) & 0xFFc0c0c0;
		dc.Draw()->DrawTexRect(dc.GetBounds(), 0, 0, 1, 1, color);
		dc.Flush();
	}

	static constexpr double INTERVAL = 8.0;
	static constexpr float TRANSITION = 3.0f;

	int lastIndex_ = -1;
	int nextIndex_ = -1;
	double nextT_ = -INTERVAL;
};

class BouncingIconAnimation : public Animation {
public:
	void Draw(UIContext &dc, double t, float alpha, Lin::Vec3 focus) override {
		dc.Flush();
		dc.Begin();

		// Handle change in resolution.
		float xres = dc.GetBounds().w;
		float yres = dc.GetBounds().h;
		if (last_xres != xres || last_yres != yres) {
			Recalculate(xres, yres);
		}

		// Draw the image.
		float xpos = xbase + dc.GetBounds().x;
		float ypos = ybase + dc.GetBounds().y;
		ImageID icon = !color_ix && System_GetPropertyBool(SYSPROP_APP_GOLD) ? ImageID("I_ICON_GOLD") : ImageID("I_ICON");
		ui_draw2d.DrawImage(icon, xpos, ypos, scale, COLORS[color_ix], ALIGN_CENTER);
		dc.Flush();

		// Switch direction if within border.
		float xmax = xres - border;
		float ymax = yres - border;
		bool should_recolor = false;

		if (xbase > xmax || xbase < border) {
			xspeed = -xspeed;
			should_recolor = true;
		}

		if (ybase > ymax || ybase < border) {
			yspeed = -yspeed;
			should_recolor = true;
		}

		if (should_recolor) {
			RandomizeColor();
		}

		// Place to border if out of bounds.
		xbase = Clamp(xbase, border, xmax);
		ybase = Clamp(ybase, border, ymax);

		// Update location.
		xbase += xspeed;
		ybase += yspeed;
	}

private:
	static constexpr int COLOR_COUNT = 11;
	static constexpr Color COLORS[COLOR_COUNT] = {0xFFFFFFFF, 0xFFFFFF00, 0xFFFF0000, 0xFF00FF00, 0xFF0000FF,
			0xFF00FFFF, 0xFFFF00FF, 0xFF4111D1, 0xFF3577F3, 0xFFAA77FF, 0xFF623B84};
	static constexpr float BORDER_SIZE = 35.0f;

	float xbase = 0.0f;
	float ybase = 0.0f;
	float last_xres = 0.0f;
	float last_yres = 0.0f;
	float xspeed = 1.0f;
	float yspeed = 1.0f;
	float scale = 1.0f;
	float border = BORDER_SIZE;
	int color_ix = 0;
	int last_color_ix = -1;
	GMRng rng;

	void Recalculate(int xres, int yres) {
		// First calculation.
		if (last_color_ix == -1) {
			xbase = xres / 2.0f;
			ybase = yres / 2.0f;
			last_color_ix = 0;

			// Determine initial direction.
			rng.Init(time_now_d() * 100239);
			if (rng.R32() % 2 != 0) xspeed = -xspeed;
			if (rng.R32() % 2 != 0) yspeed = -yspeed;
		}

		// Scale certain attributes to resolution.
		scale = std::min(xres, yres) / 400.0f;
		float speed = scale < 2.5f ? scale * 0.58f : scale * 0.46f;
		xspeed = std::copysign(speed, xspeed);
		yspeed = std::copysign(speed, yspeed);
		border = BORDER_SIZE * scale;

		last_xres = xres;
		last_yres = yres;
	}

	void RandomizeColor() {
		do {
			color_ix = rng.R32() % COLOR_COUNT;
		} while (color_ix == last_color_ix);

		last_color_ix = color_ix;
	}
};

// The AERIS XMB background: a vertical gradient with a sky glow and a corner glow, a faded grid, and the
// ribbon (two glow bands and three bundles of lines fanned around their master curves) with sparkles
// drifting over it. The curves reproduce the mockup's, with u running 0 to 1 across the screen.
class XmbRibbonAnimation : public Animation {
	static constexpr double TWO_PI = 6.283185307179586;

public:
	void Draw(UIContext &dc, double t, float alpha, Lin::Vec3 focus) override {
		const Bounds b = dc.GetBounds();
		const UI::Theme &theme = dc.GetTheme();
		const uint32_t accent = theme.xmbAccent;
		const uint32_t accent2 = theme.xmbAccent2;

		// A custom background image replaces the gradient, the glows and the grid.
		const bool drawBase = bgTexture == nullptr;

		dc.Flush();
		dc.BeginNoTex();
		if (drawBase) {
			const GradientStop stops[3] = {
				{ 0.0f, alphaMul(theme.xmbBgTop, alpha) },
				{ 0.46f, alphaMul(theme.backgroundColor | 0xFF000000, alpha) },
				{ 1.0f, alphaMul(theme.xmbBgBottom, alpha) },
			};
			dc.Draw()->MultiVGradient(b.x, b.y, b.w, b.h, stops, 3);
		}
		dc.Flush();

		dc.Begin();
		if (drawBase) {
			const ImageID glow("I_XMB_GLOW");
			const uint32_t sky = colorBlend(0xFFFFFFFF, accent, 0.45f);
			// Corner glow, bottom left.
			DrawGlow(dc, glow, b.x + 0.04f * b.w, b.y + 1.12f * b.h, 0.70f * b.w, 0.75f * b.h, colorAlpha(accent2, 0.55f * alpha));
			// Sky, top right.
			DrawGlow(dc, glow, b.x + 0.78f * b.w, b.y - 0.12f * b.h, 0.78f * b.w, 0.79f * b.h, colorAlpha(sky, 0.38f * alpha));
			// Sun.
			const float sun = 0.3f * std::max(b.w, b.h * 1.76f);
			DrawGlow(dc, glow, b.x + 0.875f * b.w, b.y - 0.05f * b.h, sun, sun, whiteAlpha(0.55f * alpha));
		}
		dc.Flush();

		dc.BeginNoTex();
		if (drawBase) {
			DrawGrid(dc, b, alpha);
		}
		DrawRibbon(dc, b, t, alpha, accent);
		dc.Flush();

		dc.Begin();
		DrawSparks(dc, b, t, alpha);
		dc.Flush();
	}

private:
	struct Bundle {
		float base, a1, a2, phase;
		double period;
		int lines;
		float spacing;
		float master;  // Alpha of the master line.
		float strength;  // Of the other lines.

		// The lines alternate above and below the master curve, swinging wider the further out they are.
		float Fan(int i) const { return 1.0f + 0.02f * ((i + 1) / 2) * (i % 2 ? 1.0f : -1.0f) * i; }
		float Alpha(int i) const { return i == 0 ? master : strength * std::max(0.08f, 0.42f - 0.025f * i); }
	};

	static float Ribbon(float u, float base, float a1, float a2, float p, double t, double period) {
		const float w = (float)(t * TWO_PI / period);
		return base + a1 * sinf(0.751f + 5.409f * u + p + w) + a2 * sinf(1.571f + 11.31f * u + 1.7f * p - w);
	}

	static void DrawGlow(UIContext &dc, ImageID glow, float cx, float cy, float w, float h, uint32_t color) {
		dc.Draw()->DrawImageStretch(glow, cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f, color);
	}

	// 48 dp lines, faded out by an ellipse around the upper middle of the screen.
	static void DrawGrid(UIContext &dc, const Bounds &b, float alpha) {
		constexpr float spacing = 48.0f;
		constexpr float seg = 24.0f;
		const float lineW = g_display.pixel_in_dps_x;
		auto mask = [&](float x, float y) {
			const float dx = (x - (b.x + b.w * 0.5f)) / (b.w * 0.7f);
			const float dy = (y - (b.y + b.h * 0.4f)) / (b.h * 0.6f);
			const float d = sqrtf(dx * dx + dy * dy);
			return std::clamp((0.8f - d) / 0.6f, 0.0f, 1.0f);
		};
		const float a = 0.045f * alpha;
		for (float x = b.x; x < b.x2(); x += spacing) {
			for (float y = b.y; y < b.y2(); y += seg) {
				const float m0 = mask(x, y), m1 = mask(x, y + seg);
				if (m0 > 0.0f || m1 > 0.0f) {
					dc.Draw()->RectVGradient(x, y, x + lineW, y + seg, whiteAlpha(a * m0), whiteAlpha(a * m1));
				}
			}
		}
		for (float y = b.y; y < b.y2(); y += spacing) {
			for (float x = b.x; x < b.x2(); x += seg) {
				const float m = mask(x + seg * 0.5f, y);
				if (m > 0.0f) {
					dc.Draw()->Rect(x, y, seg, g_display.pixel_in_dps_y, whiteAlpha(a * m));
				}
			}
		}
		dc.Flush();
	}

	static void DrawRibbon(UIContext &dc, const Bounds &b, double t, float alpha, uint32_t accent) {
		// base, a1, a2, phase, period (s), lines, spacing (fraction of the height), master alpha, strength
		static constexpr Bundle bundles[] = {
			{0.735f, 0.061f, 0.027f, 0.0f, 22.0, 10, 0.0037f, 0.8f, 1.0f},
			{0.754f, 0.049f, 0.032f, 1.3f, 28.0, 7, 0.0049f, 0.5f, 0.65f},
			{0.790f, 0.039f, 0.020f, 2.6f, 34.0, 5, 0.0061f, 0.38f, 0.5f},
		};
		const int steps = std::clamp((int)g_display.dp_xres / 3, 20, 500);
		const float step = b.w / steps;
		const float px = g_display.pixel_in_dps_y;
		const uint32_t bandA = colorAlpha(accent, 0.28f * alpha);
		const uint32_t bandB = whiteAlpha(0.12f * alpha);
		for (int n = 0; n < steps; n++) {
			const float x = b.x + n * step;
			const float u = (n + 0.5f) / steps;
			const float edge = std::min(1.0f, std::min(u, 1.0f - u) * 3.3f);
			// Glow bands.
			const float yA = Ribbon(u, 0.735f, 0.056f, 0.027f, 0.0f, t, 26.0);
			const float yB = Ribbon(u, 0.797f, 0.047f, 0.022f, 2.1f, t, 34.0);
			dc.Draw()->RectVGradient(x, b.y + yA * b.h, x + step, b.y2(), bandA, 0);
			dc.Draw()->RectVGradient(x, b.y + yB * b.h, x + step, b.y2(), bandB, 0);
			for (const Bundle &k : bundles) {
				const float y0 = Ribbon(u, k.base, k.a1, k.a2, k.phase, t, k.period);
				for (int i = 0; i < k.lines; i++) {
					const float y = k.base + (y0 - k.base) * k.Fan(i) + i * k.spacing;
					const float thickness = i == 0 ? 2.0f * px : px;
					dc.Draw()->Rect(x, b.y + y * b.h, step, thickness, whiteAlpha(k.Alpha(i) * edge * alpha));
				}
			}
			// 24 rects of 6 vertices per column, so flush well before the buffer fills.
			if ((n & 127) == 127) {
				dc.Flush();
			}
		}
	}

	void DrawSparks(UIContext &dc, const Bounds &b, double t, float alpha) {
		if (sparks_.empty()) {
			GMRng rng;
			rng.Init(0x5EA5);
			sparks_.resize(36);
			static const float sizes[] = { 1.2f, 1.6f, 2.0f, 2.4f, 3.0f, 4.2f, 5.5f };
			for (Spark &s : sparks_) {
				s.u = rng.F();
				s.v = 0.58f + rng.F() * 0.30f;
				s.radius = sizes[rng.R32() % ARRAY_SIZE(sizes)];
				const float dir = (rng.R32() & 1) ? 1.0f : -1.0f;
				s.dx = dir * (60.0f + rng.F() * 120.0f);
				s.dy = -rng.F() * 38.0f;
				s.driftPeriod = 9.0f + rng.F() * 11.0f;
				s.twinklePeriod = 2.5f + rng.F() * 3.5f;
				s.phase = rng.F();
			}
		}
		const ImageID spark("I_XMB_SPARK");
		const AtlasImage *img = dc.Draw()->GetAtlas()->getImage(spark);
		if (!img) {
			return;
		}
		for (const Spark &s : sparks_) {
			const float drift = 0.5f - 0.5f * cosf((float)(TWO_PI * (t / s.driftPeriod + s.phase)));
			const float x = b.x + s.u * b.w + s.dx * drift;
			const float y = b.y + s.v * b.h + s.dy * drift;
			// Keyframes 0, 1, 0.25, 0.9, 0.
			static const float keys[] = { 0.0f, 1.0f, 0.25f, 0.9f, 0.0f };
			const float f = (float)fmod(t / s.twinklePeriod + s.phase * 3.0, 1.0) * 4.0f;
			const int k = std::min(3, (int)f);
			const float a = keys[k] + (keys[k + 1] - keys[k]) * (f - k);
			const float scale = s.radius * 2.0f / img->w;
			dc.Draw()->DrawImage(spark, x, y, scale, whiteAlpha(a * alpha), ALIGN_CENTER);
		}
	}

	struct Spark {
		float u, v, radius, dx, dy, driftPeriod, twinklePeriod, phase;
	};
	std::vector<Spark> sparks_;
};

// TODO: Add more styles. Remember to add to the enum in ConfigValues.h and the selector in GameSettings too.

static BackgroundAnimation g_CurBackgroundAnimation = BackgroundAnimation::OFF;
static std::unique_ptr<Animation> g_Animation;
static bool bgTextureInited = false;  // Separate variable because init could fail.

void UIBackgroundInit(UIContext &dc) {
	const Path bgPng = GetSysDirectory(DIRECTORY_SYSTEM) / "background.png";
	const Path bgJpg = GetSysDirectory(DIRECTORY_SYSTEM) / "background.jpg";
	if (File::Exists(bgPng) || File::Exists(bgJpg)) {
		const Path &bgFile = File::Exists(bgPng) ? bgPng : bgJpg;
		bgTexture = CreateTextureFromFile(dc.GetDrawContext(), bgFile.c_str(), ImageFileType::DETECT, true);
	}
}

void UIBackgroundShutdown() {
	if (bgTexture) {
		bgTexture->Release();
		bgTexture = nullptr;
	}
	bgTextureInited = false;
	g_Animation.reset(nullptr);
	g_CurBackgroundAnimation = BackgroundAnimation::OFF;
}

void DrawBackground(UIContext &dc, float alpha, Lin::Vec3 focus) {
	if (!bgTextureInited) {
		UIBackgroundInit(dc);
		bgTextureInited = true;
	}
	if (g_CurBackgroundAnimation != (BackgroundAnimation)g_Config.iBackgroundAnimation) {
		g_CurBackgroundAnimation = (BackgroundAnimation)g_Config.iBackgroundAnimation;

		switch (g_CurBackgroundAnimation) {
		case BackgroundAnimation::FLOATING_SYMBOLS:
			g_Animation.reset(new FloatingSymbolsAnimation(false));
			break;
		case BackgroundAnimation::RECENT_GAMES:
			g_Animation.reset(new RecentGamesAnimation());
			break;
		case BackgroundAnimation::WAVE:
			g_Animation.reset(new WaveAnimation());
			break;
		case BackgroundAnimation::MOVING_BACKGROUND:
			g_Animation.reset(new MovingBackground());
			break;
		case BackgroundAnimation::BOUNCING_ICON:
			g_Animation.reset(new BouncingIconAnimation());
			break;
		case BackgroundAnimation::FLOATING_SYMBOLS_COLORED:
			g_Animation.reset(new FloatingSymbolsAnimation(true));
			break;
		case BackgroundAnimation::XMB_RIBBON:
			g_Animation.reset(new XmbRibbonAnimation());
			break;
		default:
			g_Animation.reset(nullptr);
		}
	}

	uint32_t bgColor = whiteAlpha(alpha);

	if (bgTexture != nullptr) {
		dc.Flush();
		dc.Begin();
		dc.GetDrawContext()->BindTexture(0, bgTexture);
		dc.Draw()->DrawTexRect(dc.GetBounds(), 0, 0, 1, 1, bgColor);

		dc.Flush();
		dc.RebindTexture();
	} else {
		// I_BG original color: 0xFF754D24
		ImageID img = ImageID("I_BG");
		dc.Begin();
		dc.Draw()->DrawImageStretch(img, dc.GetBounds(), bgColor & dc.GetTheme().backgroundColor);
		dc.Flush();
	}

#if PPSSPP_PLATFORM(IOS)
	// iOS uses an old screenshot when restoring the task, so to avoid an ugly
	// jitter we accumulate time instead.
	static int frameCount = 0.0;
	frameCount++;
	double t = (double)frameCount / System_GetPropertyFloat(SYSPROP_DISPLAY_REFRESH_RATE);
#else
	double t = time_now_d();
#endif

	if (g_Animation) {
		g_Animation->Draw(dc, t, alpha, focus);
	}
}

uint32_t GetBackgroundColorWithAlpha(const UIContext &dc) {
	return colorAlpha(colorBlend(dc.GetTheme().backgroundColor, 0, 0.5f), 0.72f);  // 0.72 = 183 = B7
}

enum class BackgroundFillMode {
	Stretch = 0,
	AdaptiveCropToScreen = 1,
	FitToScreen = 2,
};

void DrawBackgroundTexture(UIContext &dc, Draw::Texture *texture, Lin::Vec3 focus, float alpha) {
	dc.GetDrawContext()->BindTexture(0, texture);
	uint32_t color = whiteAlpha(std::clamp(alpha, 0.0f, 1.0f)) & 0xFFc0c0c0;

	// TODO: Make this configurable?
	const BackgroundFillMode mode = BackgroundFillMode::AdaptiveCropToScreen;

	const Bounds screenBounds = dc.GetBounds();
	float imageW = screenBounds.w;
	float imageH = screenBounds.h;
	float imageAspect = (float)texture->Width() / (float)texture->Height();

	float squash = imageAspect / screenBounds.AspectRatio();

	// Allow a lot of leeway for the image aspect - let it stretch a bit, and only then start cropping.
	const float aspectLeeway = 0.5f;
	if (mode == BackgroundFillMode::AdaptiveCropToScreen && squash >= 0.6f && squash < 1.7f) {
		imageAspect = screenBounds.AspectRatio();
	}

	// Fit the image into the screen bounds according to the fill mode.
	if (imageAspect > screenBounds.AspectRatio()) {
		// Image is wider than screen.
		if (mode == BackgroundFillMode::AdaptiveCropToScreen) {
			// Crop width.
			imageW = screenBounds.h * imageAspect;
		} else if (mode == BackgroundFillMode::FitToScreen) {
			// Fit height.
			imageH = screenBounds.w / imageAspect;
		}
	} else {
		// Image is taller than screen.
		if (mode == BackgroundFillMode::AdaptiveCropToScreen) {
			// Crop height.
			imageH = screenBounds.w / imageAspect;
		} else if (mode == BackgroundFillMode::FitToScreen) {
			// Fit width.
			imageW = screenBounds.h / imageAspect;
		}
	}

	Bounds finalImageBounds = Bounds::FromCenterWH(screenBounds.centerX(), screenBounds.centerY(), imageW, imageH);

	dc.Draw()->DrawTexRect(finalImageBounds, 0, 0, 1, 1, color);
	dc.Flush();
	dc.RebindTexture();
}

void DrawGameBackground(UIContext &dc, const Path &gamePath, Lin::Vec3 focus, float alpha) {
	using namespace Draw;
	using namespace UI;
	dc.Flush();

	std::shared_ptr<GameInfo> ginfo;
	if (!gamePath.empty()) {
		ginfo = g_gameInfoCache->GetInfo(dc.GetDrawContext(), gamePath, GameInfoFlags::PIC1);
	}

	GameInfoTex *pic = (ginfo && ginfo->Ready(GameInfoFlags::PIC1)) ? ginfo->GetPIC1() : nullptr;
	if (pic && pic->texture) {
		float alphaMul = ease((time_now_d() - pic->timeLoaded) * 3.0f);
		DrawBackgroundTexture(dc, pic->texture, focus, alpha * alphaMul);
	} else {
		::DrawBackground(dc, 1.0f, focus);
		dc.RebindTexture();
		dc.Flush();
	}
}
