#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace config
{
// One installed monospace family, with the faces the terminal can use.
struct MonoFontFamily
{
    std::string family;
    std::string regular; // .ttf/.otf path
    std::string bold;    // "" when the family has no separate bold face
};

// Installed monospace families (via fontconfig), sorted by name. Only files
// ImGui's rasteriser can load (.ttf/.otf, upright); bitmap and Type 1 fonts and
// italics are skipped. Takes a few ms (`fc-list`), so callers may do it inline.
std::vector<MonoFontFamily> ScanMonospaceFonts();

// "…-Regular.ttf" → "…-Bold.ttf" when that file exists, "" otherwise. Used to
// give a font picked by path its bold face.
std::string GuessBoldSibling(const std::string& regularPath);

// A TrueType/OpenType header check, so a wrong file is refused with a message
// instead of tripping an assertion inside the font loader.
bool LooksLikeFontFile(const std::filesystem::path& path, std::string* error);
} // namespace config
