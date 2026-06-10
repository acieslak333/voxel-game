#pragma once

#include <glm/glm.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace vg {

// -----------------------------------------------------------------------------
//  Palette
// -----------------------------------------------------------------------------
//  A named colour palette loaded from a YAML map of name -> "#RRGGBB"
//  (assets/colors.yaml, sampled from assets/colormap.png). Colours are stored in
//  sRGB; linear() removes the sRGB curve for use with sRGB render targets (where
//  clear colours and lighting maths are specified in linear space).
//
//  This is the single place block tints, the sky colour, UI accents, etc. can
//  pull a consistent set of colours from.
// -----------------------------------------------------------------------------
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
