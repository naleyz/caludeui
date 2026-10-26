#include "config/font_catalog.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace config
{
namespace
{
namespace fs = std::filesystem;

std::string LowerExt(const std::string& path)
{
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext;
}

struct Face
{
    std::string path;
    int         weight = 80;
};

// FC_WEIGHT_REGULAR = 80, FC_WEIGHT_BOLD = 200. Closest wins; a face has to be
// at least semibold (180) to count as "the bold one".
const Face* Closest(const std::vector<Face>& faces, int target, int minWeight, int maxWeight)
{
    const Face* best = nullptr;
    for (const auto& f : faces)
    {
        if (f.weight < minWeight || f.weight > maxWeight) continue;
        if (!best || std::abs(f.weight - target) < std::abs(best->weight - target)) best = &f;
    }
    return best;
}
} // namespace

std::vector<MonoFontFamily> ScanMonospaceFonts()
{
    // Fixed command, no user input in it.
    FILE* p = ::popen("fc-list :spacing=100 -f '%{family[0]}\\t%{weight}\\t%{slant}\\t%{variable}\\t%{file}\\n' 2>/dev/null", "r");
    if (!p) return {};
    std::string out;
    std::array<char, 4096> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), p)) out += buf.data();
    ::pclose(p);

    std::map<std::string, std::vector<Face>> families;
    std::istringstream lines(out);
    for (std::string line; std::getline(lines, line);)
    {
        std::vector<std::string> col;
        std::istringstream ls(line);
        for (std::string c; std::getline(ls, c, '\t');) col.push_back(c);
        if (col.size() != 5 || col[0].empty()) continue;

        const std::string ext = LowerExt(col[4]);
        if (ext != ".ttf" && ext != ".otf") continue;
        if (std::atoi(col[2].c_str()) != 0) continue; // italic / oblique

        Face f;
        f.path = col[4];
        // A variable font's default instance is what ImGui loads, whatever weight range it lists.
        f.weight = col[3] == "True" ? 80 : std::atoi(col[1].c_str());
        families[col[0]].push_back(std::move(f));
    }

    std::vector<MonoFontFamily> result;
    for (auto& [name, faces] : families)
    {
        const Face* regular = Closest(faces, 80, 0, 150);
        if (!regular) continue; // bold-only family: nothing sensible as a base face
        MonoFontFamily fam;
        fam.family  = name;
        fam.regular = regular->path;
        if (const Face* bold = Closest(faces, 200, 180, 1000); bold && bold != regular) fam.bold = bold->path;
        result.push_back(std::move(fam));
    }
    return result; // std::map already sorted them
}

std::string GuessBoldSibling(const std::string& regularPath)
{
    const fs::path path(regularPath);
    const std::string name = path.filename().string();
    for (const char* marker : {"Regular", "regular", "-Book", "Book"})
    {
        const auto pos = name.find(marker);
        if (pos == std::string::npos) continue;
        std::string bold = name;
        bold.replace(pos, std::string(marker).size(), marker[0] == '-' ? "-Bold" : (marker[0] == 'r' ? "bold" : "Bold"));
        std::error_code ec;
        const fs::path candidate = path.parent_path() / bold;
        if (fs::is_regular_file(candidate, ec)) return candidate.string();
    }
    return {};
}

bool LooksLikeFontFile(const fs::path& path, std::string* error)
{
    std::error_code ec;
    if (!fs::is_regular_file(path, ec))
    {
        *error = "файл не найден: " + path.string();
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    char magic[4] = {};
    if (!in.read(magic, 4))
    {
        *error = "не удаётся прочитать файл: " + path.string();
        return false;
    }
    const std::string m(magic, 4);
    if (m == std::string("\0\1\0\0", 4) || m == "true" || m == "OTTO") return true;
    *error = m == "ttcf" ? "коллекции шрифтов (.ttc) не поддерживаются — укажите отдельный .ttf/.otf"
                         : "это не файл шрифта TrueType/OpenType: " + path.filename().string();
    return false;
}
} // namespace config
