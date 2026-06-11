#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace vg {

class BlockRegistry;

// -----------------------------------------------------------------------------
//  Structure
// -----------------------------------------------------------------------------
//  A small voxel template (a well, a boulder, a ruin...) stamped into the world
//  during generation. Loaded from a hand-authored YAML file under
//  assets/structures/ (see Structure.cpp for the format). The grid is row-major
//  x→z→y; each cell is a block id, or kSkip to leave whatever terrain is there
//  (so a structure can sit ON the ground without a solid bounding box, and carve
//  air only where it explicitly wants to).
//
//  `anchor` is the local cell that lands on the terrain surface at a placement
//  origin: anchor.x/z are the horizontal centre, anchor.y is the layer that sits
//  at ground level (cells below it are foundation that overwrites terrain, cells
//  above build upward). This keeps stamping a pure function of the origin column's
//  surface height, so it is identical no matter which chunk streams in first.
// -----------------------------------------------------------------------------
struct Structure {
    static constexpr uint16_t kSkip = 0xFFFF; // "leave the existing block here"

    std::string name;
    glm::ivec3  size{0};          // x, y, z extent in blocks
    glm::ivec3  anchor{0};        // local cell that meets the terrain surface
    float       weight  = 1.0f;   // relative pick weight among all structures
    bool        surface = true;   // only place where the origin column is dry land
    std::vector<uint16_t> voxels; // size.x*size.y*size.z, indexed by index()

    [[nodiscard]] int index(int x, int y, int z) const {
        return (y * size.z + z) * size.x + x;
    }
    // Block id at a local cell, or kSkip (out-of-range also reads as kSkip).
    [[nodiscard]] uint16_t at(int x, int y, int z) const {
        if (x < 0 || y < 0 || z < 0 || x >= size.x || y >= size.y || z >= size.z) {
            return kSkip;
        }
        return voxels[static_cast<size_t>(index(x, y, z))];
    }
};

// -----------------------------------------------------------------------------
//  StructureSet — every structure template loaded from assets/structures/.
//  Owns the templates and the max horizontal half-extent any of them reaches from
//  a placement origin (so the generator knows how far to scan for nearby origins
//  whose footprint might cover the column it is filling — the seam-safe gather,
//  mirroring how trees gather from nearby roots).
// -----------------------------------------------------------------------------
class StructureSet {
public:
    // Load every *.yaml under `dir` (missing dir → empty set, structures off).
    // Block names in each template's legend are resolved via the registry.
    StructureSet(const std::string& dir, const BlockRegistry& registry);

    [[nodiscard]] bool empty() const { return structures_.empty(); }
    [[nodiscard]] const std::vector<Structure>& all() const { return structures_; }
    // Max blocks any structure extends from its origin along X or Z — the radius a
    // column must scan candidate origins over. 0 when empty.
    [[nodiscard]] int maxReachXZ() const { return maxReachXZ_; }
    [[nodiscard]] float totalWeight() const { return totalWeight_; }

    // Pick a structure index for a placement roll r in [0,1) weighted by `weight`.
    // Returns -1 if empty.
    [[nodiscard]] int pick(float r) const;

private:
    std::vector<Structure> structures_;
    int   maxReachXZ_  = 0;
    float totalWeight_ = 0.0f;
};

} // namespace vg
