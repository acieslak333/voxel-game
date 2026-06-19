/**
 * @file ColorPalette.cpp
 * @brief loadColorPalette and listColorPalettes implementations.
 *
 * loadColorPalette parses a Lospec .hex file line-by-line (RRGGBB or #RRGGBB),
 * skipping blank lines and comment lines (';' or '//'). listColorPalettes
 * enumerates .hex files in a directory and returns their stems sorted alphabetically.
 * @see docs/CODE_INDEX.md
 */
#include "core/ColorPalette.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>

namespace vg {

namespace {

// Trim ASCII whitespace from both ends.
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Parse "rrggbb" / "#rrggbb" -> sRGB [0,1]. Returns false if not 6 hex digits.
bool parseHexColor(std::string h, glm::vec3& out) {
    if (!h.empty() && h.front() == '#') h.erase(0, 1);
    if (h.size() != 6) return false;
    for (char c : h) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    auto chan = [&](int off) {
        return static_cast<float>(std::stoi(h.substr(off, 2), nullptr, 16)) / 255.0f;
    };
    out = glm::vec3(chan(0), chan(2), chan(4));
    return true;
}

} // namespace

std::vector<glm::vec3> loadColorPalette(const std::string& path) {
    std::vector<glm::vec3> colors;
    std::ifstream in(path);
    if (!in) return colors;

    std::string line;
    while (std::getline(in, line) &&
           static_cast<int>(colors.size()) < kMaxPaletteColors) {
        line = trim(line);
        if (line.empty() || line[0] == ';' ||
            (line.size() >= 2 && line[0] == '/' && line[1] == '/')) {
            continue; // comment / blank
        }
        glm::vec3 c;
        if (parseHexColor(line, c)) colors.push_back(c);
        // Non-colour lines (e.g. a `# heading`) are silently ignored.
    }
    return colors;
}

std::vector<std::string> listColorPalettes(const std::string& dir) {
    std::vector<std::string> names;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return names;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        const std::filesystem::path& p = entry.path();
        if (p.extension() == ".hex") {
            names.push_back(p.stem().string());
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace vg
