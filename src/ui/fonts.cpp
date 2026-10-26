#include "ui/fonts.hpp"
#include "config/font_catalog.hpp"
#include "ui/icons.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <utility>
#include <vector>

namespace ui::fonts
{
namespace
{
namespace fs = std::filesystem;

// ImGui sizes a face by its hhea ascent−descent, not its em: Anthropic Sans
// at 16 has a 12.8 px em — close to claude.ai's 14 px body text on screen.
constexpr float kUiSize = 16.0f;

// Noto Sans/Serif have a smaller x-height relative to that box (0.394 vs
// Anthropic's 0.432); scaled up, Cyrillic and Latin read as one size.
// Merged glyphs share the primary face's baseline, so no vertical fix-up.
constexpr float kNotoMatchScale = 0.432f / 0.394f;

// assets/ next to the binary, one level up (build/claudeui), or the source
// tree this binary was built from; $CLAUDEUI_ASSETS overrides.
fs::path AssetsDir()
{
    std::vector<fs::path> candidates;
    if (const char* env = std::getenv("CLAUDEUI_ASSETS"); env && *env) candidates.emplace_back(env);
    std::error_code ec;
    if (auto exe = fs::read_symlink("/proc/self/exe", ec); !ec)
    {
        candidates.push_back(exe.parent_path() / "assets");
        candidates.push_back(exe.parent_path().parent_path() / "assets");
    }
#ifdef CLAUDEUI_SOURCE_ASSETS_DIR
    candidates.emplace_back(CLAUDEUI_SOURCE_ASSETS_DIR);
#endif
    candidates.emplace_back("assets");
    for (const auto& c : candidates)
        if (fs::is_directory(c / "fonts", ec)) return c;
    return {};
}

bool Exists(const fs::path& p)
{
    std::error_code ec;
    return !p.empty() && fs::is_regular_file(p, ec);
}

float       gScale = 1.0f;         // DPI scale Load() was given
std::string gTerminalError;        // why the last SetTerminalFont failed
config::TerminalFont gApplied;     // what Mono/MonoBold currently show

const fs::path kJbRegular = "/usr/share/fonts/TTF/JetBrainsMonoNerdFontMono-Regular.ttf";
const fs::path kJbBold    = "/usr/share/fonts/TTF/JetBrainsMonoNerdFontMono-Bold.ttf";
const fs::path kJbPropo   = "/usr/share/fonts/TTF/JetBrainsMonoNerdFontPropo-Regular.ttf"; // icons only

ImFontConfig BaseConfig()
{
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    cfg.PixelSnapH  = true;
    cfg.Flags |= ImFontFlags_NoLoadError; // a bad file is reported by us, not asserted by ImGui
    return cfg;
}

// Symbols Claude Code's TUI and our labels use that the main faces lack
// (⏺ ⎿ ✳ ✻ ⏵ ◐ ⧉ ↳ ✓ …). First source that has a glyph wins.
std::vector<std::pair<fs::path, ImFontConfig>> SymbolFallbacks()
{
    std::vector<std::pair<fs::path, ImFontConfig>> symbols;
    for (const char* p : {"/usr/share/fonts/Adwaita/AdwaitaMono-Regular.ttf",
                          "/usr/share/fonts/noto/NotoSansSymbols2-Regular.ttf",
                          "/usr/share/fonts/noto/NotoSansSymbols-Regular.ttf",
                          "/usr/share/fonts/noto/NotoSansMono-Regular.ttf", "/usr/share/fonts/TTF/DejaVuSansMono.ttf"})
        symbols.emplace_back(p, BaseConfig());
    return symbols;
}

// Adds `primary` (or the first existing of `primaries`), then merges each
// existing fallback into it. Returns nullptr if no primary could be loaded.
ImFont* AddFace(ImGuiIO& io, const std::vector<fs::path>& primaries, float size,
                const std::vector<std::pair<fs::path, ImFontConfig>>& fallbacks)
{
    ImFont* font = nullptr;
    for (const auto& p : primaries)
    {
        if (!Exists(p)) continue;
        ImFontConfig cfg = BaseConfig();
        font = io.Fonts->AddFontFromFileTTF(p.c_str(), size, &cfg);
        if (font) break;
    }
    if (!font) return nullptr;
    for (const auto& [path, cfg] : fallbacks)
    {
        if (!Exists(path)) continue;
        ImFontConfig merge = cfg;
        merge.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(path.c_str(), size, &merge);
    }
    return font;
}

// A terminal grid places one glyph per cell, so every glyph must advance by the
// same amount; a proportional font would shear the columns.
bool IsMonospaced(ImFont* font, float px)
{
    ImFontBaked* baked = font->GetFontBaked(px);
    const float m = baked->GetCharAdvance('M');
    if (m <= 0.0f) return false;
    for (ImWchar c : {ImWchar('i'), ImWchar('l'), ImWchar('W'), ImWchar('.'), ImWchar('m')})
        if (std::fabs(baked->GetCharAdvance(c) - m) > 0.5f) return false;
    return true;
}

struct TerminalFaces
{
    ImFont* regular = nullptr;
    ImFont* bold    = nullptr;
};

// Adds the terminal's regular and bold faces for `pref`. A custom face is
// backed by JetBrains Mono (Cyrillic, box drawing, Nerd Font glyphs) and the
// symbol fonts for whatever it lacks; the atlas is left untouched on failure.
bool AddTerminalFaces(ImGuiIO& io, const config::TerminalFont& pref, TerminalFaces* out, std::string* error)
{
    const bool custom = !pref.IsDefaultFace();
    const float px    = pref.size * gScale;

    const fs::path regularPath = custom ? fs::path(pref.regularPath) : kJbRegular;
    const fs::path boldPath    = custom ? fs::path(pref.boldPath) : kJbBold;
    if (custom && !config::LooksLikeFontFile(regularPath, error)) return false;

    auto fallbacks = [&](const fs::path& jb) {
        auto list = SymbolFallbacks();
        if (custom) list.insert(list.begin(), {jb, BaseConfig()});
        return list;
    };

    out->regular = AddFace(io, {regularPath}, px, fallbacks(kJbRegular));
    if (!out->regular)
    {
        *error = custom ? "не удалось загрузить шрифт " + regularPath.filename().string()
                        : "не найден встроенный шрифт терминала " + kJbRegular.string();
        return false;
    }
    if (custom && !IsMonospaced(out->regular, px))
    {
        io.Fonts->RemoveFont(out->regular);
        out->regular = nullptr;
        *error       = "«" + (pref.family.empty() ? regularPath.filename().string() : pref.family) +
                 "» — не моноширинный шрифт: символы разной ширины разъедутся по сетке терминала";
        return false;
    }

    // Bold is optional; one that doesn't line up with the regular face is worse than none.
    std::string ignored;
    if (!boldPath.empty() && (!custom || config::LooksLikeFontFile(boldPath, &ignored)))
        out->bold = AddFace(io, {boldPath}, px, fallbacks(kJbBold));
    if (out->bold && custom)
    {
        const bool same = IsMonospaced(out->bold, px) &&
                          std::fabs(out->bold->GetFontBaked(px)->GetCharAdvance('M') -
                                    out->regular->GetFontBaked(px)->GetCharAdvance('M')) <= 0.5f;
        if (!same)
        {
            io.Fonts->RemoveFont(out->bold);
            out->bold = nullptr;
        }
    }
    if (!out->bold) out->bold = out->regular;
    return true;
}
} // namespace

std::string BundledFontPath(const char* file)
{
    const fs::path assets = AssetsDir();
    if (assets.empty()) return {};
    const fs::path p = assets / "fonts" / file;
    return Exists(p) ? p.string() : std::string{};
}

const std::string& TerminalFontError()
{
    return gTerminalError;
}

void PreviewTerminalSize(float logicalPx)
{
    TerminalSize = std::clamp(logicalPx, config::TerminalFont::kMinSize, config::TerminalFont::kMaxSize) * gScale;
}

bool SetTerminalFont(const config::TerminalFont& pref)
{
    // Same face, other size: sizes are baked on demand, nothing to rebuild.
    if (Mono && pref.family == gApplied.family && pref.regularPath == gApplied.regularPath &&
        pref.boldPath == gApplied.boldPath)
    {
        TerminalSize = pref.size * gScale;
        gApplied     = pref;
        gTerminalError.clear();
        return true;
    }

    ImGuiIO& io = ImGui::GetIO();
    TerminalFaces faces;
    std::string   error;
    if (!AddTerminalFaces(io, pref, &faces, &error))
    {
        gTerminalError = error;
        return false;
    }

    // Drop the previous faces unless the interface font aliases them (the
    // no-font-files fallback in Load).
    ImFont* oldRegular = Mono;
    ImFont* oldBold    = MonoBold;
    Mono               = faces.regular;
    MonoBold           = faces.bold;
    for (ImFont* old : {oldRegular, oldBold == oldRegular ? nullptr : oldBold})
        if (old && old != Ui && old != Serif) io.Fonts->RemoveFont(old);

    TerminalSize = pref.size * gScale;
    gApplied     = pref;
    gTerminalError.clear();
    return true;
}

void Load(float scale, const config::TerminalFont& terminal)
{
    ImGuiIO& io = ImGui::GetIO();
    const fs::path assets = AssetsDir();
    const fs::path fontsDir = assets.empty() ? fs::path{} : assets / "fonts";

    gScale = scale;
    const auto symbols = SymbolFallbacks();

    // ── Terminal ─────────────────────────────────────────────────────
    {
        TerminalFaces faces;
        std::string   error;
        if (AddTerminalFaces(io, terminal, &faces, &error)) gApplied = terminal;
        else
        {
            // The chosen font is unusable — start with the built-in one, and say why in Settings.
            gTerminalError = error;
            config::TerminalFont fallback;
            fallback.size = terminal.size;
            if (AddTerminalFaces(io, fallback, &faces, &error)) gApplied = fallback;
        }
        Mono         = faces.regular;
        MonoBold     = faces.bold;
        TerminalSize = terminal.size * scale;
    }

    // ── Interface ────────────────────────────────────────────────────
    std::vector<std::pair<fs::path, ImFontConfig>> uiFallbacks;
    // Icons: Anthropicons when the (optional, not redistributable) file is
    // there, else Codicons out of the Nerd Font, else none — see icons.hpp.
    const fs::path anthropicons = fontsDir.empty() ? fs::path{} : fontsDir / "Anthropicons-Variable.ttf";
    if (Exists(anthropicons))
    {
        // Anthropicons sit entirely above the baseline (ascent = em, no
        // descent), centred half an em up; text caps centre ~0.29 of the size
        // up. Shift icons down by the difference so they line up with labels.
        constexpr float kIconScale = 1.05f;
        ImFontConfig icons = BaseConfig();
        icons.ExtraSizeScale = kIconScale;
        icons.GlyphOffset = {0.0f, std::round((0.5f * kIconScale - 0.29f) * kUiSize * scale)};
        icons.GlyphMinAdvanceX = kUiSize * scale; // icons line up in menus
        uiFallbacks.emplace_back(anthropicons, icons);
        icons::Use(icons::Set::Anthropicons);
    }
    else if (const fs::path nerd = Exists(kJbPropo) ? kJbPropo : kJbRegular; Exists(nerd))
    {
        // Only the Codicons block, so the Nerd Font's other private-use glyphs
        // can't stand in for anything. The Propo variant keeps icons full
        // size; the Mono one squeezes them into a single cell.
        static constexpr ImWchar kCodicons[] = {0xEA60, 0xEC1E, 0};
        ImFontConfig icons = BaseConfig();
        icons.GlyphRanges = kCodicons;
        icons.GlyphMinAdvanceX = kUiSize * scale; // icons line up in menus
        uiFallbacks.emplace_back(nerd, icons);
        icons::Use(icons::Set::Codicons);
    }
    else icons::Use(icons::Set::Text);
    ImFontConfig noto = BaseConfig();
    noto.ExtraSizeScale = kNotoMatchScale;
    uiFallbacks.emplace_back("/usr/share/fonts/noto/NotoSans-Regular.ttf", noto); // Cyrillic
    uiFallbacks.insert(uiFallbacks.end(), symbols.begin(), symbols.end());
    uiFallbacks.emplace_back(kJbRegular, BaseConfig()); // last resort for arrows, boxes, …
    Ui = AddFace(io, {fontsDir / "anthropic-sans.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf"}, kUiSize * scale,
                 uiFallbacks);

    // ── Wordmark ─────────────────────────────────────────────────────
    Serif = AddFace(io, {fontsDir / "anthropic-serif.ttf"}, kUiSize * scale,
                    {{"/usr/share/fonts/noto/NotoSerif-Regular.ttf", noto}});

    if (!Mono)
    {
        Mono = io.Fonts->AddFontDefault();
        ImGui::GetStyle().FontScaleMain = scale;
    }
    if (!MonoBold) MonoBold = Mono;
    if (!Ui) Ui = Mono;
    if (!Serif) Serif = Ui;
    io.FontDefault = Ui;
}
} // namespace ui::fonts
