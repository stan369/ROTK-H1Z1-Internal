#pragma once
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx11.h"
#include "imgui/imgui_internal.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <cmath>
#include <d3d11.h>

// ImGui deliberately comments this declaration out in imgui_impl_win32.h (to avoid
// dragging a <windows.h> dependency into that header for non-Win32 users) and expects
// callers to copy it in themselves - this is that copy, used by hkWndProc in Lol2.cpp.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ImGui-based replacement for the old hand-rolled text menu (DrawMenu()/MenuItem in
// Lol2.cpp). Styling helpers adapted from a found ImGui menu skin ("Archetype CS2"),
// then given its own visual pass below (theme colors, panel shadows/borders, sidebar
// icons, gradient fills) so it reads as a single deliberate design rather than the
// original skin's placeholder look. Only Init() changed structurally (takes the game's
// own device/context/hwnd directly instead of owning a separate Renderer/window, since
// this runs inside the game's own Present hook, not a standalone app) and the tab
// content, which is wired to real cheat state in gui.cpp instead of placeholder demo
// widgets.

// Centralized palette so every widget pulls from the same handful of colors instead of
// scattering literal RGB triples through the draw calls - makes the whole menu read as
// one coherent theme and means a future palette swap only touches this block.
namespace GuiTheme {
	static const ImColor Background = ImColor(15, 16, 21, 255);
	static const ImColor BackgroundEdge = ImColor(22, 20, 30, 255);
	static const ImColor TitleBar = ImColor(21, 22, 29, 255);
	static const ImColor Sidebar = ImColor(18, 19, 25, 255);
	static const ImColor Panel = ImColor(23, 25, 33, 255);
	static const ImColor PanelHeader = ImColor(29, 31, 40, 255);
	static const ImColor PanelBorder = ImColor(255, 255, 255, 16);
	static const ImColor Shadow = ImColor(0, 0, 0, 90);

	static const ImColor AccentA = ImColor(56, 189, 248, 255);   // sky blue
	static const ImColor AccentB = ImColor(167, 139, 250, 255);  // violet
	static const ImColor AccentDim = ImColor(56, 189, 248, 60);

	static const ImColor TextPrimary = ImColor(235, 238, 245, 255);
	static const ImColor TextMuted = ImColor(145, 150, 165, 255);
	static const ImColor TextLocked = ImColor(190, 130, 130, 255);

	static const ImColor TrackOff = ImColor(38, 39, 48, 255);
	static const ImColor TrackLocked = ImColor(48, 32, 32, 255);
	static const ImColor KnobLocked = ImColor(170, 110, 110, 255);

	static const ImColor Success = ImColor(110, 231, 150, 255);
	static const ImColor Danger = ImColor(248, 113, 113, 255);
}

struct AnimFloat {
	float value = 0.0f;
	float speed = 8.0f;

	float AnimateTo(float target, float deltaTime) {
		this->value += (target - this->value) * (1.0f - expf(-this->speed * deltaTime));
		return this->value;
	}
};

struct ChildRegion {
	ImVec2 pos;
	ImVec2 size;
	ImVec2 contentPos;
	ImVec2 contentSize;
	ImVec2 cursorPos;
};

class Gui {
public:
	void Init(HWND hWnd, ID3D11Device* device, ID3D11DeviceContext* context);
	void Shutdown();
	void Render(int width, int height);

private:
	float titleBarHeight = 34.0f;
	float sidebarWidth = 160.0f;
	static inline const char* tabs[] = { "Combat", "ESP", "Settings", "Friends" };
	static inline int activeTab = 0;
	float tabHeight = 30.0f;
	float tabSpacing = 6.0f;
	float tabWidth = sidebarWidth - 20.0f;
	float cursorSpacing = 8.0f;

	void DrawMainWindow(int width, int height);
	void DrawCombatTab(ImDrawList* draw, float contentX, float contentY, float contentW, float contentH);
	void DrawEspTab(ImDrawList* draw, float contentX, float contentY, float contentW, float contentH);
	void DrawSettingsTab(ImDrawList* draw, float contentX, float contentY, float contentW, float contentH);
	void DrawFriendsTab(ImDrawList* draw, float contentX, float contentY, float contentW, float contentH);

	void HandleMenuDragging(ImVec2 pos, ImVec2 size) const {
		static bool dragging = false;
		static ImVec2 dragOffset = ImVec2(0, 0);
		ImVec2 titleBarMin = pos;
		ImVec2 titleBarMax = ImVec2(pos.x + size.x, pos.y + this->titleBarHeight);

		ImVec2 mousePos = ImGui::GetIO().MousePos;
		bool hovered = mousePos.x >= titleBarMin.x && mousePos.x <= titleBarMax.x && mousePos.y >= titleBarMin.y && mousePos.y <= titleBarMax.y;
		if (hovered && ImGui::IsMouseClicked(0)) {
			dragging = true;
			dragOffset = ImVec2(mousePos.x - pos.x, mousePos.y - pos.y);
		}

		if (!ImGui::IsMouseDown(0)) {
			dragging = false;
		}

		if (dragging) {
			ImVec2 newPos = ImVec2(mousePos.x - dragOffset.x, mousePos.y - dragOffset.y);
			ImGui::SetWindowPos(newPos);
		}
	}

	// A few layered, shrinking-alpha rects offset below/right of a panel fake a soft drop
	// shadow without needing a blur shader - cheap, and enough to lift panels off the
	// background at this menu's scale.
	void PanelShadow(ImDrawList* draw, ImVec2 pos, ImVec2 size, float rounding) const {
		const int layers = 4;
		for (int i = layers; i >= 1; i--) {
			float spread = (float)i * 1.5f;
			int alpha = 22 - i * 4;
			if (alpha <= 0) continue;
			draw->AddRectFilled(
				ImVec2(pos.x - spread * 0.3f, pos.y - spread * 0.2f + 3.0f),
				ImVec2(pos.x + size.x + spread * 0.3f, pos.y + size.y + spread * 0.6f),
				ImColor(0, 0, 0, alpha), rounding + spread * 0.3f);
		}
	}

	void Text(ImDrawList* draw, ChildRegion& region, const char* label, ImColor color, bool outline) const {
		ImVec2 textSize = ImGui::CalcTextSize(label);

		if (outline) {
			draw->AddText(ImGui::GetFont(), 15.0f, region.cursorPos + ImVec2(-1, 0), ImColor(0, 0, 0, 255), label);
			draw->AddText(ImGui::GetFont(), 15.0f, region.cursorPos + ImVec2(1, 0), ImColor(0, 0, 0, 255), label);
			draw->AddText(ImGui::GetFont(), 15.0f, region.cursorPos + ImVec2(0, -1), ImColor(0, 0, 0, 255), label);
			draw->AddText(ImGui::GetFont(), 15.0f, region.cursorPos + ImVec2(0, 1), ImColor(0, 0, 0, 255), label);
		}

		draw->AddText(ImGui::GetFont(), 15.0f, region.cursorPos, color, label);

		region.cursorPos.y += textSize.y + this->cursorSpacing;
	}

	void Text(ImDrawList* draw, ImVec2 pos, const char* label, ImColor color, bool outline) const {
		if (outline) {
			draw->AddText(ImGui::GetFont(), 15.0f, pos + ImVec2(-1, 0), ImColor(0, 0, 0, 255), label);
			draw->AddText(ImGui::GetFont(), 15.0f, pos + ImVec2(1, 0), ImColor(0, 0, 0, 255), label);
			draw->AddText(ImGui::GetFont(), 15.0f, pos + ImVec2(0, -1), ImColor(0, 0, 0, 255), label);
			draw->AddText(ImGui::GetFont(), 15.0f, pos + ImVec2(0, 1), ImColor(0, 0, 0, 255), label);
		}

		draw->AddText(ImGui::GetFont(), 15.0f, pos, color, label);
	}

	// Thin muted rule to separate logically distinct groups of controls within one panel
	// (e.g. the aimbot toggle/keybinds from its FOV/smoothness sliders) without needing a
	// whole new child panel for each group.
	void Divider(ImDrawList* draw, ChildRegion& region) const {
		float width = region.contentSize.x;
		draw->AddLine(region.cursorPos, ImVec2(region.cursorPos.x + width, region.cursorPos.y), GuiTheme::PanelBorder, 1.0f);
		region.cursorPos.y += this->cursorSpacing;
	}

	ChildRegion Child(ImDrawList* draw, ImVec2 pos, ImVec2 size, const char* title) {
		float headerHeight = 30.0f;
		float rounding = 8.0f;

		this->PanelShadow(draw, pos, size, rounding);

		draw->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), GuiTheme::Panel, rounding);
		draw->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), GuiTheme::PanelBorder, rounding, 0, 1.0f);

		// Header starts as the flat panel-header color and only picks up the accent
		// gradient as a translucent seam near its bottom edge. ShadeVertsLinearColor-
		// GradientKeepAlpha only overwrites RGB and keeps whatever alpha the vertices
		// already had, so the initial fill's alpha (40, not 255) is what makes this seam
		// translucent instead of a loud solid bar like the title bar/active-tab gradients.
		draw->AddRectFilled(ImVec2(pos.x, pos.y), ImVec2(pos.x + size.x, pos.y + headerHeight), GuiTheme::PanelHeader, rounding, ImDrawFlags_RoundCornersTop);
		int vtxStart = draw->VtxBuffer.Size;
		draw->AddRectFilled(ImVec2(pos.x, pos.y + headerHeight - 10), ImVec2(pos.x + size.x, pos.y + headerHeight), ImColor(255, 255, 255, 40), 0);
		int vtxEnd = draw->VtxBuffer.Size;
		ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw, vtxStart, vtxEnd, ImVec2(pos.x, pos.y + headerHeight - 10), ImVec2(pos.x + size.x, pos.y + headerHeight - 10), ImColor(56, 189, 248, 255), ImColor(167, 139, 250, 255));
		draw->AddLine(ImVec2(pos.x + 4, pos.y + headerHeight), ImVec2(pos.x + size.x - 4, pos.y + headerHeight), GuiTheme::PanelBorder, 1.0f);

		// small accent tick to the left of the title instead of relying on the faint
		// header tint alone to separate it from the body
		draw->AddRectFilled(ImVec2(pos.x + 10, pos.y + headerHeight / 2 - 6), ImVec2(pos.x + 13, pos.y + headerHeight / 2 + 6), GuiTheme::AccentA, 1.5f);

		ImVec2 textSize = ImGui::CalcTextSize(title);
		this->Text(draw, ImVec2(pos.x + 22, pos.y + headerHeight / 2 - textSize.y / 2), title, GuiTheme::TextPrimary, true);

		ImVec2 contentPos = ImVec2(pos.x + 10, pos.y + headerHeight + 10);
		ImVec2 contentSize = ImVec2(size.x - 20, size.y - headerHeight - 18);

		return { pos, size, contentPos, contentSize, contentPos };
	}

	// value == nullptr means "always off, disabled" - used for SafeMode-gated rows so
	// they render grayed out with a [locked] tag instead of being clickable.
	void Checkbox(ImDrawList* draw, ChildRegion& region, bool* value, const char* label, bool locked = false) const {
		static std::unordered_map<const char*, AnimFloat> anims;

		float switchWidth = 38.0f;
		float switchHeight = 16.0f;
		float knobRadius = switchHeight / 2.0f;
		ImVec2 pos = region.cursorPos;
		ImVec2 trackMin = pos;
		ImVec2 trackMax = ImVec2(pos.x + switchWidth, pos.y + switchHeight);

		ImVec2 mousePos = ImGui::GetIO().MousePos;
		bool hovered = !locked && mousePos.x >= trackMin.x && mousePos.x <= trackMax.x && mousePos.y >= trackMin.y && mousePos.y <= trackMax.y;
		bool active = hovered && ImGui::IsMouseClicked(0);
		if (active) {
			*value = !*value;
		}

		float deltaTime = ImGui::GetIO().DeltaTime;
		float anim = anims[label].AnimateTo(*value ? 1.0f : 0.0f, deltaTime);

		ImColor trackColor = locked ? GuiTheme::TrackLocked : GuiTheme::TrackOff;
		if (*value && !locked) {
			// soft glow behind the track when on, in addition to the filled track itself
			ImVec2 glowPad(3.0f, 3.0f);
			draw->AddRectFilled(trackMin - glowPad, trackMax + glowPad, ImColor(GuiTheme::AccentA.Value.x, GuiTheme::AccentA.Value.y, GuiTheme::AccentA.Value.z, 0.20f * anim), switchHeight / 2.0f + 3.0f);
			trackColor = ImColor(GuiTheme::AccentA.Value.x, GuiTheme::AccentA.Value.y, GuiTheme::AccentA.Value.z, anim);
		}

		draw->AddRectFilled(trackMin, trackMax, trackColor, switchHeight / 2.0f);

		float knobX = trackMin.x + knobRadius + anim * (switchWidth - switchHeight);
		ImVec2 knobCenter = ImVec2(knobX, trackMin.y + knobRadius);

		draw->AddCircleFilled(knobCenter, knobRadius - 1.0f, locked ? GuiTheme::KnobLocked : GuiTheme::TextPrimary);

		char labelBuf[64];
		if (locked) {
			sprintf_s(labelBuf, "%s [locked]", label);
		} else {
			strcpy_s(labelBuf, label);
		}
		this->Text(draw, ImVec2(trackMax.x + 8, trackMin.y - 1), labelBuf, locked ? GuiTheme::TextLocked : GuiTheme::TextPrimary, true);

		region.cursorPos.y += switchHeight + this->cursorSpacing;
	}

	// Color swatch that opens a full RGB picker popup on click (ImGui's own native
	// ColorPicker3 - saturation/value square, hue slider, RGB/hex inputs) instead of
	// cycling a fixed palette. PushID(label) scopes the popup/button IDs per-row so
	// multiple ColorPicker calls on the same frame don't collide/share state.
	void ColorPicker(ImDrawList* draw, ChildRegion& region, float* color, const char* label) const {
		float swatchSize = 16.0f;
		ImVec2 pos = region.cursorPos;

		this->Text(draw, pos, label, GuiTheme::TextMuted, true);

		ImVec2 swatchPos = ImVec2(pos.x + region.contentSize.x - swatchSize, pos.y - 2);

		draw->AddRect(swatchPos - ImVec2(2, 2), swatchPos + ImVec2(swatchSize + 2, swatchSize + 2), GuiTheme::PanelBorder, 4.0f, 0, 1.0f);

		ImGui::PushID(label);
		ImGui::SetCursorScreenPos(swatchPos);
		if (ImGui::ColorButton("##swatch", ImVec4(color[0], color[1], color[2], 1.0f),
			ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder, ImVec2(swatchSize, swatchSize))) {
			ImGui::OpenPopup("##colorpicker");
		}
		if (ImGui::BeginPopup("##colorpicker")) {
			ImGui::ColorPicker3("##picker", color, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoAlpha);
			ImGui::EndPopup();
		}
		ImGui::PopID();

		region.cursorPos.y += swatchSize + this->cursorSpacing;
	}

	// Click to arm *awaitingTarget = target, then the actual key capture (listening for
	// the next keypress) happens outside ImGui, in the existing hkPresent input block in
	// Lol2.cpp - this button just shows current state and starts/stops listening.
	void KeyBindButton(ImDrawList* draw, ChildRegion& region, int* target, int** awaitingTarget,
		const char* label, const char* (*getKeyName)(int)) const {
		float boxHeight = 22.0f;
		float width = region.contentSize.x;
		ImVec2 boxMin = region.cursorPos + ImVec2(0, 18);
		ImVec2 boxMax = ImVec2(boxMin.x + width, boxMin.y + boxHeight);

		bool awaiting = (*awaitingTarget == target);

		ImVec2 mousePos = ImGui::GetIO().MousePos;
		bool hovered = mousePos.x >= boxMin.x && mousePos.x <= boxMax.x && mousePos.y >= boxMin.y && mousePos.y <= boxMax.y;
		if (hovered && ImGui::IsMouseClicked(0)) {
			*awaitingTarget = awaiting ? nullptr : target;
		}

		this->Text(draw, region.cursorPos, label, GuiTheme::TextMuted, true);
		draw->AddRectFilled(boxMin, boxMax, awaiting ? GuiTheme::AccentA : GuiTheme::TrackOff, 4.0f);
		if (!awaiting) draw->AddRect(boxMin, boxMax, GuiTheme::PanelBorder, 4.0f, 0, 1.0f);

		const char* text = awaiting ? "[press a key]" : getKeyName(*target);
		ImVec2 textSize = ImGui::CalcTextSize(text);
		this->Text(draw, ImVec2(boxMin.x + width / 2.0f - textSize.x / 2.0f, boxMin.y + boxHeight / 2.0f - textSize.y / 2.0f), text, GuiTheme::TextPrimary, true);

		region.cursorPos.y += boxHeight + 18.0f + this->cursorSpacing;
	}

	void SliderFloat(ImDrawList* draw, ChildRegion& region, float* value, float min, float max, const char* label) const {
		static std::unordered_map<const char*, AnimFloat> anims;

		float width = region.contentSize.x;
		float height = 6.0f;

		ImVec2 barPos = region.cursorPos + ImVec2(0, 20);
		ImVec2 barSize(width, height);

		ImVec2 mousePos = ImGui::GetIO().MousePos;
		bool hovered = mousePos.x >= (barPos - ImVec2(0, 6)).x && mousePos.x <= (barPos - ImVec2(0, 6)).x + (barSize + ImVec2(0, 12)).x && mousePos.y >= (barPos - ImVec2(0, 6)).y && mousePos.y <= (barPos - ImVec2(0, 6)).y + (barSize + ImVec2(0, 12)).y;
		bool active = hovered && ImGui::IsMouseDown(0);
		if (active) {
			float mouseX = ImGui::GetIO().MousePos.x;
			float t = (mouseX - barPos.x) / barSize.x;
			t = ImClamp(t, 0.0f, 1.0f);
			*value = min + t * (max - min);
		}

		float normalized = (*value - min) / (max - min);
		float deltaTime = ImGui::GetIO().DeltaTime;
		float animFill = anims[label].AnimateTo(normalized, deltaTime);

		draw->AddRectFilled(barPos, barPos + barSize, GuiTheme::TrackOff, 3.0f);

		float fillWidth = barSize.x * animFill;
		if (fillWidth > 0.5f) {
			int vtxStart = draw->VtxBuffer.Size;
			draw->AddRectFilled(barPos, ImVec2(barPos.x + fillWidth, barPos.y + barSize.y), GuiTheme::AccentA, 3.0f);
			int vtxEnd = draw->VtxBuffer.Size;
			ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw, vtxStart, vtxEnd, barPos, ImVec2(barPos.x + fillWidth, barPos.y), GuiTheme::AccentA, GuiTheme::AccentB);
		}

		float knobX = barPos.x + fillWidth;
		draw->AddCircleFilled(ImVec2(knobX, barPos.y + barSize.y / 2), 7.0f, GuiTheme::TextPrimary);
		draw->AddCircle(ImVec2(knobX, barPos.y + barSize.y / 2), 7.0f, GuiTheme::AccentA, 0, 1.5f);

		char valueText[32];
		snprintf(valueText, sizeof(valueText), "%.f", *value);

		ImVec2 textSize = ImGui::CalcTextSize(valueText);
		this->Text(draw, region.cursorPos, label, GuiTheme::TextMuted, true);
		this->Text(draw, ImVec2(barPos.x + width - textSize.x, region.cursorPos.y), valueText, GuiTheme::AccentA, true);

		region.cursorPos.y += 25.0f + this->cursorSpacing;
	}

	// Half-width drag-to-set slider with no glow animation and its value shown as a label
	// above the bar rather than beside it - about half of SliderFloat's vertical footprint,
	// used by WeaponProfileRow below so all of a weapon's FOV+Smoothness controls (and every
	// weapon in the list) fit in one panel without needing to scroll.
	void MiniSlider(ImDrawList* draw, ImVec2 pos, float width, float* value, float min, float max, const char* label) const {
		float height = 5.0f;
		ImVec2 barPos = pos;
		ImVec2 barSize(width, height);

		ImVec2 mousePos = ImGui::GetIO().MousePos;
		bool hovered = mousePos.x >= barPos.x && mousePos.x <= barPos.x + width && mousePos.y >= barPos.y - 8.0f && mousePos.y <= barPos.y + height + 8.0f;
		bool active = hovered && ImGui::IsMouseDown(0);
		if (active) {
			float t = (mousePos.x - barPos.x) / width;
			t = ImClamp(t, 0.0f, 1.0f);
			*value = min + t * (max - min);
		}

		float normalized = (*value - min) / (max - min);
		draw->AddRectFilled(barPos, barPos + barSize, GuiTheme::TrackOff, height / 2.0f);
		float fillWidth = width * normalized;
		if (fillWidth > 0.5f) {
			draw->AddRectFilled(barPos, ImVec2(barPos.x + fillWidth, barPos.y + height), GuiTheme::AccentA, height / 2.0f);
		}
		draw->AddCircleFilled(ImVec2(barPos.x + fillWidth, barPos.y + height / 2.0f), 4.5f, GuiTheme::TextPrimary);

		char valueText[32];
		snprintf(valueText, sizeof(valueText), "%s %.1f", label, *value);
		this->Text(draw, ImVec2(barPos.x, barPos.y - 14.0f), valueText, GuiTheme::TextMuted, false);
	}

	// One row per weapon in the Weapon Profiles panel: name, then its FOV and Smoothness
	// as side-by-side MiniSliders so all ~11 weapons fit in the Combat tab's Aimbot panel
	// without scrolling.
	void WeaponProfileRow(ImDrawList* draw, ChildRegion& region, const char* name, float* fov, float* smoothness) const {
		this->Text(draw, region.cursorPos, name, GuiTheme::TextPrimary, true);
		region.cursorPos.y += 15.0f;

		float gap = 14.0f;
		float halfW = (region.contentSize.x - gap) / 2.0f;
		ImVec2 sliderPos = region.cursorPos + ImVec2(0, 14.0f);  // room for the value label above the bar
		this->MiniSlider(draw, sliderPos, halfW, fov, 5.0f, 150.0f, "FOV");
		this->MiniSlider(draw, ImVec2(sliderPos.x + halfW + gap, sliderPos.y), halfW, smoothness, 1.0f, 15.0f, "SM");

		region.cursorPos.y += 14.0f + 5.0f + 8.0f;
	}

	// Simple full-width clickable button (Save Config / Unload).
	bool Button(ImDrawList* draw, ChildRegion& region, const char* label, ImColor color) const {
		float boxHeight = 28.0f;
		float width = region.contentSize.x;
		ImVec2 boxMin = region.cursorPos;
		ImVec2 boxMax = ImVec2(boxMin.x + width, boxMin.y + boxHeight);

		ImVec2 mousePos = ImGui::GetIO().MousePos;
		bool hovered = mousePos.x >= boxMin.x && mousePos.x <= boxMax.x && mousePos.y >= boxMin.y && mousePos.y <= boxMax.y;
		bool clicked = hovered && ImGui::IsMouseClicked(0);

		ImColor c = hovered
			? ImColor(ImMin(color.Value.x * 1.25f, 1.0f), ImMin(color.Value.y * 1.25f, 1.0f), ImMin(color.Value.z * 1.25f, 1.0f), 1.0f)
			: color;
		draw->AddRectFilled(boxMin, boxMax, c, 5.0f);
		// thin brighter strip along the top edge for a subtle glossy highlight
		draw->AddRectFilled(boxMin, ImVec2(boxMax.x, boxMin.y + 2.0f), ImColor(255, 255, 255, 35), 5.0f, ImDrawFlags_RoundCornersTop);

		ImVec2 textSize = ImGui::CalcTextSize(label);
		this->Text(draw, ImVec2(boxMin.x + width / 2.0f - textSize.x / 2.0f, boxMin.y + boxHeight / 2.0f - textSize.y / 2.0f), label, GuiTheme::TextPrimary, true);

		region.cursorPos.y += boxHeight + this->cursorSpacing;
		return clicked;
	}

	// Editable text field, backed by ImGui's own native InputText - unlike every other
	// widget here (all hand-drawn), real keyboard text entry (cursor, selection, IME) is
	// impractical to reimplement by hand, and the window this renders into is a real
	// ImGui window underneath the custom draw-list styling, so a native widget can be
	// dropped in directly. Used for the "new config name" field (see DrawSettingsTab).
	void TextInput(ImDrawList* draw, ChildRegion& region, char* buf, size_t bufSize, const char* label) const {
		float height = 24.0f;
		ImVec2 pos = region.cursorPos;
		this->Text(draw, pos, label, GuiTheme::TextMuted, true);

		ImVec2 boxMin = pos + ImVec2(0, 18.0f);
		ImVec2 boxMax = ImVec2(boxMin.x + region.contentSize.x, boxMin.y + height);
		draw->AddRectFilled(boxMin, boxMax, GuiTheme::TrackOff, 4.0f);
		draw->AddRect(boxMin, boxMax, GuiTheme::PanelBorder, 4.0f, 0, 1.0f);

		ImGui::PushID(label);
		ImGui::SetCursorScreenPos(boxMin + ImVec2(6.0f, 3.0f));
		ImGui::PushItemWidth(region.contentSize.x - 12.0f);
		ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(GuiTheme::TextPrimary.Value.x, GuiTheme::TextPrimary.Value.y, GuiTheme::TextPrimary.Value.z, 1.0f));
		ImGui::InputText("##input", buf, bufSize);
		ImGui::PopStyleColor(4);
		ImGui::PopItemWidth();
		ImGui::PopID();

		region.cursorPos.y += 18.0f + height + this->cursorSpacing;
	}

	// Small geometric glyphs for the sidebar tabs, drawn with plain draw-list primitives
	// (no icon font/texture dependency) - crosshair, eye, bolt, gear for Combat/ESP/
	// Exploits/Settings respectively.
	void DrawTabIcon(ImDrawList* draw, ImVec2 center, int tabIndex, ImColor color) const {
		switch (tabIndex) {
		case 0: {  // Combat - crosshair. Ticks start flush against the ring (no gap) and
			// run further out than before so it doesn't read as a plain diamond/dot at
			// sidebar scale.
			draw->AddCircle(center, 5.5f, color, 0, 1.8f);
			draw->AddLine(ImVec2(center.x, center.y - 9.5f), ImVec2(center.x, center.y - 5.5f), color, 1.8f);
			draw->AddLine(ImVec2(center.x, center.y + 5.5f), ImVec2(center.x, center.y + 9.5f), color, 1.8f);
			draw->AddLine(ImVec2(center.x - 9.5f, center.y), ImVec2(center.x - 5.5f, center.y), color, 1.8f);
			draw->AddLine(ImVec2(center.x + 5.5f, center.y), ImVec2(center.x + 9.5f, center.y), color, 1.8f);
			break;
		}
		case 1: {  // ESP - eye
			const int samples = 10;
			ImVec2 pts[samples * 2];
			for (int i = 0; i < samples; i++) {
				float t = (float)i / (float)(samples - 1);
				float x = center.x - 8.5f + t * 17.0f;
				float y = center.y - sinf(t * (float)M_PI) * 5.5f;
				pts[i] = ImVec2(x, y);
			}
			for (int i = 0; i < samples; i++) {
				float t = (float)i / (float)(samples - 1);
				float x = center.x + 8.5f - t * 17.0f;
				float y = center.y + sinf(t * (float)M_PI) * 4.5f;
				pts[samples + i] = ImVec2(x, y);
			}
			draw->AddPolyline(pts, samples * 2, color, ImDrawFlags_Closed, 1.7f);
			draw->AddCircleFilled(center, 3.0f, color);
			break;
		}
		case 2: {  // Settings - gear
			draw->AddCircle(center, 5.5f, color, 0, 1.8f);
			draw->AddCircleFilled(center, 2.2f, color);
			for (int i = 0; i < 8; i++) {
				float angle = (float)i / 8.0f * 2.0f * (float)M_PI;
				ImVec2 toothCenter = ImVec2(center.x + cosf(angle) * 8.5f, center.y + sinf(angle) * 8.5f);
				draw->AddRectFilled(toothCenter - ImVec2(1.6f, 1.6f), toothCenter + ImVec2(1.6f, 1.6f), color);
			}
			break;
		}
		case 3: {  // Friend List - simple person glyph: circular head + trapezoid shoulders
			draw->AddCircleFilled(ImVec2(center.x, center.y - 3.5f), 3.3f, color);
			ImVec2 body[4] = {
				ImVec2(center.x - 6.0f, center.y + 9.0f),
				ImVec2(center.x + 6.0f, center.y + 9.0f),
				ImVec2(center.x + 4.0f, center.y + 1.0f),
				ImVec2(center.x - 4.0f, center.y + 1.0f),
			};
			draw->AddConvexPolyFilled(body, 4, color);
			break;
		}
		}
	}
};
