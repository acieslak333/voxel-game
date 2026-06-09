#pragma once

#include "world/Block.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace vg {

// -----------------------------------------------------------------------------
//  BlockProperties
// -----------------------------------------------------------------------------
//  Static, per-block-type data. This is the single source of truth for how a
//  block behaves and looks. Designed to be easy to extend: add a field here and
//  set it in BlockRegistry's constructor.
// -----------------------------------------------------------------------------
struct BlockProperties {
    std::string name;
    bool solid  = false; // participates in collision (Milestone 2)
    bool opaque = false; // hides the faces of neighbours touching it (meshing)

    // Texture-array layer index per face, indexed by the Face enum. Lets a
    // block show different textures on top/side/bottom (e.g. grass).
    std::array<uint32_t, FaceCount> faceLayers{};

    // TODO(future): renderType (Cube / Cross / custom Model) so non-cube blocks
    // (furnace, plants) can bypass greedy meshing; hardness; light emission; etc.
};

// -----------------------------------------------------------------------------
//  BlockRegistry
// -----------------------------------------------------------------------------
//  Maps block id -> properties and owns the list of texture images that need to
//  be loaded into the texture array (deduplicated, so a texture shared by two
//  blocks occupies a single array layer). Keeping the texture list here means
//  the registry stays the one place that knows about block appearance.
// -----------------------------------------------------------------------------
class BlockRegistry {
public:
    BlockRegistry(); // registers the built-in blocks

    [[nodiscard]] const BlockProperties& get(uint16_t id) const;
    [[nodiscard]] bool isSolid(uint16_t id) const { return get(id).solid; }
    [[nodiscard]] bool isOpaque(uint16_t id) const { return get(id).opaque; }
    [[nodiscard]] uint32_t faceLayer(uint16_t id, int face) const {
        return get(id).faceLayers[static_cast<size_t>(face)];
    }

    // Texture images to upload, in layer order (index == array layer).
    [[nodiscard]] const std::vector<std::string>& texturePaths() const {
        return texturePaths_;
    }
    [[nodiscard]] uint32_t layerCount() const {
        return static_cast<uint32_t>(texturePaths_.size());
    }

private:
    // Register a block at a specific id (grows the table as needed).
    void registerBlock(BlockId id, BlockProperties props);
    // Intern a texture filename, returning its array layer (dedup by name).
    uint32_t internTexture(const std::string& filename);

    std::vector<BlockProperties> blocks_;       // indexed by block id
    std::vector<std::string>     texturePaths_; // indexed by texture layer
};

} // namespace vg
