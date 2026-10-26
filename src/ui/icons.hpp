#pragma once

#include <cstdint>

// Interface icons. Which font draws them is decided at startup by
// fonts::Load, because the preferred one isn't part of the repository:
//
//   1. Anthropicons (assets/fonts/Anthropicons-Variable.ttf, optional, not
//      redistributable — see assets/fonts/README.md). Codepoints come from the
//      claude.ai UI.
//   2. Codicons from the system JetBrains Mono Nerd Font, which the terminal
//      needs anyway.
//   3. No icon font: icon-only buttons fall back to a text symbol ("+", "⟳",
//      "…"), labels drop the icon.
//
// Anthropicons caveat: a few glyphs build their static shape from two
// *overlapping* same-winding contours (e.g. a "+" as two crossed bars).
// stb_truetype — ImGui's rasterizer here, no FreeType backend — doesn't
// composite that correctly and renders garbage (U+E001's "+" came out looking
// like "Ę", U+E11D's refresh arrow like a sparkle). Before swapping a
// codepoint, dump its glyf contours with fontTools and reject any two contours
// whose bounding boxes overlap without one nesting inside the other.
namespace ui::icons
{
enum class Icon : std::uint8_t
{
    NewChat,
    Sessions,
    Settings,
    Copy,
    Close,
    Search,
    Retry,
    More,
    Folder,
    Exit,
    Count_
};

enum class Set : std::uint8_t
{
    Anthropicons,
    Codicons,
    Text,
};

// Called by fonts::Load once it knows which icon font made it into the atlas.
void Use(Set set);
Set  Current();

// The glyph alone, for icon-only buttons: never empty (a text stand-in when
// there is no icon font).
const char* Glyph(Icon icon);
// For a menu item's icon column: "" when there is no icon font.
const char* MenuGlyph(Icon icon);
// "<glyph>  text", or just "text" without an icon font. `text` must be a
// string literal (or otherwise outlive the program) — results are cached by
// its address. ImGui "##"/"###" suffixes pass through untouched.
const char* Label(Icon icon, const char* text);
} // namespace ui::icons
