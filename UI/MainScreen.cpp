// Copyright (c) 2013- PPSSPP Project.

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
#include <sstream>

#include "ppsspp_config.h"

#include "Common/CPUDetect.h"
#include "Common/System/System.h"
#include "Common/UI/Root.h"
#include "Common/UI/Context.h"
#include "Common/UI/View.h"
#include "Common/UI/ViewGroup.h"

#include "Common/UI/ScreenManager.h"
#include "Common/File/FileUtil.h"
#include "Common/StringUtils.h"
#include "Core/System.h"
#include "Core/Util/RecentFiles.h"
#include "Core/ELF/ParamSFO.h"
#include "Core/Util/GameManager.h"

#include "UI/BackgroundAudio.h"
#include "UI/EmuScreen.h"
#include "UI/MainScreen.h"
#include "UI/GamePacker.h"
#include "UI/GameScreen.h"
#include "UI/GameInfoCache.h"
#include "UI/GameSettingsScreen.h"
#include "UI/IAPScreen.h"
#include "UI/RemoteISOScreen.h"
#include "UI/DisplayLayoutScreen.h"
#include "UI/SavedataScreen.h"
#include "UI/InstallUpdateScreen.h"
#include "UI/InstallZipScreen.h"
#include "UI/InstallPkgScreen.h"
#include "UI/Background.h"
#include "UI/GameBrowser.h"
#include "UI/AdhocServerScreen.h"
#include "UI/Store.h"
#include "UI/XmbViews.h"
#include "Common/Math/curves.h"
#include "Common/System/Display.h"
#include "Common/UI/ScrollView.h"
#include "Common/UI/Tween.h"
#include "Core/Config.h"
#include "Core/Loaders.h"
#include "Common/Data/Text/I18n.h"
#include "Core/Util/DarwinFileSystemServices.h" // For the browser

#include "Core/HLE/sceUmd.h"
#include "Core/Util/PSARUnpack.h"

bool MainScreen::showHomebrewTab = false;
int MainScreen::xmbCategory_ = -1;

static void SaveConfigAndExit() {
	// Let's make sure the config was saved, since it may not have been.
	if (!g_Config.Save("MainScreen::OnExit")) {
		System_Toast("Failed to save settings!\nCheck permissions, or try to restart the device.");
	}
	UpdateUIState(UISTATE_EXIT);
	// Request the framework to exit cleanly.
	System_ExitApp();
}

static void LaunchInEmuScreen(ScreenManager *screenManager, Screen *currentScreen, EmuScreen *emuScreen) {
	if (currentScreen) {
		screenManager->cancelScreensAbove(currentScreen);
	}
	screenManager->switchScreen(emuScreen);
}

static void LaunchFile(ScreenManager *screenManager, Screen *currentScreen, const Path &path) {
	if (GamePacker::IsPacking(path)) {
		// Can't play it while it's read (and maybe replaced), so show its progress instead.
		screenManager->push(new GameScreen(path, false));
		return;
	}

	std::string extension = path.GetFileExtension();
	if (extension == ".zip" || extension == ".7z") {
		// If is a zip file, we have a screen for that.
		screenManager->push(new InstallZipScreen(path));
	} else if (extension == ".pkg") {
		// A game update package - not something to boot, something to install.
		screenManager->push(new InstallPkgScreen(path));
	} else {
		// Check if we already know that this game isn't playable.
		// If coming from the main screen, the info will already be computed here since the icon is displayed etc.
		// Otherwise (launching from a file association, a shortcut, drag-and-drop...) we have to block until
		// it's available - we can't decide what to do below without it. HIGH, so we don't wait behind
		// whatever else is queued, like a search's loads for every game in a big list.
		const GameInfoFlags neededFlags = GameInfoFlags::FILE_TYPE | GameInfoFlags::PARAM_SFO;
		std::shared_ptr<GameInfo> info = g_gameInfoCache->GetInfo(nullptr, path, neededFlags, nullptr, GameInfoFlags::EMPTY, TaskPriority::HIGH);
		info->WaitUntilReady(neededFlags);

		switch (info->fileType) {
		case IdentifiedFileType::PSP_UMD_VIDEO_ISO:
		{
			// The XMB plays it, if there's one to boot. Otherwise we show info about it.
			const Path vshPath = GetUmdVideoVSHPath();
			if (vshPath.empty()) {
				screenManager->push(new GameScreen(path, false));
			} else {
				LaunchInEmuScreen(screenManager, currentScreen, new EmuScreen(vshPath, path, true));
			}
			return;
		}
		case IdentifiedFileType::PSP_ISO:
		{
			// A disc with both a game and video on it. The XMB would start the game by itself, so for
			// the video, it comes up without auto-start and the disc waits in the Video column.
			const Path vshPath = GetUmdVideoVSHPath();
			if (vshPath.empty()) {
				break;
			}
			std::unique_ptr<FileLoader> fileLoader(ConstructFileLoader(path));
			if (!fileLoader || !(DetectUmdDiscType(fileLoader.get()) & PSP_UMD_TYPE_VIDEO)) {
				break;
			}
			if (!UmdHasBootableGame(fileLoader.get())) {
				// The game is only a placeholder, so there's just the video.
				LaunchInEmuScreen(screenManager, currentScreen, new EmuScreen(vshPath, path, false));
				return;
			}
			auto ga = GetI18NCategory(I18NCat::GAME);
			const std::vector<std::string> choices = { std::string(ga->T("Play game")), std::string(ga->T("Watch video")) };
			UI::ListPopupScreen *popup = new UI::ListPopupScreen(ga->T("This disc has both a game and video"), choices, -1, [=](int choice) {
				if (choice == 0) {
					LaunchInEmuScreen(screenManager, currentScreen, new EmuScreen(path));
				} else if (choice == 1) {
					LaunchInEmuScreen(screenManager, currentScreen, new EmuScreen(vshPath, path, false));
				}
			});
			popup->SetChoiceIcons({ { 0, ImageID("I_PLAY") }, { 1, ImageID("I_UMD_VIDEO_ISO") } });
			screenManager->push(popup);
			return;
		}
		case IdentifiedFileType::PSP_SAVEDATA_DIRECTORY:
		{
			// Show the savedata popup, why not?
			std::string title = SanitizeString(info->GetTitle(), StringRestriction::NoLineBreaksOrSpecials, 0, 200);
			screenManager->push(new SavedataPopupScreen(Path(), path, title));
			return;
		}
		case IdentifiedFileType::PSP_PBP:
		case IdentifiedFileType::PSP_PBP_DIRECTORY:
		{
			// Check if it's an update file. If so, we'll offer to install it directly,
			// instead of running it (which currently will not work).
			if (info->id == "MSTKUPDATE") {
				std::string title = info->GetTitle();  // includes the version.
				// The unpacker wants the PBP itself, not the folder it happens to sit in.
				const Path pbpPath = info->fileType == IdentifiedFileType::PSP_PBP ? path : path / "EBOOT.PBP";
				screenManager->push(new InstallUpdateScreen(pbpPath, title, true));
				return;
			}
			break;
		}
		default:
			break;
		}
		// Otherwise let the EmuScreen take care of it, including error handling.
		LaunchInEmuScreen(screenManager, currentScreen, new EmuScreen(path));
	}
}

static bool IsTempPath(const Path &str) {
	std::string item = str.ToString();

#ifdef _WIN32
	// Normalize slashes.
	item = ReplaceAll(item, "/", "\\");
#endif

	std::vector<std::string> tempPaths = System_GetPropertyStringVec(SYSPROP_TEMP_DIRS);
	for (auto temp : tempPaths) {
#ifdef _WIN32
		temp = ReplaceAll(temp, "/", "\\");
		if (!temp.empty() && temp[temp.size() - 1] != '\\')
			temp += "\\";
#else
		if (!temp.empty() && temp[temp.size() - 1] != '/')
			temp += "/";
#endif
		if (startsWith(item, temp))
			return true;
	}

	return false;
}

MainScreen::MainScreen() {
	g_BackgroundAudio.SetGame(Path());
	packGeneration_ = GamePacker::Generation();
}

MainScreen::~MainScreen() {
	g_BackgroundAudio.SetGame(Path());
}

bool MainScreen::WantsTextInput() const {
	// We don't want to pop a software keyboard on the main screen, just for type-to-search.
	return !System_GetPropertyBool(SYSPROP_KEYBOARD_IS_SOFT);
}


#if PPSSPP_PLATFORM(IOS)
constexpr std::string_view getGamesUri = "https://www.ppsspp.org/getgames_ios";
constexpr std::string_view getHomebrewUri = "https://www.ppsspp.org/gethomebrew_ios";
#else
constexpr std::string_view getGamesUri = "https://www.ppsspp.org/getgames";
constexpr std::string_view getHomebrewUri = "https://www.ppsspp.org/gethomebrew";
#endif
constexpr std::string_view remoteGamesUri = "https://www.ppsspp.org/docs/reference/disc-streaming";

void MainScreen::CreateRecentTab() {
	using namespace UI;
	auto mm = GetI18NCategory(I18NCat::MAINMENU);

	LinearLayout *tabContainer = new LinearLayout(ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT));
	tabContainer->SetSpacing(0.0f);
	SearchBar *search = tabContainer->Add(new SearchBar(new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, Margins(8, 8, 8, 0))));

	ScrollView *scrollView = tabContainer->Add(new ScrollView(ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, 1.0f)));
	scrollView->SetTag("MainScreenRecentGames");

	bool portrait = GetDeviceOrientation() == DeviceOrientation::Portrait;

	GameBrowser *tabRecentGames = new GameBrowser(GetRequesterToken(),
		Path("!RECENT"), BrowseFlags::NONE, portrait, &g_Config.iGameBrowserView1, screenManager(), "", "",
		new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT));
	tabRecentGames->SetSearchBar(search);

	scrollView->Add(tabRecentGames);
	gameBrowsers_.push_back(tabRecentGames);

	tabHolder_->AddTab(mm->T("Recent"), ImageID::invalid(), tabContainer);
	tabRecentGames->OnChoice.Handle(this, &MainScreen::OnGameSelectedInstant);
	tabRecentGames->OnHoldChoice.Handle(this, &MainScreen::OnGameSelected);
	tabRecentGames->OnHighlight.Handle(this, &MainScreen::OnGameHighlight);
}

GameBrowser *MainScreen::CreateBrowserTab(const Path &path, std::string_view title, std::string_view howToTitle, std::string_view howToUri, BrowseFlags browseFlags, int *viewMode, float *scrollPos) {
	using namespace UI;
	auto mm = GetI18NCategory(I18NCat::MAINMENU);

	const bool portrait = GetDeviceOrientation() == DeviceOrientation::Portrait;

	LinearLayout *tabContainer = new LinearLayout(ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT));
	tabContainer->SetSpacing(0.0f);
	SearchBar *search = tabContainer->Add(new SearchBar(new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT)));

	ScrollView *scrollView = tabContainer->Add(new ScrollView(ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, 1.0f)));
	scrollView->SetTag(title);  // Re-use title as tag, should be fine.

	GameBrowser *gameBrowser = new GameBrowser(GetRequesterToken(), path, browseFlags, portrait, viewMode, screenManager(),
		mm->T(howToTitle), howToUri,
		new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT));

	gameBrowser->SetSearchBar(search);

	scrollView->Add(gameBrowser);
	gameBrowsers_.push_back(gameBrowser);

	tabHolder_->AddTab(mm->T(title), ImageID::invalid(), tabContainer);
	if (scrollPos) {
		scrollView->RememberPosition(scrollPos);
	}

	gameBrowser->OnChoice.Handle(this, &MainScreen::OnGameSelectedInstant);
	gameBrowser->OnHoldChoice.Handle(this, &MainScreen::OnGameSelected);
	gameBrowser->OnHighlight.Handle(this, &MainScreen::OnGameHighlight);

	return gameBrowser;
}

class LogoView : public UI::AnchorLayout {
public:
	LogoView(bool portrait, UI::LayoutParams *layoutParams) : UI::AnchorLayout(layoutParams), portrait_(portrait) {}
	void Draw(UIContext &dc) override {
		using namespace UI;
		UI::AnchorLayout::Draw(dc);

		const AtlasImage *iconImg = dc.Draw()->GetAtlas()->getImage(GetIconID());
		const AtlasImage *logoImg = dc.Draw()->GetAtlas()->getImage(ImageID("I_LOGO"));
		if (!iconImg) {
			return;
		}

		dc.Draw()->DrawImage(GetIconID(), bounds_.x, bounds_.y, 1.0f);

		if (bounds_.w < iconImg->w + logoImg->w + 36) {
			return;
		}

		dc.Draw()->DrawImage(ImageID("I_LOGO"), bounds_.x + iconImg->w + 8, bounds_.y + 4, 1.0f);

		std::string versionString = PPSSPP_GIT_VERSION;
		// Strip the 'v' from the displayed version, and shorten the commit hash.
		if (versionString.size() > 2) {
			if (versionString[0] == 'v' && isdigit(versionString[1])) {
				versionString = versionString.substr(1);
			}
			if (CountChar(versionString, '-') == 2) {
				// Shorten the commit hash.
				size_t cutPos = versionString.find_last_of('-') + 8;
				versionString = versionString.substr(0, std::min(cutPos, versionString.size()));
			}
		}
		dc.Flush();

		const bool tiny = versionString.size() > 10;

		const FontStyle *style = GetTextStyle(dc, tiny ? TextSize::Tiny : TextSize::Small);
		dc.SetFontStyle(*style);
		dc.DrawText(versionString,
			bounds_.x + iconImg->w + 8,
			bounds_.y + logoImg->h + (tiny ? 8 : 6),
			dc.GetTheme().infoStyle.fgColor);
		dc.SetFontStyle(dc.GetTheme().uiFont);
	}

	void GetContentDimensions(const UIContext &dc, float &w, float &h) const override {
		const AtlasImage *iconImg = dc.Draw()->GetAtlas()->getImage(GetIconID());
		w = iconImg->w;
		h = iconImg->h;
	}

	bool Touch(const TouchInput &touch) override {
		bool retval = UI::AnchorLayout::Touch(touch);
		if (!portrait_ && (touch.flags & TouchInputFlags::DOWN) && bounds_.Contains(touch.x, touch.y) && touch.y >= bounds_.y2() - 20) {
			auto di = GetI18NCategory(I18NCat::DIALOG);
			System_CopyStringToClipboard(PPSSPP_GIT_VERSION);
			g_OSD.Show(OSDType::MESSAGE_INFO, ApplySafeSubstitutions(di->T("Copied to clipboard: %1"), PPSSPP_GIT_VERSION), 0.0f, "copyToClip");
			return true;
		}
		return retval;
	}

private:
	ImageID GetIconID() const {
		return System_GetPropertyBool(SYSPROP_APP_GOLD) ? ImageID("I_ICON_GOLD") : ImageID("I_ICON");
	}

	const bool portrait_;
};

void MainScreen::CreateMainButtons(UI::LinearLayout *parent, bool portrait) {
	using namespace UI;
	auto mm = GetI18NCategory(I18NCat::MAINMENU);
	auto di = GetI18NCategory(I18NCat::DIALOG);
	const bool showGold = !System_GetPropertyBool(SYSPROP_APP_GOLD) && (System_GetPropertyInt(SYSPROP_DEVICE_TYPE) != DEVICE_TYPE_VR);

	if (portrait) {
		// Icon buttons only, and no exit button - there's no space.
		parent->Add(new Spacer(1.0f, new LinearLayoutParams(1.0f)));
		if (System_GetPropertyBool(SYSPROP_HAS_FILE_BROWSER)) {
			parent->Add(new Choice(ImageID("I_FOLDER_OPEN"), new LinearLayoutParams()))->OnClick.Handle(this, &MainScreen::OnLoadFile);
		}
		parent->Add(new Choice(ImageID("I_GEAR"), new LinearLayoutParams()))->OnClick.Handle(this, &MainScreen::OnGameSettings);
		parent->Add(new Choice(ImageID("I_INFO"), new LinearLayoutParams()))->OnClick.Handle(this, &MainScreen::OnCredits);
		if (showGold) {
			Choice *gold = parent->Add(new Choice(ImageID("I_ICON_GOLD"), new LinearLayoutParams()));
			gold->OnClick.Add([this](UI::EventParams &) {
				LaunchBuyGold(this->screenManager());
			});
			gold->SetIconRight(ImageID("I_ICON_GOLD"), 0.5f);
			gold->SetImageScale(0.6f);  // for the left icon.
			gold->SetShine(true);
		}
		parent->ApplyRoundCorners();
		return;
	} else {
		ChoiceGroupList *buttons = parent->Add(new ChoiceGroupList());
		buttons->SetPadding(Margins(0));
		buttons->SetSpacing(12.0f);

		ChoiceGroup *mainButtons = buttons->Add(new ChoiceGroup());

		if (System_GetPropertyBool(SYSPROP_HAS_FILE_BROWSER)) {
			mainButtons->Add(new Choice(mm->T("Load", "Load...")))->OnClick.Handle(this, &MainScreen::OnLoadFile);
		}
		mainButtons->Add(new Choice(mm->T("Game Settings", "Settings")))->OnClick.Handle(this, &MainScreen::OnGameSettings);
		mainButtons->Add(new Choice(mm->T("About PPSSPP")))->OnClick.Handle(this, &MainScreen::OnCredits);
		mainButtons->Add(new Choice(mm->T("www.ppsspp.org")))->OnClick.Handle(this, &MainScreen::OnPPSSPPOrg);

		if (showGold) {
			Choice *gold = mainButtons->Add(new Choice(mm->T("Buy PPSSPP Gold")));
			gold->OnClick.Add([this](UI::EventParams &) {
				LaunchBuyGold(this->screenManager());
			});
			gold->SetIconRight(ImageID("I_ICON_GOLD"), 0.5f);
			gold->SetImageScale(0.6f);
			gold->SetShine(true);
		}

		// Exit button.

		// Officially, iOS apps should not have exit buttons. Remove it to maximize app store review chances.
		// (The exit button previously created problems on Android, but now we allow it in landscape mode.)
#if !PPSSPP_PLATFORM(IOS_APP_STORE)
		Choice *exitButton = buttons->Add(new Choice(di->T("Exit")));
		exitButton->OnClick.Add([](UI::EventParams &e) {
			SaveConfigAndExit();
		});
		exitButton->SetRoundCorners(RoundCorners::All);
#endif
	}
}

void MainScreen::CreateViews() {
	builtLayout_ = g_Config.iMainMenuLayout;
	if (UseXmbLayout()) {
		CreateXmbViews();
	} else {
		CreateClassicViews();
	}
	CreateNoticeBars();
}

void MainScreen::CreateClassicViews() {
	xmbLayout_ = false;
	xmbPanes_.clear();
	xmbCategoryBar_ = nullptr;
	xmbTopBar_ = nullptr;
	xmbPaneHolder_ = nullptr;

	// Information in the top left.
	// Back button to the bottom left.
	// Scrolling action menu to the right.
	using namespace UI;

	const bool vertical = GetDeviceOrientation() == DeviceOrientation::Portrait;

	auto mm = GetI18NCategory(I18NCat::MAINMENU);

	// Thie is the true root, leaves room for persistent notifications.
	root_ = new LinearLayout(ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, FILL_PARENT));

	tabHolder_ = new TabHolder(ORIENT_HORIZONTAL, 64, TabHolderFlags::Default, nullptr, nullptr, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, 1.0f));
	ViewGroup *leftColumn = tabHolder_;
	tabHolder_->SetTag("MainScreenGames");
	gameBrowsers_.clear();

	tabHolder_->SetClip(true);

	bool showRecent = g_Config.iMaxRecent > 0;
	bool hasStorageAccess = !System_GetPropertyBool(SYSPROP_SUPPORTS_PERMISSIONS) ||
		System_GetPermissionStatus(SYSTEM_PERMISSION_STORAGE) == PERMISSION_STATUS_GRANTED;
	bool storageIsTemporary = IsTempPath(GetSysDirectory(DIRECTORY_SAVEDATA)) && !confirmedTemporary_;
	if (showRecent && !hasStorageAccess) {
		showRecent = g_recentFiles.HasAny();
	}

	if (showRecent) {
		CreateRecentTab();
	}

	Button *focusButton = nullptr;
	if (hasStorageAccess) {
		CreateBrowserTab(Path(g_Config.currentDirectory), "Games", "How to get games", getGamesUri, BrowseFlags::STANDARD, &g_Config.iGameBrowserView2, &g_Config.fGameListScrollPosition);
		CreateBrowserTab(GetSysDirectory(DIRECTORY_GAME), "Homebrew & Demos", "How to get homebrew & demos", getHomebrewUri, BrowseFlags::HOMEBREW_STORE, &g_Config.iGameBrowserView3, &g_Config.fHomebrewScrollPosition);

		if (g_Config.bRemoteTab && !g_Config.sLastRemoteISOServer.empty()) {
			Path remotePath(FormatRemoteISOUrl(g_Config.sLastRemoteISOServer.c_str(), g_Config.iLastRemoteISOPort, RemoteSubdir().c_str()));
			GameBrowser *remoteBrowser = CreateBrowserTab(remotePath, "Remote disc streaming", "Remote disc streaming", remoteGamesUri, BrowseFlags::NAVIGATE, &g_Config.iGameBrowserView4, &g_Config.fRemoteScrollPosition);
			remoteBrowser->SetHomePath(remotePath);
		}

		if (g_recentFiles.HasAny()) {
			tabHolder_->SetCurrentTab(std::clamp(g_Config.iDefaultTab, 0, g_Config.bRemoteTab ? 3 : 2), true);
		} else if (g_Config.iMaxRecent > 0) {
			tabHolder_->SetCurrentTab(1, true);	
		}

		if (backFromStore_ || showHomebrewTab) {
			tabHolder_->SetCurrentTab(2, true);
			backFromStore_ = false;
			showHomebrewTab = false;
		}

		if (storageIsTemporary) {
			LinearLayout *buttonHolder = new LinearLayout(ORIENT_HORIZONTAL, new LinearLayoutParams(WRAP_CONTENT, WRAP_CONTENT));
			buttonHolder->Add(new Spacer(new LinearLayoutParams(1.0f)));
			focusButton = new Button(mm->T("SavesAreTemporaryIgnore", "Ignore warning"), new LinearLayoutParams(WRAP_CONTENT, WRAP_CONTENT));
			focusButton->SetPadding(32, 16);
			buttonHolder->Add(focusButton)->OnClick.Add([this](UI::EventParams &e) {
				confirmedTemporary_ = true;
				RecreateViews();
			});
			buttonHolder->Add(new Spacer(new LinearLayoutParams(1.0f)));

			leftColumn->Add(new Spacer(new LinearLayoutParams(0.1f)));
			leftColumn->Add(new TextView(mm->T("SavesAreTemporary", "PPSSPP saving in temporary storage"), ALIGN_HCENTER, false));
			leftColumn->Add(new TextView(mm->T("SavesAreTemporaryGuidance", "Extract PPSSPP somewhere to save permanently"), ALIGN_HCENTER, false));
			leftColumn->Add(new Spacer(10.0f));
			leftColumn->Add(buttonHolder);
			leftColumn->Add(new Spacer(new LinearLayoutParams(0.1f)));
		}
	} else {
		if (!showRecent) {
			leftColumn = new LinearLayout(ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, 1.0f));
			// Just so it's destroyed on recreate.
			leftColumn->Add(tabHolder_);
			tabHolder_->SetVisibility(V_GONE);
		}

		LinearLayout *buttonHolder = new LinearLayout(ORIENT_HORIZONTAL, new LinearLayoutParams(WRAP_CONTENT, WRAP_CONTENT));
		buttonHolder->Add(new Spacer(new LinearLayoutParams(1.0f)));
		focusButton = new Button(mm->T("Give PPSSPP permission to access storage"), new LinearLayoutParams(WRAP_CONTENT, WRAP_CONTENT));
		focusButton->SetPadding(32, 16);
		buttonHolder->Add(focusButton)->OnClick.Handle(this, &MainScreen::OnAllowStorage);
		buttonHolder->Add(new Spacer(new LinearLayoutParams(1.0f)));

		leftColumn->Add(new Spacer(new LinearLayoutParams(0.1f)));
		leftColumn->Add(buttonHolder);
		leftColumn->Add(new Spacer(10.0f));
		leftColumn->Add(new TextView(mm->T("PPSSPP can't load games or save right now"), ALIGN_HCENTER, false));
		leftColumn->Add(new Spacer(new LinearLayoutParams(0.1f)));
	}

	if (vertical) {
		LinearLayout *header = new LinearLayout(ORIENT_HORIZONTAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, Margins(8, 8, 8, 16)));
		header->SetSpacing(5.0f);
		header->Add(new LogoView(true, new LinearLayoutParams(1.0f)));

		LinearLayout *buttonGroup = new LinearLayout(ORIENT_HORIZONTAL, new LinearLayoutParams(WRAP_CONTENT, WRAP_CONTENT, 1.0f, UI::Gravity::G_VCENTER));

		CreateMainButtons(buttonGroup, vertical);
		header->Add(buttonGroup);

		LinearLayout *rootLayout = new LinearLayout(ORIENT_VERTICAL, new LinearLayoutParams(1.0f));
		rootLayout->SetSpacing(0.0f);

		leftColumn->ReplaceLayoutParams(new LinearLayoutParams(1.0f));
		rootLayout->Add(header);
		rootLayout->Add(leftColumn);
		root_->Add(rootLayout);

		// no space for a fullscreen button!
	} else {
		const Margins actionMenuMargins(0, 10, 10, 0);
		ViewGroup *rightColumn = new ScrollView(ORIENT_VERTICAL, new LinearLayoutParams(320, FILL_PARENT, actionMenuMargins));
		LinearLayout *rightColumnItems = new LinearLayout(ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT));
		rightColumnItems->SetSpacing(0.0f);
		ViewGroup *logo = new LogoView(false, new LinearLayoutParams(FILL_PARENT, 80.0f));

		if (System_GetPropertyInt(SYSPROP_DEVICE_TYPE) == DEVICE_TYPE_DESKTOP) {
			auto gr = GetI18NCategory(I18NCat::GRAPHICS);
			Button *fullscreenButton = logo->Add(new Button("", ImageID(), new AnchorLayoutParams(48, 48, NONE, 0, 0, NONE, Centering::None)));
			fullscreenButton->SetIgnoreText(true);
			fullscreenButton->OnClick.Add([](UI::EventParams &e) {
				g_Config.bFullScreen = !g_Config.bFullScreen;
				System_ApplyFullscreenState();
			});
			fullscreenButton->SetImageIDFunc([]() {
				return g_Config.bFullScreen ? ImageID("I_RESTORE") : ImageID("I_FULLSCREEN");
			});
		}
		rightColumnItems->Add(logo);

		LinearLayout *rightColumnChoices = rightColumnItems;
		CreateMainButtons(rightColumnChoices, vertical);

		rightColumn->Add(rightColumnItems);

		LinearLayout *columns = new LinearLayout(ORIENT_HORIZONTAL, new LinearLayoutParams(1.0f));
		columns->Add(leftColumn);
		columns->Add(rightColumn);
		root_->Add(columns);
	}

	if (focusButton) {
		root_->SetDefaultFocusView(focusButton);
	} else if (tabHolder_->GetVisibility() != V_GONE) {
		root_->SetDefaultFocusView(tabHolder_);
	}
}

void MainScreen::CreateNoticeBars() {
	using namespace UI;

	root_->SetTag("mainroot");

	const UI::Drawable dismissableBackground = screenManager()->getUIContext()->GetTheme().itemDownStyle.background;

	auto CreateDismissableBar = [this, dismissableBackground](std::string_view message, std::string_view action, std::function<void()> onDismiss) {
		Margins margins(0, 0);
		UI::LinearLayout *bar = new LinearLayout(ORIENT_HORIZONTAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, margins));

		UI::Margins textMargins(10, 5);
		UI::Margins buttonMargins(5, 0);
		UI::Drawable solid(dismissableBackground);
		bar->SetSpacing(5.0f);
		bar->SetBG(solid);

		bar->Add(new TextView(message, FLAG_WRAP_TEXT, false, new LinearLayoutParams(1.0f, UI::Gravity::G_VCENTER, textMargins)));
		if (!action.empty()) {
			bar->Add(new Choice(action, new LinearLayoutParams(0.0f, UI::Gravity::G_VCENTER, buttonMargins)))->OnClick.Handle(this, &MainScreen::OnDownloadUpgrade);
		}

		Choice *dismiss = bar->Add(new Choice("", ImageID("I_CROSS"), new LinearLayoutParams(0.0f, UI::Gravity::G_VCENTER, buttonMargins)));
		dismiss->OnClick.Add([this, onDismiss](UI::EventParams &e) {
			onDismiss();
			RecreateViews();
		});
		return bar;
	};

	if (!g_Config.sUpgradeMessage.empty()) {
		auto di = GetI18NCategory(I18NCat::DIALOG);
		std::string upgradeMessage(di->T("New version of PPSSPP available"));
		// The version only really fits in the horizontal layout.
		upgradeMessage += ": " + g_Config.sUpgradeVersion;
		UI::LinearLayout *upgradeBar = CreateDismissableBar(upgradeMessage, di->T("Download"), []() {
			g_Config.DismissUpgrade();
			g_Config.Save("dismissupgrade");
		});

		// Slip in at the top.
		root_->Insert(0, upgradeBar);
	}

#if PPSSPP_PLATFORM(WINDOWS) && PPSSPP_ARCH(X86)
	if (cpu_info.OS64bit && !g_Config.bWow64WarningDismissed) {
		auto di = GetI18NCategory(I18NCat::DIALOG);
		std::string_view message = di->T("You're running the 32-bit version. Use PPSSPPWindows64.exe instead for best performance.");
		UI::LinearLayout *upgradeBar = CreateDismissableBar(message, "", [this]() {
			g_Config.bWow64WarningDismissed = true;
			g_Config.Save("dismisswow64");
		});
		root_->Insert(0, upgradeBar);
	}
#endif
}

bool MainScreen::UseXmbLayout() const {
	if (g_Config.iMainMenuLayout != (int)MainMenuLayout::XMB) {
		return false;
	}
	// The warnings about storage are only in the classic layout.
	const bool hasStorageAccess = !System_GetPropertyBool(SYSPROP_SUPPORTS_PERMISSIONS) ||
		System_GetPermissionStatus(SYSTEM_PERMISSION_STORAGE) == PERMISSION_STATUS_GRANTED;
	const bool storageIsTemporary = IsTempPath(GetSysDirectory(DIRECTORY_SAVEDATA)) && !confirmedTemporary_;
	return hasStorageAccess && !storageIsTemporary;
}

void MainScreen::CreateXmbViews() {
	using namespace UI;
	auto mm = GetI18NCategory(I18NCat::MAINMENU);
	auto ms = GetI18NCategory(I18NCat::MAINSETTINGS);
	auto di = GetI18NCategory(I18NCat::DIALOG);
	auto sa = GetI18NCategory(I18NCat::SAVEDATA);
	auto nw = GetI18NCategory(I18NCat::NETWORKING);

	xmbLayout_ = true;
	xmbPanes_.clear();
	// addPane hands out references into this.
	xmbPanes_.reserve(7);
	gameBrowsers_.clear();
	tabHolder_ = nullptr;
	xmbScrollTarget_ = -1.0f;

	const bool portrait = GetDeviceOrientation() == DeviceOrientation::Portrait;
	const float w = g_display.dp_xres;
	const float h = g_display.dp_yres;

	// From the mockup at 1440x816: the categories 18% from the top, the list 18% from the left and
	// about 40% wide, the hints along the bottom.
	const float side = portrait ? 16.0f : 40.0f;
	const float topBarY = portrait ? 16.0f : 28.0f;
	const float categoryY = std::max(topBarY + 48.0f, h * 0.147f);
	const float listX = portrait ? 16.0f : w * 0.178f;
	const float listY = categoryY + 176.0f;
	const float listW = portrait ? w - 32.0f : std::min(std::max(w * 0.42f, 420.0f), w - listX - side);
	const float listBottom = 22.0f + 44.0f + 10.0f;
	// The pane holder reaches 16 dp past the list on each side, for the icons that hang off the rows.
	constexpr float holderPad = 16.0f;

	root_ = new LinearLayout(ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, FILL_PARENT));
	AnchorLayout *xmb = root_->Add(new AnchorLayout(new LinearLayoutParams(FILL_PARENT, FILL_PARENT, 1.0f)));

	xmbTopBar_ = xmb->Add(new Xmb::TopBar(new AnchorLayoutParams(FILL_PARENT, 40.0f, side, topBarY, side, NONE)));

	// Clipped at the top of the list, which hides the rows above the selected one, as the XMB does.
	xmbPaneHolder_ = xmb->Add(new AnchorLayout(new AnchorLayoutParams(FILL_PARENT, FILL_PARENT, listX - holderPad, listY, std::max(0.0f, w - listX - listW - holderPad), listBottom)));
	xmbPaneHolder_->SetClip(true);

	Xmb::HintBar *hints = xmb->Add(new Xmb::HintBar(new AnchorLayoutParams(FILL_PARENT, 44.0f, side, NONE, side, 22.0f)));
	hints->OnQuickActions.Add([this](EventParams &) {
		ShowQuickActions();
	});

	std::vector<Xmb::CategoryBar::Category> categories;
	auto addPane = [&](XmbCategory category, std::string_view label, ImageID icon, std::string_view crumb) -> XmbPane & {
		XmbPane pane;
		pane.category = category;
		pane.label = label;
		pane.crumb = crumb;
		// No edges set, so the slide-in tween moves it rather than squeezing it.
		pane.scroll = xmbPaneHolder_->Add(new ScrollView(ORIENT_VERTICAL, new AnchorLayoutParams(FILL_PARENT, FILL_PARENT)));
		pane.scroll->SetTag(StringFromFormat("XmbPane%d", (int)category));
		pane.scroll->SetShadows(false);
		pane.scroll->SetScrollbar(false);
		LinearLayout *content = pane.scroll->Add(new LinearLayout(ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, Margins(holderPad, 0.0f, holderPad, 0.0f))));
		content->SetSpacing(0.0f);
		pane.content = content;
		categories.push_back({ std::string(label), icon });
		xmbPanes_.push_back(pane);
		return xmbPanes_.back();
	};
	auto addItem = [](ViewGroup *parent, std::string_view title, std::string_view subtitle, const char *icon, Xmb::Hue hue, std::function<void()> onClick) {
		Xmb::Item *item = parent->Add(new Xmb::Item(title, subtitle, ImageID(icon), hue, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT)));
		item->OnClick.Add([onClick](EventParams &) {
			onClick();
		});
	};
	auto addBrowser = [&](XmbPane &pane, const Path &path, BrowseFlags flags, int *viewMode, float *scrollPos) {
		GameBrowser *browser = new GameBrowser(GetRequesterToken(), path, flags | BrowseFlags::XMB, portrait, viewMode, screenManager(), "", "",
			new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT));
		browser->OnChoice.Handle(this, &MainScreen::OnGameSelectedInstant);
		browser->OnHoldChoice.Handle(this, &MainScreen::OnGameSelected);
		browser->OnHighlight.Handle(this, &MainScreen::OnGameHighlight);
		if (scrollPos) {
			pane.scroll->RememberPosition(scrollPos);
		}
		pane.browser = browser;
		gameBrowsers_.push_back(browser);
		return browser;
	};

	// Settings: the pages of the settings screen.
	{
		XmbPane &pane = addPane(XmbCategory::SETTINGS, ms->T("Settings"), ImageID("I_SLIDERS"), mm->T("GameSettingsCrumb", "Game Settings"));
		pane.list = pane.content->Add(new Xmb::List(new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT)));
		struct Page {
			const char *title;
			const char *summaryKey;
			const char *summary;
			const char *icon;
			Xmb::Hue hue;
		};
		// In the order of GameSettingsScreen's tabs.
		static const Page pages[] = {
			{ "Graphics", "GraphicsSummary", "Backend \xC2\xB7 resolution \xC2\xB7 frame rate", "I_DISPLAY", Xmb::Hue::Aqua },
			{ "Controls", "ControlsSummary", "Mapping \xC2\xB7 touch layout \xC2\xB7 tilt", "I_CONTROLLER", Xmb::Hue::Violet },
			{ "Audio", "AudioSummary", "Volume \xC2\xB7 backend \xC2\xB7 sound effects", "I_SPEAKER_MAX", Xmb::Hue::Amber },
			{ "Networking", "NetworkingSummary", "Ad hoc \xC2\xB7 infrastructure \xC2\xB7 ports", "I_WIFI", Xmb::Hue::Lime },
			{ "Tools", "ToolsSummary", "Save data \xC2\xB7 cheats \xC2\xB7 developer", "I_TOOLS", Xmb::Hue::Coral },
			{ "System", "SystemSummary", "Language \xC2\xB7 UI \xC2\xB7 storage", "I_PSP", Xmb::Hue::Aqua },
		};
		for (int i = 0; i < (int)ARRAY_SIZE(pages); i++) {
			addItem(pane.list, ms->T(pages[i].title), mm->T(pages[i].summaryKey, pages[i].summary), pages[i].icon, pages[i].hue, [this, i]() {
				g_Config.iSettingsCurrentTab = i;
				screenManager()->push(new GameSettingsScreen(Path()));
			});
		}
	}

	if (g_Config.iMaxRecent > 0) {
		XmbPane &pane = addPane(XmbCategory::RECENT, mm->T("Recent"), ImageID("I_XMB_RECENT"), mm->T("Recently played"));
		GameBrowser *browser = addBrowser(pane, Path("!RECENT"), BrowseFlags::NONE, &g_Config.iGameBrowserView1, nullptr);
		pane.content->Add(browser);
		if (!g_recentFiles.HasAny()) {
			pane.content->Add(new TextView(mm->T("No recently played games"), ALIGN_VCENTER, true, new LinearLayoutParams(FILL_PARENT, Xmb::ROW_H, Margins(Xmb::ROW_PAD_LEFT, 0, 0, 0))))->SetTextColor(0xFFCBBD9D);
		}
	}

	{
		XmbPane &pane = addPane(XmbCategory::GAMES, mm->T("Games"), ImageID("I_CONTROLLER"), mm->T("Games"));
		GameBrowser *browser = addBrowser(pane, Path(g_Config.currentDirectory), BrowseFlags::STANDARD, &g_Config.iGameBrowserView2, &g_Config.fGameListScrollPosition);
		// The browser does the searching, so these items stay put.
		if (System_GetPropertyBool(SYSPROP_HAS_FILE_BROWSER)) {
			addItem(pane.content, mm->T("Load", "Load..."), mm->T("LoadSummary", "Browse for a game file"), "I_FOLDER_OPEN", Xmb::Hue::Amber, [this]() {
				UI::EventParams e{};
				OnLoadFile(e);
			});
		}
		if (browser->CanBrowseForFolder()) {
			addItem(pane.content, mm->T("Browse", "Browse..."), mm->T("BrowseSummary", "Choose the folder of your games"), "I_FOLDER", Xmb::Hue::Amber, [browser]() {
				browser->BrowseForFolder();
			});
		}
		pane.content->Add(browser);
		addItem(pane.content, mm->T("How to get games"), "ppsspp.org", "I_LINK_OUT", Xmb::Hue::Aqua, []() {
			System_LaunchUrl(LaunchUrlType::BROWSER_URL, getGamesUri);
		});
	}

	{
		XmbPane &pane = addPane(XmbCategory::HOMEBREW, mm->T("Homebrew & Demos"), ImageID("I_HOMEBREW_STORE"), "PSP/GAME");
		GameBrowser *browser = addBrowser(pane, GetSysDirectory(DIRECTORY_GAME), BrowseFlags::HOMEBREW_STORE, &g_Config.iGameBrowserView3, &g_Config.fHomebrewScrollPosition);
		pane.content->Add(browser);
		addItem(pane.content, mm->T("Homebrew store"), mm->T("HomebrewStoreSummary", "Free homebrew, installed in place"), "I_HOMEBREW_STORE", Xmb::Hue::Violet, [this]() {
			screenManager()->push(new StoreScreen());
		});
		addItem(pane.content, mm->T("How to get homebrew & demos"), "ppsspp.org", "I_LINK_OUT", Xmb::Hue::Aqua, []() {
			System_LaunchUrl(LaunchUrlType::BROWSER_URL, getHomebrewUri);
		});
	}

	{
		XmbPane &pane = addPane(XmbCategory::SAVED_DATA, sa->T("Save data"), ImageID("I_FILE_SAVE"), "PSP/SAVEDATA");
		pane.list = pane.content->Add(new Xmb::List(new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT)));
		addItem(pane.list, sa->T("Save data"), mm->T("SaveDataSummary", "Browse, back up and delete saves"), "I_FILE_SAVE", Xmb::Hue::Aqua, [this]() {
			screenManager()->push(new SavedataScreen(Path()));
		});
	}

	{
		XmbPane &pane = addPane(XmbCategory::NETWORK, mm->T("Network"), ImageID("I_WEB_BROWSER"), mm->T("Online"));
		pane.list = pane.content->Add(new Xmb::List(new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT)));
		addItem(pane.list, nw->T("AdHoc server"), mm->T("AdhocServerSummary", "Choose or host a server"), "I_NETWORK", Xmb::Hue::Aqua, [this]() {
			auto nw = GetI18NCategory(I18NCat::NETWORKING);
			screenManager()->push(new AdhocServerScreen(&g_Config.sProAdhocServer, nw->T("Ad hoc server address")));
		});
		addItem(pane.list, mm->T("Remote disc streaming"), mm->T("RemoteDiscSummary", "Play games shared from another device"), "I_WIFI", Xmb::Hue::Violet, [this]() {
			screenManager()->push(new RemoteISOScreen(Path()));
		});
		addItem(pane.list, ms->T("Networking"), mm->T("NetworkingSummary", "Ad hoc \xC2\xB7 infrastructure \xC2\xB7 ports"), "I_GEAR", Xmb::Hue::Lime, [this]() {
			g_Config.iSettingsCurrentTab = 3;
			screenManager()->push(new GameSettingsScreen(Path()));
		});
	}

	{
		XmbPane &pane = addPane(XmbCategory::PPSSPP, "PPSSPP", ImageID("I_PSP"), mm->T("About PPSSPP"));
		pane.list = pane.content->Add(new Xmb::List(new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT)));
		std::string version = PPSSPP_GIT_VERSION;
		addItem(pane.list, mm->T("About PPSSPP"), version, "I_INFO", Xmb::Hue::Aqua, [this]() {
			screenManager()->push(new CreditsScreen());
		});
		addItem(pane.list, "www.ppsspp.org", mm->T("WebsiteSummary", "Website and guides"), "I_WEB_BROWSER", Xmb::Hue::Violet, []() {
			System_LaunchUrl(LaunchUrlType::BROWSER_URL, "https://www.ppsspp.org");
		});
		if (!System_GetPropertyBool(SYSPROP_APP_GOLD) && System_GetPropertyInt(SYSPROP_DEVICE_TYPE) != DEVICE_TYPE_VR) {
			addItem(pane.list, mm->T("Buy PPSSPP Gold"), mm->T("GoldSummary", "Support development"), "I_ICON_GOLD", Xmb::Hue::Amber, [this]() {
				LaunchBuyGold(screenManager());
			});
		}
#if !PPSSPP_PLATFORM(IOS_APP_STORE)
		addItem(pane.list, di->T("Exit"), mm->T("ExitSummary", "Close PPSSPP"), "I_EXIT", Xmb::Hue::Coral, []() {
			SaveConfigAndExit();
		});
#endif
	}

	// Room after the last item, so it can scroll up to the top like the others.
	const float tailH = std::max(0.0f, h - listY - listBottom - Xmb::ROW_H_FOCUSED);
	for (XmbPane &pane : xmbPanes_) {
		pane.content->Add(new Spacer(tailH));
		pane.scroll->SetVisibility(V_GONE);
	}

	xmbCategoryBar_ = xmb->Add(new Xmb::CategoryBar(categories, new AnchorLayoutParams(FILL_PARENT, 150.0f, 0.0f, categoryY, 0.0f, NONE)));
	xmbCategoryBar_->SetAnchorX(listX + Xmb::ORB_X_IN_LIST);
	xmbCategoryBar_->OnChoice.Add([this](EventParams &e) {
		SetXmbPane((int)e.a, true);
	});

	// Which category to start in, like the classic layout's default tab.
	XmbCategory start = XmbCategory::GAMES;
	if (xmbCategory_ >= 0) {
		start = (XmbCategory)xmbCategory_;
	} else if (g_recentFiles.HasAny() && g_Config.iMaxRecent > 0) {
		start = XmbCategory::RECENT;
	}
	if (backFromStore_ || showHomebrewTab) {
		start = XmbCategory::HOMEBREW;
		backFromStore_ = false;
		showHomebrewTab = false;
	}
	// Recent may be turned off. Then Games.
	int startPane = -1;
	int gamesPane = 0;
	for (int i = 0; i < (int)xmbPanes_.size(); i++) {
		if (xmbPanes_[i].category == start) {
			startPane = i;
		}
		if (xmbPanes_[i].category == XmbCategory::GAMES) {
			gamesPane = i;
		}
	}
	if (startPane < 0) {
		startPane = gamesPane;
	}
	xmbPane_ = -1;
	SetXmbPane(startPane, false);
}

void MainScreen::SetXmbPane(int index, bool animate) {
	using namespace UI;
	if (index < 0 || index >= (int)xmbPanes_.size() || index == xmbPane_) {
		return;
	}
	const int old = xmbPane_;
	xmbPane_ = index;
	XmbPane &pane = xmbPanes_[index];
	xmbCategory_ = (int)pane.category;
	xmbScrollTarget_ = -1.0f;

	View *focused = GetFocusedView();
	if (old >= 0) {
		XmbPane &prev = xmbPanes_[old];
		// Like the XMB, leaving a category forgets its search.
		if (prev.browser && !prev.browser->SearchFilter().empty()) {
			prev.browser->SetSearchFilter("", false);
		}
		if (prev.list) {
			prev.list->ClearSearch();
		}
		prev.scroll->SetVisibility(V_GONE);
		if (focused && prev.scroll->ContainsSubview(focused) && !IsFocusMovementEnabled()) {
			SetFocusedView(nullptr, FocusFlags::CAUSE_FOCUS_MOVE);
		}
	}

	pane.scroll->SetVisibility(V_VISIBLE);
	if (animate && old >= 0) {
		// Slide in from the side it's on, as the orbs slide the other way.
		AnchorTranslateTween *tween = new AnchorTranslateTween(0.3f, bezierEaseInOut);
		pane.scroll->AddTween(tween);
		tween->Reset(Point2D(index > old ? 80.0f : -80.0f, 0.0f));
		tween->Apply(pane.scroll);
		tween->Divert(Point2D(0.0f, 0.0f));
		PlayUISound(index > old ? UISound::TOGGLE_ON : UISound::TOGGLE_OFF);
	}
	if (xmbCategoryBar_) {
		xmbCategoryBar_->SetSelected(index, animate);
	}
	root_->SetDefaultFocusView(pane.scroll);

	if (IsFocusMovementEnabled()) {
		if (!(pane.lastFocus && pane.scroll->ContainsSubview(pane.lastFocus) && pane.lastFocus->GetVisibility() == V_VISIBLE && pane.lastFocus->SetFocus(FocusFlags::CAUSE_OTHER))) {
			pane.scroll->SetFocus(FocusFlags::CAUSE_OTHER);
		}
	}
}

void MainScreen::UpdateXmb() {
	using namespace UI;
	if (xmbPane_ < 0 || xmbPane_ >= (int)xmbPanes_.size()) {
		return;
	}
	XmbPane &pane = xmbPanes_[xmbPane_];

	std::string location = pane.browser ? pane.browser->FriendlyPath() : "";
	xmbTopBar_->SetCrumb(pane.label, location.empty() ? pane.crumb : location);
	xmbTopBar_->SetSearch(pane.browser ? pane.browser->SearchFilter() : pane.list ? pane.list->SearchFilter() : "");
	xmbCategoryBar_->SetAnchorX(xmbPaneHolder_->GetBounds().x + 16.0f + Xmb::ORB_X_IN_LIST);

	// Keep the selected row at the top of the list, like the XMB.
	View *focused = GetFocusedView();
	if (!focused || !pane.scroll->ContainsSubview(focused)) {
		return;
	}
	pane.lastFocus = focused;
	if (!IsFocusMovementEnabled()) {
		return;
	}
	const float offset = focused->GetBounds().y - pane.content->GetBounds().y;
	if (fabsf(offset - xmbScrollTarget_) > 0.5f) {
		pane.scroll->ScrollTo(offset);
		xmbScrollTarget_ = offset;
	}
}

bool MainScreen::XmbKey(const KeyInput &key) {
	if (!(key.flags & KeyInputFlags::DOWN)) {
		return false;
	}
	if ((key.keyCode == NKCODE_K && (key.flags & KEY_MOD_SHORTCUT)) || UI::IsQuickActionKey(key)) {
		ShowQuickActions();
		return true;
	}
	// The keyboard's left and right also change category, as the list only goes up and down.
	const bool keyboardOrPad = key.deviceId != DEVICE_ID_MOUSE;
	if (UI::IsTabLeftKey(key) || key.keyCode == NKCODE_PAGE_UP || (keyboardOrPad && key.keyCode == NKCODE_DPAD_LEFT && !(key.flags & KeyInputFlags::ModAlt))) {
		SetXmbPane(xmbPane_ - 1, true);
		return true;
	}
	if (UI::IsTabRightKey(key) || key.keyCode == NKCODE_PAGE_DOWN || (keyboardOrPad && key.keyCode == NKCODE_DPAD_RIGHT && !(key.flags & KeyInputFlags::ModAlt))) {
		SetXmbPane(xmbPane_ + 1, true);
		return true;
	}
	return false;
}

void MainScreen::ShowQuickActions() {
	auto mm = GetI18NCategory(I18NCat::MAINMENU);
	auto di = GetI18NCategory(I18NCat::DIALOG);
	auto sy = GetI18NCategory(I18NCat::SYSTEM);

	struct Action {
		std::string title;
		const char *icon;
		std::function<void()> run;
	};
	std::vector<Action> actions;
	if (System_GetPropertyBool(SYSPROP_HAS_FILE_BROWSER)) {
		actions.push_back({ std::string(mm->T("Load", "Load...")), "I_FOLDER_OPEN", [this]() {
			UI::EventParams e{};
			OnLoadFile(e);
		} });
	}
	actions.push_back({ std::string(mm->T("Game Settings", "Settings")), "I_GEAR", [this]() {
		screenManager()->push(new GameSettingsScreen(Path()));
	} });
	if (System_GetPropertyInt(SYSPROP_DEVICE_TYPE) == DEVICE_TYPE_DESKTOP) {
		actions.push_back({ std::string(mm->T("Toggle fullscreen")), "I_FULLSCREEN", []() {
			g_Config.bFullScreen = !g_Config.bFullScreen;
			System_ApplyFullscreenState();
		} });
	}
	if (g_Config.iMaxRecent > 0 && g_recentFiles.HasAny()) {
		actions.push_back({ std::string(sy->T("Clear Recent Games List")), "I_TRASHCAN", [this]() {
			g_recentFiles.Clear();
			screenManager()->RecreateAllViews();
		} });
	}
	actions.push_back({ std::string(mm->T("About PPSSPP")), "I_INFO", [this]() {
		screenManager()->push(new CreditsScreen());
	} });
#if !PPSSPP_PLATFORM(IOS_APP_STORE)
	actions.push_back({ std::string(di->T("Exit")), "I_EXIT", []() {
		SaveConfigAndExit();
	} });
#endif

	std::vector<std::string> titles;
	std::map<int, ImageID> icons;
	for (int i = 0; i < (int)actions.size(); i++) {
		titles.push_back(actions[i].title);
		icons[i] = ImageID(actions[i].icon);
	}
	UI::ListPopupScreen *popup = new UI::ListPopupScreen(mm->T("Quick actions"), titles, -1, [actions](int choice) {
		if (choice >= 0 && choice < (int)actions.size()) {
			actions[choice].run();
		}
	});
	popup->SetChoiceIcons(icons);
	screenManager()->push(popup);
}

bool MainScreen::key(const KeyInput &key) {
	if (xmbLayout_ && XmbKey(key)) {
		return true;
	}
	if (key.flags & KeyInputFlags::DOWN) {
		if (key.keyCode == NKCODE_F && (key.flags & KEY_MOD_SHORTCUT) && System_GetPropertyBool(SYSPROP_HAS_TEXT_INPUT_DIALOG)) {
			auto se = GetI18NCategory(I18NCat::SEARCH);
			System_InputBoxGetString(GetRequesterToken(), se->T("Search term"), searchFilter_, false, [this](std::string_view value, int) {
				searchFilter_ = StripSpaces(value);
				searchChanged_ = true;
			});
		}
	}

	bool retval = UIBaseScreen::key(key);
	if (retval) {
		return true;
	}

	// This is not a DialogScreen so we have to implement behavior here too.
	// However we add a small safety hatch by checking for gamepad, and for now we only allow this behavior
	// on Android. Might reconsider for other platforms.
	#if PPSSPP_PLATFORM(ANDROID)
	if (key.flags & KeyInputFlags::DOWN) {
		if ((key.deviceId == DEVICE_ID_PAD_0 || key.deviceId == DEVICE_ID_XINPUT_0) && UI::IsEscapeKey(key)) {
			System_ExitApp();
		}
	}
	#endif
	return true;
}

void MainScreen::OnAllowStorage(UI::EventParams &e) {
	System_AskForPermission(SYSTEM_PERMISSION_STORAGE);
}

// See Config::SupportsUpgradeCheck() if you add more platforms.
void MainScreen::OnDownloadUpgrade(UI::EventParams &e) {
#if PPSSPP_PLATFORM(ANDROID)
	// Go to app store
	if (System_GetPropertyBool(SYSPROP_APP_GOLD)) {
		System_LaunchUrl(LaunchUrlType::BROWSER_URL, "market://details?id=org.ppsspp.ppssppgold");
	} else {
		System_LaunchUrl(LaunchUrlType::BROWSER_URL, "market://details?id=org.ppsspp.ppsspp");
	}
#elif PPSSPP_PLATFORM(WINDOWS)
	System_LaunchUrl(LaunchUrlType::BROWSER_URL, "https://www.ppsspp.org/download");
#elif PPSSPP_PLATFORM(IOS_APP_STORE)
	System_LaunchUrl(LaunchUrlType::BROWSER_URL, "itms-apps://itunes.apple.com/app/id6496972903");
#else
	// Go directly to ppsspp.org and let the user sort it out
	// (for details and in case downloads doesn't have their platform.)
	System_LaunchUrl(LaunchUrlType::BROWSER_URL, "https://www.ppsspp.org/");
#endif
}

void MainScreen::sendMessage(UIMessage message, const char *value) {
	// Always call the base class method first to handle the most common messages.
	UIBaseScreen::sendMessage(message, value);

	if (message == UIMessage::REQUEST_GAME_BOOT) {
		LaunchFile(screenManager(), this, Path(value));
	} else if (message == UIMessage::PERMISSION_GRANTED && !strcmp(value, "storage")) {
		RecreateViews();
	} else if (message == UIMessage::RECENT_FILES_CHANGED) {
		RecreateViews();
	}
}

void MainScreen::update() {
	UIScreen::update();
	UpdateUIState(UISTATE_MENU);

	if (builtLayout_ != g_Config.iMainMenuLayout) {
		RecreateViews();
	} else if (xmbLayout_) {
		UpdateXmb();
	}

	if (packGeneration_ != GamePacker::Generation()) {
		// A pack finished: a new file, and maybe the original gone.
		packGeneration_ = GamePacker::Generation();
		const GamePacker::Result result = GamePacker::LastResult();
		if (restoreFocusGamePath_ == result.oldPath) {
			restoreFocusGamePath_ = result.newPath;
		}
		for (auto browser : gameBrowsers_) {
			browser->RequestRefresh();
		}
	}

	if (searchChanged_) {
		for (auto browser : gameBrowsers_) {
			if (browser->GetVisibility() == UI::V_VISIBLE) {
				browser->SetSearchFilter(searchFilter_, false);
			}
		}
		searchChanged_ = false;
	}
}

void MainScreen::OnLoadFile(UI::EventParams &e) {
	if (System_GetPropertyBool(SYSPROP_HAS_FILE_BROWSER)) {
		auto mm = GetI18NCategory(I18NCat::MAINMENU);
		System_BrowseForFile(GetRequesterToken(), mm->T("Load"), BrowseFileType::BOOTABLE, [](std::string_view value, int) {
			System_PostUIMessage(UIMessage::REQUEST_GAME_BOOT, value);
		});
	}
}

void MainScreen::DrawBackground(UIContext &dc) {
	if (highlightedBackgrounds_.empty()) {
		return;
	}

	constexpr float fadeTime = 0.25f;

	double now = time_now_d();

	for (auto iter = highlightedBackgrounds_.begin(); iter != highlightedBackgrounds_.end(); ) {
		std::shared_ptr<GameInfo> ginfo;
		ginfo = g_gameInfoCache->GetInfo(dc.GetDrawContext(), iter->gamePath, GameInfoFlags::PIC1);
		float timeSinceStart = float(now - std::max(iter->startTime, ginfo->pic1.timeLoaded));
		float alpha = std::clamp(timeSinceStart / fadeTime, 0.0f, 1.0f);
		if (iter->endTime > 0.0) {
			// TODO: Consider only fading out if it's the last one in the list, to avoid background shine-through.
			float fadeOutAlpha = std::max(0.0f, float(now - iter->endTime) / fadeTime);
			if (fadeOutAlpha > 1.0f) {
				iter = highlightedBackgrounds_.erase(iter);
				continue;
			}
			alpha *= 1.0f - fadeOutAlpha;
		}
		iter++;

		if (!ginfo->pic1.texture) {
			continue;
		}

		// The XMB layout keeps it faint behind the ribbon, so the list stays readable.
		DrawBackgroundTexture(dc, ginfo->pic1.texture, Lin::Vec3(0.0f, 0.0f, 0.0f), xmbLayout_ ? alpha * 0.3f : alpha);
	}
}

void MainScreen::DrawBackgroundFor(UIContext &dc, const Path &gamePath, float alpha) {
	::DrawGameBackground(dc, gamePath, Lin::Vec3(0.f, 0.f, 0.f), alpha);
}

void MainScreen::OnGameSelected(UI::EventParams &e) {
	Path path(e.s);
	std::shared_ptr<GameInfo> ginfo = g_gameInfoCache->GetInfo(nullptr, path, GameInfoFlags::FILE_TYPE);
	if (ginfo->fileType == IdentifiedFileType::PSP_SAVEDATA_DIRECTORY) {
		return;
	}
	if (g_GameManager.GetState() == GameManagerState::INSTALLING)
		return;

	// Restore focus if it was highlighted (e.g. by gamepad.)
	restoreFocusGamePath_ = highlightedGamePath_;
	g_BackgroundAudio.SetGame(path);
	lockBackgroundAudio_ = true;
	screenManager()->push(new GameScreen(path, false));
}

void MainScreen::InstantHighlight(const Path &path) {
	// Clear the previous highlight immediately, so we don't have multiple at once.
	highlightedBackgrounds_.clear();
	highlightedBackgrounds_.push_back({path, 0.0f, -1.0});
}

void MainScreen::OnGameHighlight(UI::EventParams &e) {
	using namespace UI;

	Path path(e.s);

	const FocusFlags focusFlags = (FocusFlags)e.a;

	if (path == highlightedGamePath_ && (focusFlags & FocusFlags::GOT_FOCUS)) {
		// Already highlighted, nothing to do.
		return;
	}

	if (focusFlags & FocusFlags::LOST_FOCUS) {
		// Lost focus, so we want to fade out the background.

		// Trigger fadeouts on any active highlights.
		for (auto &iter : highlightedBackgrounds_) {
			if (iter.endTime < 0.0) {
				iter.endTime = time_now_d();
			}
		}
		highlightedGamePath_.clear();
		if ((focusFlags & FocusFlags::CAUSE_FOCUS_MOVE) || (focusFlags & FocusFlags::CAUSE_KB_FOCUS_DISABLED)) {
			// Focus moved to another game, so we want to fade out so we can fade in the new one.
			g_BackgroundAudio.SetGame(Path());
		}
		return;
	}

	highlightedGamePath_ = path;

	_dbg_assert_(!path.empty());

	if (path.empty()) {
		// Nothing highlighed? Exit.
		return;
	}

	// Add a new entry to the highlight list.
	highlightedBackgrounds_.push_back({path, time_now_d(), -1.0});
	if ((!highlightedGamePath_.empty() || (focusFlags & FocusFlags::LOST_FOCUS)) && !lockBackgroundAudio_) {
		g_BackgroundAudio.SetGame(highlightedGamePath_);
	}

	lockBackgroundAudio_ = false;
}

void MainScreen::OnGameSelectedInstant(UI::EventParams &e) {
	ScreenManager *screen = screenManager();
	LaunchFile(screen, nullptr, Path(e.s));
}

void MainScreen::OnGameSettings(UI::EventParams &e) {
	// Not passing a game ID, changing the global settings.
	screenManager()->push(new GameSettingsScreen(Path()));
}

void MainScreen::OnCredits(UI::EventParams &e) {
	screenManager()->push(new CreditsScreen());
}

void LaunchBuyGold(ScreenManager *screenManager) {
	if (System_GetPropertyBool(SYSPROP_USE_IAP)) {
		screenManager->push(new IAPScreen(true));
	} else if (System_GetPropertyBool(SYSPROP_USE_APP_STORE)) {
#if PPSSPP_PLATFORM(ANDROID)
		LaunchPlayStoreOrWebsiteGold();
#else
		screenManager->push(new IAPScreen(false));
#endif
	} else {
#if PPSSPP_PLATFORM(IOS_APP_STORE)
		System_LaunchUrl(LaunchUrlType::BROWSER_URL, "https://www.ppsspp.org/buygold_ios");
#else
		System_LaunchUrl(LaunchUrlType::BROWSER_URL, "https://www.ppsspp.org/buygold");
#endif
	}
}

void MainScreen::OnPPSSPPOrg(UI::EventParams &e) {
	System_LaunchUrl(LaunchUrlType::BROWSER_URL, "https://www.ppsspp.org");
}

void MainScreen::OnForums(UI::EventParams &e) {
	System_LaunchUrl(LaunchUrlType::BROWSER_URL, "https://forums.ppsspp.org");
}

void MainScreen::dialogFinished(const Screen *dialog, DialogResult result) {
	std::string tag = dialog->tag();
	if (tag == "Store") {
		backFromStore_ = true;
		RecreateViews();
	} else if (tag == "Game") {
		if (static_cast<const GameScreen *>(dialog)->GameRemoved()) {
			// Deleted or removed from recents. The recents tab recreates itself on RECENT_FILES_CHANGED.
			restoreFocusGamePath_.clear();
			g_BackgroundAudio.SetGame(Path());
			for (auto browser : gameBrowsers_) {
				browser->RequestRefresh();
			}
		} else if (!restoreFocusGamePath_.empty() && UI::IsFocusMovementEnabled()) {
			// Prevent the background from fading, since we just were displaying it.
			InstantHighlight(restoreFocusGamePath_);

			// Refocus the game button itself.
			if (xmbLayout_) {
				if (xmbPane_ >= 0 && xmbPane_ < (int)xmbPanes_.size() && xmbPanes_[xmbPane_].browser) {
					xmbPanes_[xmbPane_].browser->FocusGame(restoreFocusGamePath_);
				}
			} else {
				int tab = tabHolder_->GetCurrentTab();
				if (tab >= 0 && tab < (int)gameBrowsers_.size()) {
					gameBrowsers_[tab]->FocusGame(restoreFocusGamePath_);
				}
			}

			// Don't get confused next time.
			restoreFocusGamePath_.clear();
		} else {
			// Not refocusing, so we need to stop the audio.
			g_BackgroundAudio.SetGame(Path());
		}
	} else if (tag == "InstallZip") {
		INFO_LOG(Log::System, "InstallZip finished, refreshing");
		if (gameBrowsers_.size() >= 2) {
			gameBrowsers_[1]->RequestRefresh();
		}
	} else if (tag == "IAP") {
		// Gold status may have changed.
		RecreateViews();
	} else if (tag == "Upload") {
		// Files may have been uploaded.
		RecreateViews();
	} else if (tag == "SavedataPopup") {
		// We must have come from the file browser tab.
		if (gameBrowsers_.size() >= 2) {
			gameBrowsers_[1]->RequestRefresh();
		}
	}
}

void UmdReplaceScreen::CreateViews() {
	using namespace UI;
	Margins actionMenuMargins(0, 100, 15, 0);
	auto mm = GetI18NCategory(I18NCat::MAINMENU);
	auto di = GetI18NCategory(I18NCat::DIALOG);

	const bool portrait = GetDeviceOrientation() == DeviceOrientation::Portrait;

	TabHolder *leftColumn = new TabHolder(ORIENT_HORIZONTAL, 64, TabHolderFlags::Default, nullptr, nullptr, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, 1.0));
	leftColumn->SetTag("UmdReplace");
	leftColumn->SetClip(true);

	ViewGroup *rightColumn = new ScrollView(ORIENT_VERTICAL, new LinearLayoutParams(270, FILL_PARENT, actionMenuMargins));
	LinearLayout *rightColumnItems = new LinearLayout(ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT));
	rightColumnItems->SetSpacing(0.0f);
	rightColumn->Add(rightColumnItems);

	if (g_Config.iMaxRecent > 0) {
		ScrollView *scrollRecentGames = new ScrollView(ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT));
		scrollRecentGames->SetTag("UmdReplaceRecentGames");
		GameBrowser *tabRecentGames = new GameBrowser(GetRequesterToken(),
			Path("!RECENT"), BrowseFlags::NONE, portrait, &g_Config.iGameBrowserView1, screenManager(), "", "",
			new LinearLayoutParams(FILL_PARENT, FILL_PARENT));
		scrollRecentGames->Add(tabRecentGames);
		leftColumn->AddTab(mm->T("Recent"), ImageID::invalid(), scrollRecentGames);
		tabRecentGames->OnChoice.Handle(this, &UmdReplaceScreen::OnGameSelected);
		tabRecentGames->OnHoldChoice.Handle(this, &UmdReplaceScreen::OnGameSelected);
	}
	ScrollView *scrollAllGames = new ScrollView(ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT));
	scrollAllGames->SetTag("UmdReplaceAllGames");

	GameBrowser *tabAllGames = new GameBrowser(GetRequesterToken(), Path(g_Config.currentDirectory), BrowseFlags::STANDARD, portrait, &g_Config.iGameBrowserView2, screenManager(),
		mm->T("How to get games"), "https://www.ppsspp.org/getgames.html",
		new LinearLayoutParams(FILL_PARENT, FILL_PARENT));

	scrollAllGames->Add(tabAllGames);

	leftColumn->AddTab(mm->T("Games"), ImageID::invalid(), scrollAllGames);

	tabAllGames->OnChoice.Handle(this, &UmdReplaceScreen::OnGameSelected);

	tabAllGames->OnHoldChoice.Handle(this, &UmdReplaceScreen::OnGameSelected);

	if (System_GetPropertyBool(SYSPROP_HAS_FILE_BROWSER)) {
		rightColumnItems->Add(new Choice(mm->T("Load", "Load...")))->OnClick.Add([&](UI::EventParams &e) {
			auto mm = GetI18NCategory(I18NCat::MAINMENU);
			System_BrowseForFile(GetRequesterToken(), mm->T("Load"), BrowseFileType::BOOTABLE, [this](std::string_view value, int) {
				UseDisc(Path(value));
				TriggerFinish(DR_OK);
			});
		});
	}

	rightColumnItems->Add(new Choice(di->T("Cancel")))->OnClick.Handle<UIScreen>(this, &UIScreen::OnCancel);
	rightColumnItems->Add(new Spacer());
	rightColumnItems->Add(new Choice(mm->T("Game Settings")))->OnClick.Handle(this, &UmdReplaceScreen::OnGameSettings);

	if (g_recentFiles.HasAny()) {
		leftColumn->SetCurrentTab(0, true);
	} else if (g_Config.iMaxRecent > 0) {
		leftColumn->SetCurrentTab(1, true);
	}

	root_ = new LinearLayout(ORIENT_HORIZONTAL);
	root_->Add(leftColumn);
	root_->Add(rightColumn);
}

void UmdReplaceScreen::update() {
	UIBaseDialogScreen::update();
	UpdateUIState(UISTATE_PAUSEMENU);
}

void UmdReplaceScreen::OnGameSelected(UI::EventParams &e) {
	UseDisc(Path(e.s));
	TriggerFinish(DR_OK);
}

void UmdReplaceScreen::UseDisc(const Path &path) {
	if (!insert_) {
		__UmdReplace(path);
		return;
	}
	std::string error;
	if (!__UmdInsert(path, &error)) {
		g_OSD.Show(OSDType::MESSAGE_ERROR, error);
	}
}

void UmdReplaceScreen::OnGameSettings(UI::EventParams &e) {
	screenManager()->push(new GameSettingsScreen(Path()));
}

void GridSettingsPopupScreen::CreatePopupContents(UI::ViewGroup *parent) {
	using namespace UI;

	auto di = GetI18NCategory(I18NCat::DIALOG);
	auto sy = GetI18NCategory(I18NCat::SYSTEM);
	auto mm = GetI18NCategory(I18NCat::MAINMENU);

	ScrollView *scroll = new ScrollView(ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, 1.0f));
	LinearLayout *items = new LinearLayoutList(ORIENT_VERTICAL);
	items->SetSpacing(0.0f);

	// Same order as GameBrowserViewMode.
	static const char *viewModes[] = { "Grid", "List", "Large list" };
	items->Add(new PopupMultiChoice(&g_Config.iGameBrowserView1, mm->T("Recent"), viewModes, 0, ARRAY_SIZE(viewModes), I18NCat::SYSTEM, screenManager()));
	items->Add(new PopupMultiChoice(&g_Config.iGameBrowserView2, mm->T("Games"), viewModes, 0, ARRAY_SIZE(viewModes), I18NCat::SYSTEM, screenManager()));
	items->Add(new PopupMultiChoice(&g_Config.iGameBrowserView3, mm->T("Homebrew & Demos"), viewModes, 0, ARRAY_SIZE(viewModes), I18NCat::SYSTEM, screenManager()));
	static const char *defaultTabs[] = { "Recent", "Games", "Homebrew & Demos" };
	PopupMultiChoice *beziersChoice = items->Add(new PopupMultiChoice(&g_Config.iDefaultTab, sy->T("Default tab"), defaultTabs, 0, ARRAY_SIZE(defaultTabs), I18NCat::MAINMENU, screenManager()));

	items->Add(new ItemHeader(sy->T("Icon size")));
	items->Add(new Choice(sy->T("Increase size")))->OnClick.Handle(this, &GridSettingsPopupScreen::GridPlusClick);
	items->Add(new Choice(sy->T("Decrease size")))->OnClick.Handle(this, &GridSettingsPopupScreen::GridMinusClick);

	items->Add(new ItemHeader(sy->T("Display Extra Info")));
	items->Add(new CheckBox(&g_Config.bShowIDOnGameIcon, sy->T("Show ID")));
	items->Add(new CheckBox(&g_Config.bShowRegionOnGameIcon, sy->T("Show region flag")));

	if (g_Config.iMaxRecent > 0) {
		items->Add(new ItemHeader(sy->T("Clear Recent")));
		items->Add(new Choice(sy->T("Clear Recent Games List")))->OnClick.Handle(this, &GridSettingsPopupScreen::OnRecentClearClick);
	}

	scroll->Add(items);
	parent->Add(scroll);
}

void GridSettingsPopupScreen::GridPlusClick(UI::EventParams &e) {
	g_Config.fGameGridScale = std::min(g_Config.fGameGridScale*1.25f, MAX_GAME_GRID_SCALE);
}

void GridSettingsPopupScreen::GridMinusClick(UI::EventParams &e) {
	g_Config.fGameGridScale = std::max(g_Config.fGameGridScale/1.25f, MIN_GAME_GRID_SCALE);
}

void GridSettingsPopupScreen::OnRecentClearClick(UI::EventParams &e) {
	g_recentFiles.Clear();
	OnRecentChanged.Trigger(e);
	TriggerFinish(DR_OK);
}
