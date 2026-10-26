#pragma once

#include "config/app_settings.hpp"

#include <string>

struct ImFont;

namespace ui::fonts
{
// Interface text: Anthropic Sans for Latin, Anthropicons for icons, Noto Sans
// for Cyrillic (the Anthropic faces carry Latin only), then symbol fallbacks.
inline ImFont* Ui = nullptr;
// Terminal grid and anything that must line up in columns: JetBrains Mono
// unless the user picked another face (Settings → ClaudeUI). Replaced at
// runtime by SetTerminalFont, so don't cache these across frames.
inline ImFont* Mono     = nullptr;
inline ImFont* MonoBold = nullptr;
// Size to push with Mono, in pixels with the DPI scale applied.
inline float TerminalSize = config::TerminalFont::kDefaultSize;
// Anthropic Serif, Latin only — for the wordmark.
inline ImFont* Serif = nullptr;

// Builds the atlas sources. `contentScale` is the window's DPI scale.
// Missing files are skipped; anything unavailable falls back to Mono, then
// to ImGui's built-in font. A terminal font that can't be used falls back to
// the built-in one (the reason is in TerminalFontError()).
void Load(float contentScale, const config::TerminalFont& terminal);

// Switches the terminal face/size while running. Only between frames — before
// NewFrame — because the atlas is locked during one. Returns false and keeps
// the current face when the font can't be used (missing file, not a font, not
// monospace); TerminalFontError() then says why.
bool SetTerminalFont(const config::TerminalFont& pref);
// Live size while a slider is dragged: nothing to rebuild, safe mid-frame.
void PreviewTerminalSize(float logicalPx);
const std::string& TerminalFontError();

// Path of a font shipped in assets/fonts, "" if the assets directory isn't found.
std::string BundledFontPath(const char* file);
} // namespace ui::fonts
