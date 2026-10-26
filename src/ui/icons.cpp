#include "ui/icons.hpp"

#include <array>
#include <map>
#include <string>
#include <utility>

namespace ui::icons
{
namespace
{
struct Glyphs
{
    const char* anthropicons;
    const char* codicons; // Nerd Font "nf-cod-*"
    const char* text;     // stand-in for icon-only buttons; "" = drop it
};

constexpr std::array<Glyphs, static_cast<std::size_t>(Icon::Count_)> kGlyphs{{
    {"\xee\x80\xb3", "\xee\xa9\xa0", "+"},       // NewChat:  U+E033 new chat      | U+EA60 add
    {"\xee\x82\x89", "\xee\xaa\x82", ""},        // Sessions: U+E089 recents       | U+EA82 history
    {"\xee\x83\x96", "\xee\xab\xb8", ""},        // Settings: U+E0D6 settings      | U+EAF8 gear
    {"\xee\x81\x96", "\xee\xaf\x8c", ""},        // Copy:     U+E056 copy          | U+EBCC copy
    {"\xee\x84\x8f", "\xee\xa9\xb6", "\xc3\x97"},// Close:    U+E10F dismiss       | U+EA76 close   | ×
    {"\xee\x83\x93", "\xee\xa9\xad", ""},        // Search:   U+E0D3 search        | U+EA6D search
    {"\xee\x83\xa5", "\xee\xac\xb7", "\xe2\x9f\xb3"}, // Retry: U+E0E5 retry arc   | U+EB37 refresh | ⟳
    {"\xee\x81\xa2", "\xee\xa9\xbc", "\xe2\x80\xa6"}, // More:  U+E062 more        | U+EA7C ellipsis| …
    {"\xee\x83\x89", "\xee\xaa\x83", ""},        // Folder:   U+E0C9 projects      | U+EA83 folder
    {"\xee\x82\xa4", "\xee\xa9\xae", ""},        // Exit:     U+E0A4 log out       | U+EA6E sign out
}};

Set gSet = Set::Text;
// Keyed by the caller's literal, so each call site gets one stable string.
std::map<std::pair<Icon, const char*>, std::string> gLabels;

const char* IconFontGlyph(Icon icon)
{
    const Glyphs& g = kGlyphs[static_cast<std::size_t>(icon)];
    switch (gSet)
    {
    case Set::Anthropicons: return g.anthropicons;
    case Set::Codicons:     return g.codicons;
    case Set::Text:         break;
    }
    return "";
}
} // namespace

void Use(Set set)
{
    if (set == gSet) return;
    gSet = set;
    gLabels.clear();
}

Set Current()
{
    return gSet;
}

const char* Glyph(Icon icon)
{
    if (gSet == Set::Text)
    {
        const char* t = kGlyphs[static_cast<std::size_t>(icon)].text;
        return *t ? t : "\xc2\xb7"; // · — something clickable rather than an empty button
    }
    return IconFontGlyph(icon);
}

const char* MenuGlyph(Icon icon)
{
    return IconFontGlyph(icon);
}

const char* Label(Icon icon, const char* text)
{
    if (gSet == Set::Text) return text;
    auto [it, added] = gLabels.try_emplace({icon, text});
    if (added) it->second = std::string(IconFontGlyph(icon)) + "  " + text;
    return it->second.c_str();
}
} // namespace ui::icons
