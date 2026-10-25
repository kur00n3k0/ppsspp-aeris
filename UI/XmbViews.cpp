// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <algorithm>
#include <cmath>
#include <ctime>

#include "ppsspp_config.h"

#include "Common/Data/Color/RGBAUtil.h"
#include "Common/Data/Text/I18n.h"
#include "Common/GPU/thin3d.h"
#include "Common/Render/DrawBuffer.h"
#include "Common/System/Display.h"
#include "Common/System/System.h"
#include "Common/TimeUtil.h"
#include "Common/UI/Context.h"
#include "Common/UI/Root.h"
#include "Core/Config.h"
#include "Core/HLE/sceUtility.h"

#include "UI/XmbViews.h"

namespace Xmb {

// The mockup's colors, as 0xRRGGBB.
static uint32_t Rgb(uint32_t rgb, float alpha = 1.0f) {
	return colorAlpha(COLOR(rgb), alpha);
}

static float Lerp(float a, float b, float t) {
	return a + (b - a) * t;
}

static const FontStyle &MonoFont() {
	static const FontStyle style(FontFamily::Fixed, 14, FontStyleFlags::Default);
	return style;
}

float FocusAnim::Update(bool focused) {
	const double now = time_now_d();
	const float dt = lastTime_ == 0.0 ? 1.0f : (float)std::min(now - lastTime_, 0.1);
	lastTime_ = now;
	const float target = focused ? 1.0f : 0.0f;
	// About 0.4 s to settle, like the mockup's transitions.
	value_ += (target - value_) * std::min(1.0f, dt * 10.0f);
	if (fabsf(value_ - target) < 0.002f) {
		value_ = target;
	}
	return value_;
}

float RowHeight(float focus) {
	return Lerp(ROW_H, ROW_H_FOCUSED, focus);
}

float TileSize(float focus) {
	return Lerp(48.0f, 58.0f, focus);
}

float TextX(float focus, bool wide) {
	if (wide) {
		// A 0.5x (72x40) icon idle, 0.75x (108x60) focused, both hanging off the left of the row a bit.
		return Lerp(ROW_PAD_LEFT - 12.0f + 72.0f - 12.0f, ROW_PAD_LEFT - 25.0f + 108.0f, focus) + ROW_GAP;
	}
	return ROW_PAD_LEFT + TileSize(focus) + ROW_GAP;
}

void DrawRowGlass(UIContext &dc, const Bounds &bounds, float focus, bool down) {
	if (focus <= 0.0f && !down) {
		return;
	}
	const uint32_t accent = dc.GetTheme().xmbAccent;
	dc.Flush();
	dc.Begin();
	// The glow under the row: 0 20px 40px -18px var(--glow).
	const Bounds glow(bounds.x + bounds.w * 0.08f, bounds.y2() - 26.0f, bounds.w * 0.84f, 50.0f);
	dc.Draw()->DrawImageStretch(ImageID("I_XMB_GLOW"), glow, colorAlpha(accent, 0.45f * focus));
	const float a = down ? 1.0f : focus;
	// linear-gradient(180deg, white .2, white .07 52%, white .1), radius 20.
	const float midY = bounds.y + bounds.h * 0.52f;
	dc.Draw()->DrawRoundRectVGradient(bounds.x, bounds.y, bounds.x2(), bounds.y2(), 20.0f, whiteAlpha(0.2f * a), whiteAlpha(0.085f * a));
	dc.Draw()->RectVGradient(bounds.x + 20.0f, midY, bounds.x2() - 20.0f, bounds.y2() - 1.0f, whiteAlpha(0.0f), whiteAlpha(0.025f * a));
	if (down) {
		dc.Draw()->DrawRoundRect(bounds, 20.0f, colorAlpha(accent, 0.25f));
	}
	// inset 0 0 0 1px white .18, and the brighter top edge.
	dc.Draw()->DrawRoundRect(bounds, 20.0f, whiteAlpha(0.18f * a), RoundRectStyle::OutlineThin);
	dc.Draw()->Rect(bounds.x + 18.0f, bounds.y + 1.0f, bounds.w - 36.0f, g_display.pixel_in_dps_y, whiteAlpha(0.2f * a));
	dc.Flush();
}

void DrawRowText(UIContext &dc, float x, float centerY, float maxW, std::string_view title, std::string_view subtitle, float focus) {
	// 22 regular idle, 28 bold focused. Switch fonts halfway and scale to hide the jump.
	const bool bold = focus >= 0.5f;
	const float size = Lerp(22.0f, 28.0f, focus);
	const FontStyle titleFont(FontFamily::SansSerif, bold ? 28 : 22, bold ? FontStyleFlags::Bold : FontStyleFlags::Default);
	const float titleScale = size / (bold ? 28.0f : 22.0f);
	const uint32_t titleColor = colorBlend(0xFFFFFFFF, Rgb(0xD5EAF2), focus);
	const uint32_t subColor = colorBlend(Rgb(0xC6E4EE), Rgb(0x9DBDCB), focus);

	float tw, th;
	dc.MeasureText(titleFont, titleScale, titleScale, title.empty() ? std::string_view("A") : title, &tw, &th);
	float sw = 0.0f, sh = 0.0f;
	if (!subtitle.empty()) {
		dc.MeasureText(MonoFont(), 1.0f, 1.0f, subtitle, &sw, &sh);
	}
	const float gap = subtitle.empty() ? 0.0f : 2.0f;
	const float top = centerY - (th + gap + sh) * 0.5f;

	dc.Flush();
	dc.PushScissor(Bounds(x, centerY - 60.0f, std::max(maxW, 1.0f), 120.0f));
	dc.SetFontStyle(titleFont);
	dc.SetFontScale(titleScale, titleScale);
	dc.DrawText(title, x, top, titleColor, ALIGN_TOPLEFT);
	dc.SetFontScale(1.0f, 1.0f);
	if (!subtitle.empty()) {
		dc.SetFontStyle(MonoFont());
		dc.DrawText(subtitle, x, top + th + gap, subColor, ALIGN_TOPLEFT);
	}
	dc.Flush();
	dc.PopScissor();
	dc.SetFontStyle(dc.GetTheme().uiFont);
}

struct HueColors {
	uint32_t top, bottom, ink;
};

static HueColors GetHueColors(Hue hue) {
	switch (hue) {
	case Hue::Lime: return { Rgb(0xD4FF8E), Rgb(0x4DBF3A), Rgb(0x0B2A05) };
	case Hue::Violet: return { Rgb(0xCFC2FF), Rgb(0x6E5BEA), Rgb(0x120A3A) };
	case Hue::Amber: return { Rgb(0xFFE09A), Rgb(0xF09A2C), Rgb(0x2E1600) };
	case Hue::Coral: return { Rgb(0xFFC1B0), Rgb(0xF2607A), Rgb(0x33060F) };
	case Hue::Aqua:
	default:
		return { Rgb(0x8FF3FF), Rgb(0x16A9D8), Rgb(0x012433) };
	}
}

void DrawTile(UIContext &dc, const Bounds &tile, Hue hue, ImageID icon, std::string_view glyph, float focus) {
	const HueColors colors = GetHueColors(hue);
	const float radius = tile.h * (15.0f / 48.0f);
	dc.Flush();
	dc.Begin();
	// 0 6px 16px -6px rgba(0,0,0,.5)
	dc.Draw()->DrawImage4Grid(dc.GetTheme().dropShadow4Grid, tile.x - 4.0f, tile.y + 2.0f, tile.x2() + 4.0f, tile.y2() + 10.0f, blackAlpha(0.35f), 1.0f);
	dc.Draw()->DrawRoundRectVGradient(tile.x, tile.y, tile.x2(), tile.y2(), radius, colors.top, colors.bottom);
	// The gloss over the top half.
	dc.Draw()->DrawRoundRectVGradient(tile.x + 3.0f, tile.y + 2.0f, tile.x2() - 3.0f, tile.y + tile.h * 0.48f, radius - 3.0f, whiteAlpha(0.6f), whiteAlpha(0.0f));
	dc.Draw()->DrawRoundRect(tile, radius, whiteAlpha(0.35f), RoundRectStyle::OutlineThin);
	if (icon.isValid()) {
		DrawEmbossedIcon(dc, icon, tile.centerX(), tile.centerY(), tile.h * 0.56f, colors.ink, 0.0f);
	} else if (!glyph.empty()) {
		const bool bold = true;
		const int pts = focus >= 0.5f ? 22 : 17;
		dc.SetFontStyle(FontStyle(FontFamily::SansSerif, pts, bold ? FontStyleFlags::Bold : FontStyleFlags::Default));
		dc.DrawText(glyph, tile.centerX(), tile.centerY(), colors.ink, ALIGN_CENTER);
		dc.SetFontStyle(dc.GetTheme().uiFont);
	}
	dc.Flush();
}

void DrawEmbossedIcon(UIContext &dc, ImageID icon, float x, float y, float size, uint32_t color, float embossAlpha) {
	const AtlasImage *img = dc.Draw()->GetAtlas()->getImage(icon);
	if (!img || img->w <= 0 || img->h <= 0) {
		return;
	}
	const float scale = size / std::max(img->w, img->h);
	if (embossAlpha > 0.0f) {
		// drop-shadow(2px 3px 2px rgba(0,40,70,.45)) drop-shadow(-1px -1px 1px rgba(255,255,255,.4))
		dc.Draw()->DrawImage(icon, x + 2.0f, y + 3.0f, scale, colorAlpha(Rgb(0x002846), 0.45f * embossAlpha), ALIGN_CENTER);
		dc.Draw()->DrawImage(icon, x - 1.0f, y - 1.0f, scale, whiteAlpha(0.4f * embossAlpha), ALIGN_CENTER);
	}
	dc.Draw()->DrawImage(icon, x, y, scale, color, ALIGN_CENTER);
}

Item::Item(std::string_view title, std::string_view subtitle, ImageID icon, Hue hue, UI::LayoutParams *layoutParams)
	: UI::Clickable(layoutParams), title_(title), subtitle_(subtitle), icon_(icon), hue_(hue) {}

void Item::GetContentDimensions(const UIContext &dc, float &w, float &h) const {
	w = 400.0f;
	h = RowHeight(focus_.Value());
}

void Item::Draw(UIContext &dc) {
	const float focus = focus_.Update(HasFocus());
	DrawRowGlass(dc, bounds_, focus, down_);
	const float tile = TileSize(focus);
	DrawTile(dc, Bounds(bounds_.x + ROW_PAD_LEFT, bounds_.centerY() - tile * 0.5f, tile, tile), hue_, icon_, "", focus);
	const float textX = bounds_.x + TextX(focus, false);
	DrawRowText(dc, textX, bounds_.centerY(), bounds_.x2() - 20.0f - textX, title_, subtitle_, focus);
}

List::List(UI::LayoutParams *layoutParams) : UI::LinearLayoutList(ORIENT_VERTICAL, layoutParams) {
	SetSpacing(0.0f);
}

bool List::Key(const KeyInput &input) {
	if (UI::LinearLayoutList::Key(input)) {
		return true;
	}
	return search_.Key(this, input);
}

void List::Update() {
	UI::LinearLayoutList::Update();
	if (search_.searchPending) {
		search_.ApplySearchFilter(this, false);
	}
}

void List::ClearSearch() {
	if (!search_.searchFilter.empty()) {
		search_.searchFilter.clear();
		search_.ApplySearchFilter(this, false);
	}
}

// Each category takes a 168 dp wide cell.
static constexpr float CELL_W = 168.0f;
static constexpr float ORB_TOP = 0.0f;
static constexpr float ORB_BOX = 112.0f;

CategoryBar::CategoryBar(std::vector<Category> categories, UI::LayoutParams *layoutParams)
	: UI::View(layoutParams), categories_(std::move(categories)) {
	focus_.resize(categories_.size());
}

void CategoryBar::SetSelected(int index, bool animate) {
	selected_ = std::clamp(index, 0, std::max(0, (int)categories_.size() - 1));
	if (!animate) {
		offset_ = (float)selected_;
		lastTime_ = 0.0;
	}
}

void CategoryBar::GetContentDimensions(const UIContext &dc, float &w, float &h) const {
	w = CELL_W * categories_.size();
	h = 150.0f;
}

int CategoryBar::HitTest(float x, float y) const {
	if (y < bounds_.y || y > bounds_.y2()) {
		return -1;
	}
	const float firstCenter = anchorX_ - offset_ * CELL_W;
	const int index = (int)floorf((x - (firstCenter - CELL_W * 0.5f)) / CELL_W);
	if (index < 0 || index >= (int)categories_.size()) {
		return -1;
	}
	return index;
}

bool CategoryBar::Touch(const TouchInput &input) {
	if (input.flags & TouchInputFlags::DOWN) {
		downIndex_ = HitTest(input.x, input.y);
		return downIndex_ >= 0;
	}
	if (input.flags & TouchInputFlags::UP) {
		const int index = HitTest(input.x, input.y);
		const bool hit = downIndex_ >= 0 && index == downIndex_ && !(input.flags & TouchInputFlags::CANCEL);
		downIndex_ = -1;
		if (hit && index != selected_) {
			UI::PlayUISound(UI::UISound::SELECT);
			UI::EventParams e{};
			e.v = this;
			e.a = index;
			OnChoice.Trigger(e);
		}
		return hit;
	}
	return false;
}

void CategoryBar::Draw(UIContext &dc) {
	const double now = time_now_d();
	const float dt = lastTime_ == 0.0 ? 1.0f : (float)std::min(now - lastTime_, 0.1);
	lastTime_ = now;
	// transform .6s cubic-bezier(.2,.8,.2,1): mostly an ease-out.
	offset_ += ((float)selected_ - offset_) * std::min(1.0f, dt * 7.0f);
	if (fabsf(offset_ - selected_) < 0.001f) {
		offset_ = (float)selected_;
	}

	const UI::Theme &theme = dc.GetTheme();
	const uint32_t accent = theme.xmbAccent;
	const float orbCenterY = bounds_.y + ORB_TOP + ORB_BOX * 0.5f;
	const float firstCenter = anchorX_ - offset_ * CELL_W;

	dc.Flush();
	dc.Begin();
	for (size_t i = 0; i < categories_.size(); i++) {
		const float on = focus_[i].Update((int)i == selected_);
		const float cx = firstCenter + i * CELL_W;
		if (cx < bounds_.x - CELL_W || cx > bounds_.x2() + CELL_W) {
			continue;
		}
		const float orbSize = Lerp(64.0f, 108.0f, on);
		if (on > 0.0f) {
			// 0 14px 26px rgba(0,20,40,.55), and 0 0 22px of the tint.
			const float shadowW = orbSize * 1.5f;
			dc.Draw()->DrawImageStretch(ImageID("I_XMB_GLOW"), cx - shadowW * 0.5f, orbCenterY + 14.0f - shadowW * 0.5f, cx + shadowW * 0.5f, orbCenterY + 14.0f + shadowW * 0.5f, colorAlpha(Rgb(0x001428), 0.55f * on));
			const float glowW = orbSize * 1.45f;
			dc.Draw()->DrawImageStretch(ImageID("I_XMB_GLOW"), cx - glowW * 0.5f, orbCenterY - glowW * 0.5f, cx + glowW * 0.5f, orbCenterY + glowW * 0.5f, colorAlpha(accent, 0.25f * on));
			// The orb, tinted, then its highlights untinted.
			const uint32_t tint = colorBlend(0xFFFFFFFF, accent, 0.45f);
			const float half = orbSize * 0.5f;
			dc.Draw()->DrawImageStretch(ImageID("I_XMB_ORB"), cx - half, orbCenterY - half, cx + half, orbCenterY + half, alphaMul(tint, on));
			dc.Draw()->DrawImageStretch(ImageID("I_XMB_ORB_HL"), cx - half, orbCenterY - half, cx + half, orbCenterY + half, whiteAlpha(on));
			dc.Flush();
			// The ring 14 dp outside it, and the faint band out to 14 dp beyond that.
			dc.BeginNoTex();
			dc.Draw()->Circle(cx, orbCenterY, half + 14.0f + 7.0f, 14.0f, 48, 0.0f, whiteAlpha(0.025f * on), 1.0f);
			dc.Draw()->Circle(cx, orbCenterY, half + 14.0f, g_display.pixel_in_dps_x * 1.5f, 64, 0.0f, whiteAlpha(0.14f * on), 1.0f);
			dc.Flush();
			dc.Begin();
		}
		const uint32_t iconColor = colorBlend(0xFFFFFFFF, Rgb(0xD6EEF7, 0.62f), on);
		DrawEmbossedIcon(dc, categories_[i].icon, cx, orbCenterY, Lerp(34.0f, 44.0f, on), alphaMul(iconColor, 0.9f + 0.1f * on), 1.0f);

		if (on > 0.01f) {
			// Fades in and drops into place under the selected orb.
			dc.Flush();
			dc.SetFontStyle(FontStyle(FontFamily::SansSerif, 17, FontStyleFlags::Bold));
			dc.DrawText(categories_[i].label, cx, bounds_.y + ORB_BOX + 12.0f - 6.0f * (1.0f - on), whiteAlpha(on), ALIGN_HCENTER | ALIGN_TOP);
			dc.SetFontStyle(theme.uiFont);
			dc.Flush();
		}
	}
	dc.Flush();
}

// A chip: 30 dp high, radius 8, a faint fill and outline.
static float DrawChip(UIContext &dc, float right, float centerY, float contentW, uint32_t outline = 0) {
	const float w = contentW + 20.0f;
	const Bounds b(right - w, centerY - 15.0f, w, 30.0f);
	dc.Draw()->DrawRoundRect(b, 8.0f, whiteAlpha(0.055f));
	dc.Draw()->DrawRoundRect(b, 8.0f, outline ? outline : whiteAlpha(0.1f), RoundRectStyle::OutlineThin);
	return b.x;
}

void TopBar::GetContentDimensions(const UIContext &dc, float &w, float &h) const {
	w = 400.0f;
	h = 40.0f;
}

void TopBar::Draw(UIContext &dc) {
	auto mm = GetI18NCategory(I18NCat::MAINMENU);
	const UI::Theme &theme = dc.GetTheme();
	const float cy = bounds_.centerY();
	const FontStyle &mono = MonoFont();

	dc.Flush();
	dc.Begin();

	// Left: brand and breadcrumb.
	float x = bounds_.x;
	float w, h;
	const FontStyle brand(FontFamily::SansSerif, 17, FontStyleFlags::Bold);
	dc.SetFontStyle(brand);
	dc.DrawText("PPSSPP", x, cy, 0xFFFFFFFF, ALIGN_VCENTER);
	dc.MeasureText(brand, 1.0f, 1.0f, "PPSSPP", &w, &h);
	x += w + 14.0f;
	dc.SetFontStyle(mono);
	dc.DrawText("AERIS", x, cy, Rgb(0x9DBDCB), ALIGN_VCENTER);
	dc.MeasureText(mono, 1.0f, 1.0f, "AERIS", &w, &h);
	x += w + 14.0f;
	dc.Draw()->Rect(x, cy - 9.0f, g_display.pixel_in_dps_x, 18.0f, whiteAlpha(0.16f));
	x += 15.0f;
	if (!category_.empty()) {
		dc.DrawText(category_, x, cy, Rgb(0xA9C7D4), ALIGN_VCENTER);
		dc.MeasureText(mono, 1.0f, 1.0f, category_, &w, &h);
		x += w + 8.0f;
		dc.DrawText("/", x, cy, Rgb(0xA9C7D4, 0.5f), ALIGN_VCENTER);
		x += 16.0f;
	}
	const float crumbEnd = x;

	// Right, from the right edge: clock, battery, backend, search.
	float right = bounds_.x2();
	time_t now = time(nullptr);
	struct tm *lt = localtime(&now);
	char clock[64] = "";
	if (lt) {
		strftime(clock, sizeof(clock), "%a %d %b \xC2\xB7 %H:%M", lt);
	}
	dc.MeasureText(mono, 1.0f, 1.0f, clock, &w, &h);
	right = DrawChip(dc, right, cy, w);
	dc.DrawText(clock, right + 10.0f, cy, 0xFFFFFFFF, ALIGN_VCENTER);
	right -= 8.0f;

	// Some platforms report 0 (or -1) when there's no battery, or they haven't read it yet.
	const int battery = System_GetPropertyBool(SYSPROP_CAN_READ_BATTERY_PERCENTAGE) ? (int)System_GetPropertyInt(SYSPROP_BATTERY_PERCENTAGE) : -1;
	if (battery > 0) {
		const int pct = std::min(battery, 100);
		const bool low = pct <= 20;
		const uint32_t color = low ? Rgb(0xFF8A96) : Rgb(0xCFE6EF);
		char text[16];
		snprintf(text, sizeof(text), "%d%%", pct);
		dc.MeasureText(mono, 1.0f, 1.0f, text, &w, &h);
		const float iconW = 22.0f;
		right = DrawChip(dc, right, cy, iconW + 8.0f + w, low ? Rgb(0xFF8A96, 0.35f) : 0);
		const float bx = right + 10.0f;
		dc.Draw()->DrawRoundRect(Bounds(bx, cy - 6.0f, 19.0f, 12.0f), 3.0f, alphaMul(color, 0.7f), RoundRectStyle::OutlineMedium);
		dc.Draw()->Rect(bx + 2.5f, cy - 3.5f, std::max(1.5f, 14.0f * pct / 100.0f), 7.0f, color);
		dc.Draw()->Rect(bx + 20.0f, cy - 2.5f, 2.0f, 5.0f, alphaMul(color, 0.7f));
		dc.DrawText(text, bx + iconW + 8.0f, cy, color, ALIGN_VCENTER);
		right -= 8.0f;
	}

	Draw::DrawContext *draw = dc.GetDrawContext();
	if (draw) {
		std::string api = draw->GetInfoString(Draw::InfoField::APINAME);
		dc.MeasureText(mono, 1.0f, 1.0f, api, &w, &h);
		right = DrawChip(dc, right, cy, w);
		dc.DrawText(api, right + 10.0f, cy, Rgb(0xCFE6EF), ALIGN_VCENTER);
		right -= 8.0f;
	}

	// Type to search: the chip shows the live query, outlined in the accent.
	if (System_GetPropertyBool(SYSPROP_HAS_KEYBOARD) || !search_.empty()) {
		const bool active = !search_.empty();
		std::string_view text = active ? std::string_view(search_) : mm->T("Type to search");
		dc.MeasureText(mono, 1.0f, 1.0f, text, &w, &h);
		const float iconSize = 16.0f;
		const float contentW = std::max(160.0f, iconSize + 8.0f + w + (active ? 6.0f : 0.0f));
		const float left = right - contentW - 20.0f;
		if (left > crumbEnd + 16.0f) {
			right = DrawChip(dc, right, cy, contentW, active ? theme.xmbAccent : 0);
			const uint32_t color = active ? 0xFFFFFFFF : Rgb(0x9FC2D0);
			DrawEmbossedIcon(dc, ImageID("I_SEARCH"), right + 10.0f + iconSize * 0.5f, cy, iconSize, color, 0.0f);
			const float tx = right + 10.0f + iconSize + 8.0f;
			dc.DrawText(text, tx, cy, color, ALIGN_VCENTER);
			if (active && fmod(time_now_d(), 1.0) < 0.6) {
				dc.Draw()->Rect(tx + w + 2.0f, cy - 8.0f, 1.5f, 16.0f, 0xFFFFFFFF);
			}
		}
	}

	// The breadcrumb's location gets whatever room is left.
	if (!location_.empty()) {
		const FontStyle bold(FontFamily::Fixed, 14, FontStyleFlags::Bold);
		dc.Flush();
		dc.PushScissor(Bounds(crumbEnd, bounds_.y, std::max(1.0f, right - 16.0f - crumbEnd), bounds_.h));
		dc.SetFontStyle(bold);
		dc.DrawText(location_, crumbEnd, cy, 0xFFFFFFFF, ALIGN_VCENTER);
		dc.Flush();
		dc.PopScissor();
	}

	dc.SetFontStyle(theme.uiFont);
	dc.Flush();
}

// A key cap: 26 dp, radius 7, with a darker bottom edge. Returns its right edge. Only measures if !draw.
static float DrawKeyCap(UIContext &dc, float x, float cy, std::string_view text, ImageID glyph, uint32_t color, bool draw) {
	float w = 26.0f;
	if (!text.empty()) {
		float tw, th;
		dc.MeasureText(MonoFont(), 1.0f, 1.0f, text, &tw, &th);
		w = std::max(26.0f, tw + 12.0f);
	}
	const Bounds b(x, cy - 13.0f, w, 26.0f);
	if (!draw) {
		return b.x2();
	}
	dc.Draw()->DrawRoundRect(b, 7.0f, whiteAlpha(0.08f));
	dc.Draw()->DrawRoundRect(b, 7.0f, whiteAlpha(0.16f), RoundRectStyle::OutlineThin);
	dc.Draw()->Rect(b.x + 6.0f, b.y2() - 2.0f, b.w - 12.0f, 1.5f, blackAlpha(0.25f));
	if (glyph.isValid()) {
		DrawEmbossedIcon(dc, glyph, b.centerX(), b.centerY(), 13.0f, color, 0.0f);
	} else {
		dc.SetFontStyle(MonoFont());
		dc.DrawText(text, b.centerX(), b.centerY(), color, ALIGN_CENTER);
	}
	return b.x2();
}

// The keyboard key for an action: 22 dp high, outlined.
static float DrawKeyHint(UIContext &dc, float x, float cy, std::string_view text, bool draw) {
	float tw, th;
	dc.MeasureText(MonoFont(), 1.0f, 1.0f, text, &tw, &th);
	const Bounds b(x, cy - 11.0f, tw + 12.0f, 22.0f);
	if (draw) {
		dc.Draw()->DrawRoundRect(b, 5.0f, whiteAlpha(0.14f), RoundRectStyle::OutlineThin);
		dc.SetFontStyle(MonoFont());
		dc.DrawText(text, b.x + 6.0f, b.centerY(), Rgb(0xA9C9D6), ALIGN_VCENTER);
	}
	return b.x2();
}

void HintBar::GetContentDimensions(const UIContext &dc, float &w, float &h) const {
	w = 400.0f;
	h = 44.0f;
}

bool HintBar::Touch(const TouchInput &input) {
	const bool inside = quickActionBounds_.Contains(input.x, input.y);
	if (input.flags & TouchInputFlags::DOWN) {
		quickActionDown_ = inside;
		return inside;
	}
	if ((input.flags & TouchInputFlags::UP) && quickActionDown_) {
		quickActionDown_ = false;
		if (inside && !(input.flags & TouchInputFlags::CANCEL)) {
			UI::EventParams e{};
			e.v = this;
			OnQuickActions.Trigger(e);
		}
		return true;
	}
	return false;
}

void HintBar::Draw(UIContext &dc) {
	auto mm = GetI18NCategory(I18NCat::MAINMENU);
	auto di = GetI18NCategory(I18NCat::DIALOG);
	const float cy = bounds_.centerY();
	const bool confirmCross = g_Config.iButtonPreference == PSP_SYSTEMPARAM_BUTTON_CROSS;
	const uint32_t crossColor = Rgb(0x9EC9FF), circleColor = Rgb(0xFF9DB0), triangleColor = Rgb(0x7EF2C8), capColor = Rgb(0xE2F3F9);
	const FontStyle label(FontFamily::SansSerif, 17, FontStyleFlags::Default);
	const uint32_t labelColor = Rgb(0xB9D5E0);

	struct Cap {
		const char *text;
		const char *glyph;
		uint32_t color;
	};
	struct Hint {
		std::vector<Cap> caps;
		const char *key;  // On the keyboard.
		std::string_view label;
		bool optional;  // Left out first when there's no room.
		bool quickActions;
	};
	const Hint hints[] = {
		{ { { "", confirmCross ? "I_CROSS" : "I_CIRCLE", confirmCross ? crossColor : circleColor } }, "Enter / Space", mm->T("Confirm"), false, false },
		{ { { "", confirmCross ? "I_CIRCLE" : "I_CROSS", confirmCross ? circleColor : crossColor } }, "Esc", di->T("Back"), false, false },
		{ { { "", "I_TRIANGLE", triangleColor } }, "+", mm->T("Info"), true, false },
		{ { { "L", nullptr, capColor }, { "R", nullptr, capColor } }, "PgUp / PgDn", mm->T("Category"), true, false },
		{ { { "SELECT", nullptr, capColor } }, "Ctrl K", mm->T("Quick actions"), false, true },
	};

	// Lays the hints out from x, and returns where they end.
	auto layout = [&](bool draw, bool keyHints, bool optional) {
		float x = bounds_.x;
		for (const Hint &hint : hints) {
			if (hint.optional && !optional) {
				continue;
			}
			const float start = x;
			for (size_t i = 0; i < hint.caps.size(); i++) {
				const Cap &cap = hint.caps[i];
				x = DrawKeyCap(dc, x, cy, cap.text, cap.glyph ? ImageID(cap.glyph) : ImageID::invalid(), cap.color, draw) + (i + 1 < hint.caps.size() ? 4.0f : 8.0f);
			}
			if (keyHints) {
				x = DrawKeyHint(dc, x, cy, hint.key, draw) + 8.0f;
			}
			float w, h;
			dc.MeasureText(label, 1.0f, 1.0f, hint.label, &w, &h);
			if (draw) {
				dc.SetFontStyle(label);
				dc.DrawText(hint.label, x, cy, labelColor, ALIGN_VCENTER);
				if (hint.quickActions) {
					quickActionBounds_ = Bounds(start, bounds_.y, x + w - start, bounds_.h);
				}
			}
			x += w + 20.0f;
		}
		return x - 20.0f;
	};

	// Keyboard keys only where there's likely to be a keyboard, and only if they fit. Then fewer hints.
	bool keyHints = System_GetPropertyInt(SYSPROP_DEVICE_TYPE) == DEVICE_TYPE_DESKTOP;
	bool optional = true;
	if (keyHints && layout(false, true, true) > bounds_.x2()) {
		keyHints = false;
	}
	if (layout(false, keyHints, true) > bounds_.x2()) {
		optional = false;
	}

	dc.Flush();
	dc.Begin();
	const float end = layout(true, keyHints, optional);

	// The version, on the right, if there's room.
	std::string version = PPSSPP_GIT_VERSION;
	if (version.size() > 1 && version[0] == 'v') {
		version = version.substr(1);
	}
	version = "PPSSPP " + version;
	float w, h;
	dc.MeasureText(MonoFont(), 1.0f, 1.0f, version, &w, &h);
	if (bounds_.x2() - w > end + 20.0f) {
		dc.SetFontStyle(MonoFont());
		dc.DrawText(version, bounds_.x2(), cy, Rgb(0x9DBDCB), ALIGN_VCENTER | ALIGN_RIGHT);
	}

	dc.SetFontStyle(dc.GetTheme().uiFont);
	dc.Flush();
}

}  // namespace Xmb
