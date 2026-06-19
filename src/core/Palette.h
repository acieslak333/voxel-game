#pragma once

/**
 * @file Palette.h
 * @brief Named sRGB colour palette loaded from assets/colors.yaml.
 *
 * Provides lookup by name in both sRGB and linear colour spaces. This is the
 * single source of truth for block tints, sky colours, and UI accents. Colour
 * names are referenced from blocks.yaml, sky.yaml, and App settings.
 * @see docs/CODE_INDEX.md
 */
#include <glm/glm.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace vg {

/**
 * @brief Named colour palette loaded from a YAML name-to-"#RRGGBB" map.
 *
 * Colours are stored in sRGB [0,1]. linear() applies the inverse sRGB curve
 * for use wherever linear-space values are required (lighting, clear colours).
 */
class Palette {
public:
    // Load the palette. Throws std::runtime_error if the file is missing or any
    // entry is not a #RRGGBB hex string.
    explicit Palette(const std::string& paletteFile);

    // sRGB colour [0,1] by name. Throws std::out_of_range if the name is absent.
    [[nodiscard]] glm::vec3 srgb(const std::string& name) const;
    // Same colour with the sRGB curve removed (linear space).
    [[nodiscard]] glm::vec3 linear(const std::string& name) const;
    [[nodiscard]] bool has(const std::string& name) const;

    // All colour names in file order (handy for cycling through them in the UI).
    [[nodiscard]] const std::vector<std::string>& names() const { return order_; }

private:
    std::unordered_map<std::string, glm::vec3> colors_; // name -> sRGB [0,1]
    std::vector<std::string>                   order_;  // names in file order
};

} // namespace vg
