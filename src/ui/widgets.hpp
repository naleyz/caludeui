#pragma once

#include <imgui.h>

// Small controls shared by the redesigned screens (statistics, settings).
namespace ui::widgets
{
// Surface colours of the redesign.
inline constexpr ImU32 kCardBg   = IM_COL32(0x1a, 0x1a, 0x18, 255);
inline constexpr ImU32 kLine     = IM_COL32(0x2b, 0x2a, 0x27, 255);
inline constexpr ImU32 kMuted    = IM_COL32(0x8f, 0x8d, 0x85, 255);
inline constexpr ImU32 kOutline  = IM_COL32(0x3a, 0x39, 0x35, 255);

// iOS-style on/off switch at the cursor. Returns true when toggled (and flips *v).
bool Toggle(const char* id, bool* v);
inline float ToggleWidth() { return ImGui::GetFrameHeight() * 1.75f; }

// A row of mutually exclusive buttons on a dark track. Returns true when *value changed.
bool Segmented(const char* id, const char* const* labels, int count, int* value);
float SegmentedWidth(const char* const* labels, int count);

// Rounded tag ("задано"); an item, so a tooltip can follow.
void Pill(const char* text, ImU32 bg, ImU32 fg);

// Full-width message strip (notice or error).
void Banner(const char* text, ImU32 bg, ImU32 fg);

// A bordered, rounded card that grows to fit its contents. `title` may be null.
// Width 0 = all available. Everything between Begin and End is the card body.
void CardBegin(const char* id, const char* title, float width = 0.0f, const char* subtitle = nullptr);
void CardEnd();
// Hairline across the whole card at the cursor (between rows).
void CardDivider();

// Muted second-level text in a smaller size.
void SmallMuted(const char* text);
} // namespace ui::widgets
