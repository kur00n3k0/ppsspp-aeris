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

#pragma once

// The views of the XMB main menu layout (the AERIS design). Sizes are in dp, from the mockup.

#include <string>
#include <string_view>
#include <vector>

#include "Common/UI/View.h"
#include "Common/UI/ViewGroup.h"
#include "UI/MiscViews.h"

namespace Xmb {

constexpr float ROW_H = 64.0f;
constexpr float ROW_H_FOCUSED = 96.0f;
constexpr float ROW_PAD_LEFT = 16.0f;
constexpr float ROW_GAP = 18.0f;
// Where the selected category's orb sits, relative to the left edge of the list.
constexpr float ORB_X_IN_LIST = 45.0f;

// The colors of the icon tiles.
enum class Hue {
	Aqua,
	Lime,
	Violet,
	Amber,
	Coral,
};

// Eases towards 1 while focused and back to 0 when not.
class FocusAnim {
public:
	float Update(bool focused);
	float Value() const { return value_; }

private:
	float value_ = 0.0f;
	double lastTime_ = 0.0;
};

float RowHeight(float focus);
// The glass behind the focused row, and the glow under it.
void DrawRowGlass(UIContext &dc, const Bounds &bounds, float focus, bool down);
// Title and subtitle, centered vertically on centerY, clipped to maxW.
void DrawRowText(UIContext &dc, float x, float centerY, float maxW, std::string_view title, std::string_view subtitle, float focus);
// A glossy rounded tile with an atlas icon (or a short text glyph) in the hue's ink color.
void DrawTile(UIContext &dc, const Bounds &tile, Hue hue, ImageID icon, std::string_view glyph, float focus);
// The size of a square tile, and where the text after it starts (relative to the row).
float TileSize(float focus);
float TextX(float focus, bool wide);
// An atlas image fitted in a size x size box centered on (x, y), with the emboss the mockup gives icons.
void DrawEmbossedIcon(UIContext &dc, ImageID icon, float x, float y, float size, uint32_t color, float embossAlpha);

// A row that isn't a game: a tile, a title and a subtitle.
class Item : public UI::Clickable {
public:
	Item(std::string_view title, std::string_view subtitle, ImageID icon, Hue hue, UI::LayoutParams *layoutParams = nullptr);

	void Draw(UIContext &dc) override;
	void GetContentDimensions(const UIContext &dc, float &w, float &h) const override;
	std::string DescribeText() const override { return title_; }

private:
	std::string title_;
	std::string subtitle_;
	ImageID icon_;
	Hue hue_;
	FocusAnim focus_;
};

// A list that searches as you type, like the game browser does.
class List : public UI::LinearLayoutList {
public:
	List(UI::LayoutParams *layoutParams = nullptr);

	bool Key(const KeyInput &input) override;
	void Update() override;
	const std::string &SearchFilter() const { return search_.searchFilter; }
	void ClearSearch();

private:
	ViewSearch search_{};
};

// The row of category orbs. The selected one is large, labelled, and slides to sit over the list.
class CategoryBar : public UI::View {
public:
	struct Category {
		std::string label;
		ImageID icon;
	};

	CategoryBar(std::vector<Category> categories, UI::LayoutParams *layoutParams = nullptr);

	void SetSelected(int index, bool animate);
	int GetSelected() const { return selected_; }
	// The x of the selected orb's center, in screen coordinates.
	void SetAnchorX(float x) { anchorX_ = x; }

	void Draw(UIContext &dc) override;
	bool Touch(const TouchInput &input) override;
	bool CanBeFocused() const override { return false; }
	void GetContentDimensions(const UIContext &dc, float &w, float &h) const override;

	UI::Event OnChoice;

private:
	int HitTest(float x, float y) const;

	std::vector<Category> categories_;
	std::vector<FocusAnim> focus_;
	int selected_ = 0;
	float anchorX_ = 300.0f;
	float offset_ = 0.0f;  // Animated position of the row, in cells.
	double lastTime_ = 0.0;
	int downIndex_ = -1;
};

// Brand, breadcrumb and status chips.
class TopBar : public UI::InertView {
public:
	TopBar(UI::LayoutParams *layoutParams = nullptr) : UI::InertView(layoutParams) {}

	void SetCrumb(std::string_view category, std::string_view location) {
		category_ = category;
		location_ = location;
	}
	void SetSearch(std::string_view query) { search_ = query; }

	void Draw(UIContext &dc) override;
	void GetContentDimensions(const UIContext &dc, float &w, float &h) const override;

private:
	std::string category_;
	std::string location_;
	std::string search_;
};

// The button hints along the bottom. Clicking the quick actions hint opens them.
class HintBar : public UI::View {
public:
	HintBar(UI::LayoutParams *layoutParams = nullptr) : UI::View(layoutParams) {}

	void Draw(UIContext &dc) override;
	bool Touch(const TouchInput &input) override;
	bool CanBeFocused() const override { return false; }
	void GetContentDimensions(const UIContext &dc, float &w, float &h) const override;

	UI::Event OnQuickActions;

private:
	Bounds quickActionBounds_{};
	bool quickActionDown_ = false;
};

}  // namespace Xmb
