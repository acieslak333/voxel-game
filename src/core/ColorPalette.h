#pragma once

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace vg {

// -----------------------------------------------------------------------------
//  ColorPalette  (post-process retro palettes)
// -----------------------------------------------------------------------------
//  A flat list of sRGB colours loaded from a Lospec-style `.hex` file under
//  assets/colorpalettes/. Unlike vg::Palette (named block/UI colours from
//  colors.yaml), this is an ordered swatch list the composite pass remaps the
//  whole frame onto — the selectable "retro palette" the player picks in the
//  Retro settings tab. See assets/colorpalettes/README.md for the file format.
// -----------------------------------------------------------------------------

// Max swatches uploaded to the shader (matches composite.frag's palette array).
inline constexpr int kMaxPaletteColors = 64;

// Parse one `.hex` file -> sRGB colours [0,1]. One `RRGGBB` (optional leading
// '#') per line; blank lines and non-colour lines (`;`, `//`, comments) are
// skipped. At most kMaxPaletteColors are returned. Returns empty on a missing or
// unreadable file (the caller treats "no colours" as palette-off).
[[nodiscard]] std::vector<glm::vec3> loadColorPalette(const std::string& path);

// List the palette names (filename without the `.hex` extension) available in
// `dir`, sorted alphabetically. Names feed the in-game selector and the saved
// `retroPalette` setting; "" (empty) means no palette / off.
[[nodiscard]] std::vector<std::string> listColorPalettes(const std::string& dir);

} // namespace vg
