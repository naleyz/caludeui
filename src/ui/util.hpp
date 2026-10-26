#pragma once

#include <imgui.h>

#include <filesystem>
#include <string>

namespace ui
{
// Single-quotes a string for use as one POSIX shell word.
std::string ShellQuote(const std::string& s);

// Opens a folder or file with the desktop's default handler (xdg-open), detached.
void OpenWithDesktop(const std::filesystem::path& path);

// "/home/me/x" → "~/x"
std::string TildePath(const std::string& path);

// "2026-09-03T15:23:42.338Z" → "15:23" today, "вчера 15:23", "3 сен 15:23", "3 сен 2025"
std::string HumanTime(const std::string& isoUtc);

std::string HumanSize(std::uintmax_t bytes);

// Small coloured status dot at the current cursor position, then SameLine.
void StatusDot(ImU32 color, float radius = 4.0f);

// Small spinning arc at the current cursor position, then SameLine. Animates
// off ImGui::GetTime(), so it needs no per-frame state of its own.
void Spinner(ImU32 color, float radius = 5.0f, float thickness = 2.0f);

// Same spinner, centred at an arbitrary screen position (e.g. over a blank
// canvas) instead of advancing the cursor.
void SpinnerAt(ImDrawList* dl, ImVec2 centre, ImU32 color, float radius, float thickness);

// Gray "(?)" with a wrapped tooltip.
void HelpMarker(const char* text);

// A small non-interactive banner pinned to the top-right of the main
// viewport, for a transient alert the user should notice without it
// stealing focus. Caller owns the timing (draw every frame it should be
// visible, stop calling it once it should disappear).
void DrawToast(const char* text, ImU32 accent);
} // namespace ui
