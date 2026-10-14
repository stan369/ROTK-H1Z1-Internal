#include "gui.h"

// This file is #include'd directly from Lol2.cpp (single translation unit, matching the
// rest of this small project) so it has direct access to every g_show*/g_aimbot/color/etc.
// toggle, GetKeyName(), SaveConfigAction()/RequestUnloadAction(), and
// g_awaitingKeybindTarget without an extern-heavy header boundary - all of those are
// already declared earlier in Lol2.cpp by the point this gets included.

void Gui::Init(HWND hWnd, ID3D11Device* device, ID3D11DeviceContext* context) {
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	ImGui::StyleColorsDark();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	// No ImGuiConfigFlags_ViewportsEnable here - that lets ImGui windows pop out into
	// separate OS windows, which only makes sense for the standalone-app scaffold this
	// was adapted from. We're an overlay bound to the game's own window.
	char fontPath[MAX_PATH] = {};
	if (GetWindowsDirectoryA(fontPath, MAX_PATH)) {
		strcat_s(fontPath, "\\Fonts\\segoeui.ttf");
		if (GetFileAttributesA(fontPath) != INVALID_FILE_ATTRIBUTES) {
			io.Fonts->AddFontFromFileTTF(fontPath, 16.0f);
		}
	}

	ImGui_ImplWin32_Init(hWnd);
	ImGui_ImplDX11_Init(device, context);
}

void Gui::Shutdown() {
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
}

void Gui::Render(int width, int height) {
	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	this->DrawMainWindow(width, height);

	ImGui::Render();
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void Gui::DrawMainWindow(int width, int height) {
	ImGui::SetNextWindowSize(ImVec2((float)width, (float)height));
	ImGui::SetNextWindowPos(ImVec2(100, 100), ImGuiCond_FirstUseEver);

	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

	if (ImGui::Begin("##rotk", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground)) {
		ImDrawList* draw = ImGui::GetWindowDrawList();
		ImVec2 pos = ImGui::GetWindowPos();
		ImVec2 size = ImGui::GetWindowSize();
		this->HandleMenuDragging(pos, size);

		this->PanelShadow(draw, pos, size, 8.0f);

		// main panel - a subtle diagonal tint instead of a flat fill, just enough to give
		// the window some depth without needing an actual blur/backdrop shader
		draw->AddRectFilledMultiColor(pos, ImVec2(pos.x + size.x, pos.y + size.y),
			GuiTheme::BackgroundEdge, GuiTheme::Background, GuiTheme::Background, GuiTheme::BackgroundEdge);
		draw->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), GuiTheme::PanelBorder, 8.0f, 0, 1.0f);

		// title bar
		draw->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + this->titleBarHeight), GuiTheme::TitleBar, 8.0f, ImDrawFlags_RoundCornersTop);
		draw->AddRectFilledMultiColor(ImVec2(pos.x, pos.y + this->titleBarHeight - 2), ImVec2(pos.x + size.x, pos.y + this->titleBarHeight), GuiTheme::AccentA, GuiTheme::AccentB, GuiTheme::AccentB, GuiTheme::AccentA);

		draw->AddCircleFilled(ImVec2(pos.x + 16, pos.y + this->titleBarHeight / 2), 3.5f, GuiTheme::AccentA);
		const char* title = "ROTK";
		ImVec2 titleSize = ImGui::CalcTextSize(title);
		this->Text(draw, ImVec2(pos.x + 28, pos.y + this->titleBarHeight / 2 - titleSize.y / 2), title, GuiTheme::TextPrimary, true);

		char breadcrumb[48];
		sprintf_s(breadcrumb, "/ %s", this->tabs[this->activeTab]);
		this->Text(draw, ImVec2(pos.x + 28 + titleSize.x + 8, pos.y + this->titleBarHeight / 2 - titleSize.y / 2), breadcrumb, GuiTheme::TextMuted, true);

		const char* hint = "[INS] toggle menu";
		ImVec2 hintSize = ImGui::CalcTextSize(hint);
		this->Text(draw, ImVec2(pos.x + size.x - hintSize.x - 14, pos.y + this->titleBarHeight / 2 - hintSize.y / 2), hint, GuiTheme::TextMuted, true);

		// sidebar
		draw->AddRectFilled(ImVec2(pos.x, pos.y + this->titleBarHeight), ImVec2(pos.x + this->sidebarWidth, pos.y + size.y), GuiTheme::Sidebar);
		draw->AddLine(ImVec2(pos.x + this->sidebarWidth, pos.y + this->titleBarHeight), ImVec2(pos.x + this->sidebarWidth, pos.y + size.y), GuiTheme::PanelBorder, 1.0f);

		float deltaTime = ImGui::GetIO().DeltaTime;

		for (int i = 0; i < IM_ARRAYSIZE(this->tabs); i++) {
			ImVec2 tabPos(pos.x + 10, pos.y + this->titleBarHeight + 14 + i * (this->tabHeight + this->tabSpacing));
			ImVec2 tabEnd(tabPos.x + this->tabWidth, tabPos.y + this->tabHeight);

			ImVec2 mousePos = ImGui::GetIO().MousePos;
			bool hovered = mousePos.x >= tabPos.x && mousePos.x <= tabEnd.x && mousePos.y >= tabPos.y && mousePos.y <= tabEnd.y;
			if (hovered && ImGui::IsMouseClicked(0)) {
				this->activeTab = i;
			}

			static AnimFloat tabAnims[IM_ARRAYSIZE(tabs)];
			float anim = tabAnims[i].AnimateTo((this->activeTab == i) ? 1.0f : (hovered ? 0.5f : 0.0f), deltaTime);

			static AnimFloat highlightY;
			float targetY = pos.y + this->titleBarHeight + 14 + this->activeTab * (this->tabHeight + this->tabSpacing);
			float highlightAnimY = highlightY.AnimateTo(targetY, deltaTime);

			if (this->activeTab == i) {
				int vtxStart = draw->VtxBuffer.Size;
				draw->AddRectFilled(ImVec2(tabPos.x, highlightAnimY), ImVec2(tabEnd.x, highlightAnimY + this->tabHeight), ImColor(GuiTheme::AccentA.Value.x, GuiTheme::AccentA.Value.y, GuiTheme::AccentA.Value.z, anim), 6.0f);
				int vtxEnd = draw->VtxBuffer.Size;
				ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw, vtxStart, vtxEnd, ImVec2(tabPos.x, highlightAnimY), ImVec2(tabEnd.x, highlightAnimY), GuiTheme::AccentA, GuiTheme::AccentB);
			} else if (hovered) {
				draw->AddRectFilled(tabPos, tabEnd, ImColor(255, 255, 255, (int)(anim * 18)), 6.0f);
			}

			ImColor iconColor = (this->activeTab == i) ? GuiTheme::TextPrimary : GuiTheme::TextMuted;
			this->DrawTabIcon(draw, ImVec2(tabPos.x + 16, tabPos.y + this->tabHeight / 2), i, iconColor);
			this->Text(draw, ImVec2(tabPos.x + 32, tabPos.y + this->tabHeight / 2 - ImGui::CalcTextSize(this->tabs[i]).y / 2), this->tabs[i], (this->activeTab == i) ? GuiTheme::TextPrimary : GuiTheme::TextMuted, true);
		}

		const char* footer = "Stan369 - UC.me";
		ImVec2 footerSize = ImGui::CalcTextSize(footer);
		this->Text(draw, ImVec2(pos.x + this->sidebarWidth / 2 - footerSize.x / 2, pos.y + size.y - footerSize.y - 12), footer, ImColor(90, 94, 108, 255), false);

		// content
		float contentX = pos.x + this->sidebarWidth + 20.0f;
		float contentY = pos.y + this->titleBarHeight + 20.0f;
		float contentW = size.x - this->sidebarWidth - 40.0f;
		float contentH = size.y - this->titleBarHeight - 40.0f;

		switch (this->activeTab) {
		case 0: this->DrawCombatTab(draw, contentX, contentY, contentW, contentH); break;
		case 1: this->DrawEspTab(draw, contentX, contentY, contentW, contentH); break;
		case 2: this->DrawSettingsTab(draw, contentX, contentY, contentW, contentH); break;
		case 3: this->DrawFriendsTab(draw, contentX, contentY, contentW, contentH); break;
		}
	}
	ImGui::PopStyleVar();
	ImGui::End();
}

void Gui::DrawCombatTab(ImDrawList* draw, float contentX, float contentY, float contentW, float contentH) {
	// The editor replaces the whole tab with a per-weapon FOV/Smoothness list while open,
	// instead of trying to cram all of them into a corner of the normal Aimbot panel - but
	// its own open/closed state is separate from g_weaponProfiles (the actual enable
	// toggle), so leaving the editor doesn't silently turn the feature back off.
	if (g_weaponProfilesEditorOpen) {
		auto profC = this->Child(draw, ImVec2(contentX, contentY), ImVec2(contentW, contentH), "Weapon Profiles");
		this->Checkbox(draw, profC, &g_weaponProfiles, "Weapon Profiles Enabled");
		if (this->Button(draw, profC, "< Back", GuiTheme::AccentA)) {
			g_weaponProfilesEditorOpen = false;
		}
		this->Divider(draw, profC);
		for (int i = 0; i < g_weaponProfileCount; i++) {
			this->WeaponProfileRow(draw, profC, g_weaponProfileTable[i].displayName,
				&g_weaponProfileTable[i].fov, &g_weaponProfileTable[i].smoothness);
		}
		return;
	}

	float spacing = 20.0f;
	float childW = (contentW - spacing) / 2.0f;

	auto aimC = this->Child(draw, ImVec2(contentX, contentY), ImVec2(childW, contentH), "Aimbot");
	this->Checkbox(draw, aimC, &g_aimbot, "Aimbot");
	this->KeyBindButton(draw, aimC, &g_aimbotKey, &g_awaitingKeybindTarget, "Activation Key (hold)", GetKeyName);
	this->KeyBindButton(draw, aimC, &g_aimbotToggleKey, &g_awaitingKeybindTarget, "Toggle Key", GetKeyName);
	this->Divider(draw, aimC);
	this->SliderFloat(draw, aimC, &g_aimbotFov, 5.0f, 150.0f, "FOV");
	this->Checkbox(draw, aimC, &g_showFovCircle, "Show FOV Circle");
	this->SliderFloat(draw, aimC, &g_aimbotSmoothness, 1.0f, 15.0f, "Smoothness");
	this->Checkbox(draw, aimC, &g_weaponProfiles, "Weapon Profiles");
	if (this->Button(draw, aimC, "Configure Weapons", GuiTheme::AccentA)) {
		g_weaponProfilesEditorOpen = true;
	}

	auto recoilC = this->Child(draw, ImVec2(contentX + childW + spacing, contentY), ImVec2(childW, contentH), "NoRecoil");
	this->Checkbox(draw, recoilC, &g_noRecoil, "NoRecoil", g_safeMode);
	this->KeyBindButton(draw, recoilC, &g_noRecoilToggleKey, &g_awaitingKeybindTarget, "Toggle Key", GetKeyName);
}

void Gui::DrawEspTab(ImDrawList* draw, float contentX, float contentY, float contentW, float contentH) {
	float spacing = 20.0f;
	float childW = (contentW - spacing) / 2.0f;
	// Player ESP grew past what an even 50/50 split could fit (Head Dot, Skeleton added
	// on top of the existing rows), while Vehicle ESP still only needs 5 rows, so the top
	// row gets noticeably more of the vertical space instead of an even split.
	float topH = (contentH - spacing) * 0.64f;
	float botH = (contentH - spacing) * 0.36f;

	auto espC = this->Child(draw, ImVec2(contentX, contentY), ImVec2(childW, topH), "Player ESP");
	this->Checkbox(draw, espC, &g_showPlayerBoxes, "Boxes");
	this->ColorPicker(draw, espC, g_playerColor, "Box Color");
	this->Divider(draw, espC);
	this->Checkbox(draw, espC, &g_showPlayerNames, "Names");
	this->Checkbox(draw, espC, &g_showHealthBars, "Health Bars");
	this->Checkbox(draw, espC, &g_showDistance, "Distance (players + cars)");
	this->Checkbox(draw, espC, &g_showHeadDot, "Head Dot");
	this->ColorPicker(draw, espC, g_headDotColor, "Head Dot Color");
	this->Checkbox(draw, espC, &g_showSkeleton, "Skeleton");
	this->ColorPicker(draw, espC, g_skeletonColor, "Skeleton Color");

	auto lootC = this->Child(draw, ImVec2(contentX + childW + spacing, contentY), ImVec2(childW, topH), "Loot ESP");
	this->Checkbox(draw, lootC, &g_showItemESP, "Kevlars");
	this->ColorPicker(draw, lootC, g_itemColor, "Kevlar Color");
	this->Checkbox(draw, lootC, &g_showLootbags, "Lootbags");
	this->ColorPicker(draw, lootC, g_lootbagColor, "Lootbag Color");
	this->Checkbox(draw, lootC, &g_showGrenades, "HE Grenades");
	this->ColorPicker(draw, lootC, g_grenadeColor, "Grenade Color");
	this->Checkbox(draw, lootC, &g_showGroundWeapons, "Ground Weapons");
	this->ColorPicker(draw, lootC, g_weaponEspColor, "Weapon Color");

	auto carC = this->Child(draw, ImVec2(contentX, contentY + topH + spacing), ImVec2(contentW, botH), "Vehicle ESP");
	this->Checkbox(draw, carC, &g_showVehicleESP, "Car ESP");
	this->ColorPicker(draw, carC, g_offRoaderColor, "OffRoader Color");
	this->ColorPicker(draw, carC, g_car2Color, "Truck Color");
	this->ColorPicker(draw, carC, g_car3Color, "Police Car Color");
	this->ColorPicker(draw, carC, g_atvColor, "ATV Color");
}

void Gui::DrawSettingsTab(ImDrawList* draw, float contentX, float contentY, float contentW, float contentH) {
	float spacing = 20.0f;
	float childW = (contentW - spacing) / 2.0f;

	auto safeC = this->Child(draw, ImVec2(contentX, contentY), ImVec2(childW, contentH), "Safety");
	this->Checkbox(draw, safeC, &g_safeMode, "SafeMode");
	// Actually disable the running features, not just block re-enabling them, so
	// SafeMode means "off" the instant it's turned on - matches the old menu's behavior.
	if (g_safeMode) {
		g_noRecoil = false;
	}
	this->Text(draw, safeC, "Blocks NoRecoil.", GuiTheme::TextMuted, false);

	auto actionC = this->Child(draw, ImVec2(contentX + childW + spacing, contentY), ImVec2(childW, contentH), "Configs");

	char activeBuf[96];
	sprintf_s(activeBuf, "Active: %s", g_activeConfigName);
	this->Text(draw, actionC, activeBuf, GuiTheme::TextPrimary, false);

	if (this->Button(draw, actionC, "Save", GuiTheme::Success)) {
		SaveConfigAction();
	}
	if (g_savedFlashFrames > 0) {
		this->Text(draw, actionC, "Saved", GuiTheme::Success, true);
		g_savedFlashFrames--;
	}
	this->Divider(draw, actionC);

	this->TextInput(draw, actionC, g_newConfigNameBuf, sizeof(g_newConfigNameBuf), "New Config Name");
	if (this->Button(draw, actionC, "Save As New Config", GuiTheme::AccentA)) {
		if (g_newConfigNameBuf[0] != '\0') {
			SanitizeConfigName(g_newConfigNameBuf);
			SaveConfigAsNew(g_newConfigNameBuf);
			g_newConfigNameBuf[0] = '\0';
		}
	}
	this->Divider(draw, actionC);

	// Click a name to load it, the X to delete it - same row pattern as the Friends list.
	if (g_configNames.empty()) {
		this->Text(draw, actionC, "No saved configs yet.", GuiTheme::TextMuted, false);
	} else {
		const float rowHeight = 22.0f;
		const float removeSize = 16.0f;
		const float unloadButtonSpace = 28.0f + this->cursorSpacing;
		float usedSoFar = actionC.cursorPos.y - actionC.contentPos.y;
		int maxRows = (int)((actionC.contentSize.y - usedSoFar - unloadButtonSpace) / rowHeight);
		if (maxRows < 0) maxRows = 0;
		int shown = (int)min((size_t)maxRows, g_configNames.size());

		for (int i = 0; i < shown; i++) {
			const string& name = g_configNames[i];
			ImVec2 rowPos = actionC.cursorPos;
			bool isActive = name == g_activeConfigName;

			ImVec2 nameHitMax(rowPos.x + actionC.contentSize.x - removeSize - 6.0f, rowPos.y + rowHeight - 4.0f);
			ImVec2 mousePos = ImGui::GetIO().MousePos;
			bool nameHovered = mousePos.x >= rowPos.x && mousePos.x <= nameHitMax.x && mousePos.y >= rowPos.y - 2.0f && mousePos.y <= nameHitMax.y;

			this->Text(draw, ImVec2(rowPos.x, rowPos.y), name.c_str(), isActive ? GuiTheme::AccentA : (nameHovered ? GuiTheme::TextPrimary : GuiTheme::TextMuted), true);

			ImVec2 removeMin(rowPos.x + actionC.contentSize.x - removeSize, rowPos.y - 2.0f);
			ImVec2 removeMax(removeMin.x + removeSize, removeMin.y + removeSize);
			bool removeHovered = mousePos.x >= removeMin.x && mousePos.x <= removeMax.x && mousePos.y >= removeMin.y && mousePos.y <= removeMax.y;
			draw->AddRectFilled(removeMin, removeMax, removeHovered ? GuiTheme::Danger : GuiTheme::TrackOff, 4.0f);
			ImVec2 xCenter((removeMin.x + removeMax.x) / 2.0f, (removeMin.y + removeMax.y) / 2.0f);
			draw->AddLine(xCenter - ImVec2(4, 4), xCenter + ImVec2(4, 4), GuiTheme::TextPrimary, 1.5f);
			draw->AddLine(xCenter - ImVec2(4, -4), xCenter + ImVec2(4, -4), GuiTheme::TextPrimary, 1.5f);

			actionC.cursorPos.y += rowHeight;

			// Same reasoning as the Friends list: bail out for this frame rather than
			// render a now-stale list after either click shifts/removes an entry.
			if (removeHovered && ImGui::IsMouseClicked(0)) {
				DeleteConfigByName(name.c_str());
				break;
			}
			if (nameHovered && !isActive && ImGui::IsMouseClicked(0)) {
				SwitchToConfig(name.c_str());
				break;
			}
		}

		if ((size_t)shown < g_configNames.size()) {
			char moreBuf[64];
			sprintf_s(moreBuf, "+ %zu more", g_configNames.size() - shown);
			this->Text(draw, actionC, moreBuf, GuiTheme::TextMuted, false);
		}
	}

	if (this->Button(draw, actionC, "Unload", GuiTheme::Danger)) {
		RequestUnloadAction();
	}
}

void Gui::DrawFriendsTab(ImDrawList* draw, float contentX, float contentY, float contentW, float contentH) {
	float spacing = 20.0f;
	float childW = (contentW - spacing) / 2.0f;

	auto settingsC = this->Child(draw, ImVec2(contentX, contentY), ImVec2(childW, contentH), "Friend List");
	this->Checkbox(draw, settingsC, &g_friendListEnabled, "Enabled");
	this->KeyBindButton(draw, settingsC, &g_addFriendKey, &g_awaitingKeybindTarget, "Add Friend Key", GetKeyName);
	this->Divider(draw, settingsC);
	this->Text(draw, settingsC, "Aim at a player and press the", GuiTheme::TextMuted, false);
	this->Text(draw, settingsC, "Add Friend key to exclude them", GuiTheme::TextMuted, false);
	this->Text(draw, settingsC, "from Aimbot.", GuiTheme::TextMuted, false);
	this->Divider(draw, settingsC);
	char countBuf[48];
	sprintf_s(countBuf, "%zu friend(s) saved", g_friendNames.size());
	this->Text(draw, settingsC, countBuf, GuiTheme::TextPrimary, false);

	auto listC = this->Child(draw, ImVec2(contentX + childW + spacing, contentY), ImVec2(childW, contentH), "Saved Friends");
	if (g_friendNames.empty()) {
		this->Text(draw, listC, "No friends added yet.", GuiTheme::TextMuted, false);
	} else {
		const float rowHeight = 24.0f;
		const float removeSize = 18.0f;
		int maxRows = (int)(listC.contentSize.y / rowHeight);
		int shown = (int)min((size_t)maxRows, g_friendNames.size());

		for (int i = 0; i < shown; i++) {
			const string& name = g_friendNames[i];
			ImVec2 rowPos = listC.cursorPos;

			this->Text(draw, ImVec2(rowPos.x, rowPos.y), name.c_str(), GuiTheme::TextPrimary, true);

			ImVec2 removeMin(rowPos.x + listC.contentSize.x - removeSize, rowPos.y - 2.0f);
			ImVec2 removeMax(removeMin.x + removeSize, removeMin.y + removeSize);
			ImVec2 mousePos = ImGui::GetIO().MousePos;
			bool hovered = mousePos.x >= removeMin.x && mousePos.x <= removeMax.x && mousePos.y >= removeMin.y && mousePos.y <= removeMax.y;
			draw->AddRectFilled(removeMin, removeMax, hovered ? GuiTheme::Danger : GuiTheme::TrackOff, 4.0f);
			ImVec2 xCenter((removeMin.x + removeMax.x) / 2.0f, (removeMin.y + removeMax.y) / 2.0f);
			draw->AddLine(xCenter - ImVec2(4, 4), xCenter + ImVec2(4, 4), GuiTheme::TextPrimary, 1.5f);
			draw->AddLine(xCenter - ImVec2(4, -4), xCenter + ImVec2(4, -4), GuiTheme::TextPrimary, 1.5f);

			listC.cursorPos.y += rowHeight;

			// Erasing mid-loop shifts every later index - bail out for this frame instead
			// of rendering a now-stale list; it redraws correctly fresh next frame.
			if (hovered && ImGui::IsMouseClicked(0)) {
				RemoveFriendName(name);
				break;
			}
		}

		if ((size_t)shown < g_friendNames.size()) {
			char moreBuf[48];
			sprintf_s(moreBuf, "+ %zu more (edit friends.txt)", g_friendNames.size() - shown);
			this->Text(draw, listC, moreBuf, GuiTheme::TextMuted, false);
		}
	}
}
