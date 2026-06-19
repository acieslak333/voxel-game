#pragma once

/**
 * @file ColorPalette.h
 * @brief Post-process retro colour palette loader and directory lister.
 *
 * Loads Lospec-style .hex files from assets/colorpalettes/. The composite
 * pass remaps the whole rendered frame to the nearest colour in the active
 * palette. Unlike vg::Palette (named colours from colors.yaml), this is an
 * ordered swatch list without names, capped at kMaxPaletteColors entries.
 * @see docs/CODE_INDEX.md
 */
#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace vg {

/**
 * @brief Maximum number of swatches uploaded to the composite shader.
 *
 * Matches the palette array size in composite.frag. Entries beyond this
 * limit in a .hex file are silently discarded.
 */
// Max swatches uploaded to the shader (matches composite.frag's palette array).
inline constexpr int kMaxPaletteColors = 64;

/**
 * @brief Load sRGB colours from a Lospec-style .hex file.
 *
 * Accepts RRGGBB or #RRGGBB per line; skips blank lines and comment lines
 * starting with ';' or '//'. Returns at most kMaxPaletteColors entries.
 * Returns an empty vector if the file is missing or unreadable (palette off).
 * @param path Full path to the .hex file.
 * @return sRGB colours in [0,1], capped at kMaxPaletteColors.
 */
[[nodiscard]] std::vector<glm::vec3> loadColorPalette(const std::string& path);

/**
 * @brief List palette names (stems of .hex files) in `dir`, sorted alphabetically.
 *
 * Names feed the in-game Retro tab selector and the saved retroPalette setting.
 * An empty string ("") denotes "no palette / off" (not returned by this function).
 * @param dir Directory to scan (typically assets/colorpalettes/).
 * @return Sorted list of palette name stems.
 */
[[nodiscard]] std::vector<std::string> listColorPalettes(const std::string& dir);

} // namespace vg
