#pragma once

/**
 * @file BlockbenchModel.h
 * @brief Loader for Blockbench .bbmodel files into the box-part Skeleton + skin info.
 *
 * Maps Blockbench outliner groups to Joints and cuboid elements to Boxes, converting
 * coordinates from Blockbench units (16 = 1 block) to block space. Supports per-face
 * UV rects, box-UV cross layout, and embedded base64 texture PNGs. Pure CPU (uses
 * yaml-cpp to parse JSON); exercised headlessly by `--logictest`.
 * @see docs/CODE_INDEX.md
 */

#include "entity/Armature.h"

#include <string>

namespace vg {

// -----------------------------------------------------------------------------
//  Blockbench (.bbmodel) loader  (ISSUES #13E: real entity/item models)
// -----------------------------------------------------------------------------
//  Blockbench is a free box-model editor; its native .bbmodel file is JSON listing
//  cuboid `elements` (from/to/origin/rotation + per-face UV) under an `outliner`
//  bone tree — which maps 1:1 onto our box-part armature (entity/Armature.h):
//    outliner group  -> Joint        (pivot = origin, rest rotation = its rotation)
//    element         -> Box          (per-face UV from the model's skin)
//    element rotation-> its own Joint (so a tilted cube poses correctly)
//  Coordinates convert from Blockbench units (16 = 1 block, Y up) to block space.
//
//  Authoring: model a tool/mob in Blockbench, export "Blockbench Model" (.bbmodel)
//  plus its texture PNG into assets/models/. The same loader serves held items,
//  dropped items and mobs. Pure CPU (yaml-cpp parses the JSON), so it's exercised
//  headlessly by --logictest.
// -----------------------------------------------------------------------------
/** @brief Loaded .bbmodel: a poseable Skeleton, skin filename, resolution, and optional embedded PNG. */
struct BlockbenchModel {
    Skeleton    skeleton;        // the posed-able box rig (joints + boxes)
    std::string skin;            // texture filename the model references (under assets/models/)
    int         texW = 16;       // skin resolution (UVs were normalised against this)
    int         texH = 16;
    // Blockbench's native save EMBEDS the skin as a base64 data URI rather than an
    // external file. When the referenced texture is embedded, this holds its decoded
    // PNG file bytes (feed to stbi_load_from_memory); empty means use `skin` as a path.
    std::vector<unsigned char> embeddedPNG;
};

// Load a .bbmodel into a BlockbenchModel. Throws std::runtime_error if the file is
// missing or malformed. The returned skeleton is topologically ordered.
[[nodiscard]] BlockbenchModel loadBlockbenchModel(const std::string& path);

} // namespace vg
