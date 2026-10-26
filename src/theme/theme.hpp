#pragma once

#include <imgui.h>

// Colors from the project's CLAUDE.md palette.
namespace theme::palette
{
inline constexpr ImVec4 Dark      {0.0784f, 0.0784f, 0.0745f, 1.0f}; // #141413
inline constexpr ImVec4 Light     {0.9804f, 0.9765f, 0.9608f, 1.0f}; // #faf9f5
inline constexpr ImVec4 MidGray   {0.6902f, 0.6824f, 0.6471f, 1.0f}; // #b0aea5
inline constexpr ImVec4 LightGray {0.9098f, 0.9020f, 0.8627f, 1.0f}; // #e8e6dc
inline constexpr ImVec4 Orange    {0.8510f, 0.4667f, 0.3412f, 1.0f}; // #d97757 (primary accent)
inline constexpr ImVec4 Blue      {0.4157f, 0.6078f, 0.8000f, 1.0f}; // #6a9bcc
inline constexpr ImVec4 Green     {0.4706f, 0.5490f, 0.3647f, 1.0f}; // #788c5d
} // namespace theme::palette

namespace theme
{
enum class Mode { Dark, Light };

// Applies the Claude palette to the current ImGui style/context.
void Apply(Mode mode);
} // namespace theme
