#include "world/ChunkMesher.h"

#include "world/BlockRegistry.h"
#include "world/Chunk.h"
#include "world/Shape.h"

#include <algorithm>
#include <array>

namespace vg {

namespace {

constexpr int N = kChunkSize;

// Cheap directional shading so the geometry reads without real lighting:
// top faces brightest, bottom darkest, sides in between (and X != Z so adjacent
// walls are distinguishable).
float faceShade(int axis, bool positive) {
    if (axis == 1) return positive ? 1.0f : 0.45f; // +Y top / -Y bottom
    if (axis == 0) return 0.62f;                    // X faces
    return 0.80f;                                   // Z faces
}

// Map (axis, sign) to the Face enum used by the block registry.
int faceIndex(int axis, bool positive) {
    return axis * 2 + (positive ? 1 : 0);
}

// Deterministic spatial hash of a world block position -> a 32-bit value used to
// pick a texture variant for a multi-texture face. Pure function of position, so
// the choice is stable across re-meshes and identical in neighbouring chunks (no
// seam at chunk borders). Standard xor-of-large-primes mix + an avalanche finalizer
// so adjacent cells land on different variants instead of striping.
uint32_t variantHash(int x, int y, int z) {
    uint32_t h = static_cast<uint32_t>(x) * 73856093u ^
                 static_cast<uint32_t>(y) * 19349663u ^
                 static_cast<uint32_t>(z) * 83492791u;
    h ^= h >> 13;
    h *= 0x85ebca6bu;
    h ^= h >> 16;
    return h;
}

// Texture coordinates for an axis-aligned face, in *block units* (the REPEAT
// sampler shows one full texel-square per block instead of stretching the image
// across the whole quad). axis: 0 = X-face, 1 = Y-face (top/bottom), 2 = Z-face.
// V follows world-Y *negated* so the image is upright (row 0 at the block's top);
// U follows whichever horizontal axis lies in the face's plane. This is the crux
// of combining textures with greedy meshing AND with partial shapes: a half-block
// slab face shows the bottom half of the image, a quarter-block post shows its
// centred slice — always at a true 16-px-per-block density. Used by the cube
// greedy pass and by every axis-aligned non-cube box face (posts, slabs, stairs,
// walls, leaf-cube faces, the liquid surface).
glm::vec2 faceUV(const glm::vec3& p, int axis) {
    if (axis == 1) {
        return glm::vec2(p.x, p.z); // top / bottom: flat on X-Z
    }
    const float horiz = (axis == 0) ? p.z : p.x; // X-faces use Z, Z-faces use X
    return glm::vec2(horiz, -p.y);
}

// Ambient-occlusion brightness for each of the four occlusion levels (0 = corner
// boxed in on both sides, darkest; 3 = fully open). Multiplies the directional
// faceShade; the shader interpolates the per-vertex result for smooth lighting.
constexpr float kAoShade[4] = {0.5f, 0.7f, 0.85f, 1.0f};

// One cell of the 2D mask used while sweeping a slice. Two cells may be merged
// into the same quad only if every field matches — including the four per-corner
// AO levels, so the smooth-lighting gradient is never stretched across a merge.
struct Mask {
    bool     present   = false;
    uint16_t block     = 0;
    bool     positive  = false; // face normal points along +axis?
    uint32_t layer     = 0;
    uint32_t tint      = 0xFFFFFFFFu; // biome vegetation tint (white = none)
    float    baseShade = 0.0f;  // directional (top/bottom/side) brightness
    uint8_t  ao[4]     = {3, 3, 3, 3};         // occlusion 0..3 at the 4 face corners
    float    sky[4]    = {15, 15, 15, 15};      // smoothed sky light 0..15 per corner
    float    blk[4]    = {0, 0, 0, 0};          // smoothed block light 0..15 per corner
    glm::vec3 col[4]   = {};                     // smoothed block-light hue per corner

    bool operator==(const Mask& o) const {
        return present == o.present && block == o.block && positive == o.positive &&
               layer == o.layer && tint == o.tint && baseShade == o.baseShade && ao[0] == o.ao[0] &&
               ao[1] == o.ao[1] && ao[2] == o.ao[2] && ao[3] == o.ao[3] &&
               sky[0] == o.sky[0] && sky[1] == o.sky[1] && sky[2] == o.sky[2] &&
               sky[3] == o.sky[3] && blk[0] == o.blk[0] && blk[1] == o.blk[1] &&
               blk[2] == o.blk[2] && blk[3] == o.blk[3] &&
               col[0] == o.col[0] && col[1] == o.col[1] &&
               col[2] == o.col[2] && col[3] == o.col[3];
    }
    bool operator!=(const Mask& o) const { return !(*this == o); }
};

} // namespace

MeshData ChunkMesher::greedyMesh(const Chunk& chunk, const BlockRegistry& reg,
                                 const NeighborSampler& neighbor,
                                 const LightSampler& light, bool smoothLighting,
                                 const glm::ivec3& worldOrigin, const TintSampler& tint) {
    MeshData mesh;

    // In-bounds blocks come straight from the chunk; the boundary sweep below
    // reaches one block past each edge, where we defer to the neighbour sampler
    // so cross-chunk faces are culled (instead of treating outside as air).
    auto sample = [&](int x, int y, int z) -> Block {
        return Chunk::inBounds(x, y, z) ? chunk.get(x, y, z) : neighbor(x, y, z);
    };
    // Liquid ids (water/lava). Water is drawn TRANSLUCENT: its surfaces go into a
    // separate batch (mesh.waterVertices/Indices) the renderer alpha-blends over
    // the terrain behind it, so the seabed shows through. For that the water must
    // NOT occlude what is behind it, so it counts as see-through in the opaque
    // sweep (the seabed/shore still meshes there). Lava stays opaque (molten rock).
    // A *flowing* liquid cell (metadata > 0) is a partial-height non-cube block
    // emitted in the second pass below; a *source* cell (metadata 0, e.g. the
    // ocean) is greedy-meshed so big water bodies stay cheap.
    uint16_t waterId = 0xFFFF, lavaId = 0xFFFF;
    try { waterId = reg.idByName("water"); } catch (...) {}
    try { lavaId  = reg.idByName("lava"); }  catch (...) {}
    auto isWater   = [&](uint16_t id) { return id == waterId; };
    auto isLiquid  = [&](uint16_t id) { return id == waterId || id == lavaId; };
    auto isFlowing = [&](const Block& b) { return isLiquid(b.id) && b.metadata > 0; };

    // The shape a cell renders as: only shapeable blocks read Block::metadata as a
    // shape; everything else is a full Cube. A reshaped cube (slab/stairs/...) is
    // NOT a full opaque cube — it emits its own box geometry in the second pass and
    // must not be greedy-meshed nor cull a neighbour's face.
    auto cellShape = [&](const Block& b) -> ShapeKind {
        return reg.shapeable(b.id) ? shapeKindOf(b.metadata) : ShapeKind::Cube;
    };

    // OPAQUE sweep "solid" test: a real opaque FULL-CUBE block that is NOT water,
    // NOT a flowing liquid and NOT reshaped — so water/flowing liquids and any
    // shaped block are see-through here (they drop out into a later pass), and this
    // doubles as the "is the neighbour a solid full cube?" test used to cull the
    // boundary faces of shaped boxes.
    auto fillOpaque = [&](int x, int y, int z) {
        const Block b = sample(x, y, z);
        return reg.isOpaque(b.id) && !isWater(b.id) && !isFlowing(b) &&
               cellShape(b) == ShapeKind::Cube;
    };
    // WATER sweep "solid" test: any water is part of the body, plus opaque terrain
    // and (opaque) lava as the solids it culls against — so only the water-vs-air
    // surface is emitted (no doubled face at the seabed). The source's side against
    // flowing water is culled here, which is fine because the flowing surface rises
    // to the source's full height at their shared corners (see the corner-connected
    // flowing pass below), so there is no gap to fill.
    auto fillWater = [&](int x, int y, int z) {
        const Block b = sample(x, y, z);
        return isWater(b.id) ||
               (reg.isOpaque(b.id) && !isFlowing(b) && cellShape(b) == ShapeKind::Cube);
    };
    auto emitAny   = [&](const Block&) { return true; };
    // Only emit faces owned by a SOURCE water cell; flowing water is drawn as a
    // partial-height box in the second pass (and would otherwise get a wrong full-
    // height greedy face here).
    auto emitWater = [&](const Block& b) { return isWater(b.id) && b.metadata == 0; };

    // -------------------------------------------------------------------------
    //  The greedy sweep, parametrised so it runs twice: once for opaque terrain
    //  (-> mesh.vertices/indices) and once for translucent source water
    //  (-> mesh.waterVertices/Indices).
    //    fill(x,y,z): is this cell part of the body being meshed (culling + AO)?
    //    emit(block): emit a face owned by this block? (skips non-owned faces, so
    //                 the water pass doesn't re-emit terrain the opaque pass owns)
    //    outV/outI:   target vertex/index buffers.
    // -------------------------------------------------------------------------
    auto sweep = [&](auto fill, auto emit, std::vector<Vertex>& outV,
                     std::vector<uint32_t>& outI) {
    // Append a w x h quad lying on a plane perpendicular to `d`, with its lower
    // corner at (base in u,v) and the given face attributes.
    auto addQuad = [&](int d, int u, int v, int plane, int i, int j, int w, int h,
                       const Mask& m) {
        float du[3] = {0, 0, 0}; du[u] = static_cast<float>(w);
        float dv[3] = {0, 0, 0}; dv[v] = static_cast<float>(h);
        float base[3] = {0, 0, 0};
        base[d] = static_cast<float>(plane);
        base[u] = static_cast<float>(i);
        base[v] = static_cast<float>(j);

        const glm::vec3 p0(base[0], base[1], base[2]);
        const glm::vec3 p1(base[0] + du[0], base[1] + du[1], base[2] + du[2]);
        const glm::vec3 p2(base[0] + du[0] + dv[0], base[1] + du[1] + dv[1],
                           base[2] + du[2] + dv[2]);
        const glm::vec3 p3(base[0] + dv[0], base[1] + dv[1], base[2] + dv[2]);

        // Texture coordinates in *block units* (the REPEAT sampler tiles one
        // texel-square per block instead of stretching across the merged quad).
        // Derive them from world position so the texture is always upright:
        //   * V follows world Y *negated*, so the image's top row (e.g. the grass
        //     strip on grass_side) sits at the top of the block rather than the
        //     bottom — image row 0 maps to texture V 0, and screen-up is +Y.
        //   * U follows whichever horizontal axis lies in the face's plane.
        // Top/bottom faces (d == 1) have no vertical component, so they lie flat
        // on the X/Z plane. Block-aligned corners keep every tile boundary on a
        // block boundary.
        auto uvFor = [&](const glm::vec3& p) -> glm::vec2 { return faceUV(p, d); };

        // Per-vertex light, split into sky and block terms (0..15 -> 0..1), each
        // with the corner AO folded in. The *sky* term carries no directional
        // shade — the shader lights it dynamically against the current sun/moon
        // direction (that is what makes shadows track the time of day). The
        // *block* term keeps the fixed top/side/bottom shade so glow-lit shapes
        // still read as 3D. p0..p3 map to mask corners 0..3 (a merge requires
        // all four AO + sky + block values to match).
        auto lightOf = [&](int k) -> glm::vec2 {
            const float ao = kAoShade[m.ao[k]];
            return glm::vec2(ao * (m.sky[k] / 15.0f),
                             m.baseShade * ao * (m.blk[k] / 15.0f));
        };
        auto colorOf = [&](int k) -> uint32_t { return packColorRGBA8(m.col[k]); };
        const auto faceNormal = static_cast<uint32_t>(faceIndex(d, m.positive));

        const auto base_index = static_cast<uint32_t>(outV.size());
        outV.push_back({p0, uvFor(p0), m.layer, lightOf(0), faceNormal, colorOf(0), m.tint});
        outV.push_back({p1, uvFor(p1), m.layer, lightOf(1), faceNormal, colorOf(1), m.tint});
        outV.push_back({p2, uvFor(p2), m.layer, lightOf(2), faceNormal, colorOf(2), m.tint});
        outV.push_back({p3, uvFor(p3), m.layer, lightOf(3), faceNormal, colorOf(3), m.tint});

        // Winding: (u, v, d) form a right-handed basis, so p0->p1->p2->p3 is
        // counter-clockwise seen from the +d side. Positive faces use that order;
        // negative faces are reversed so their front side faces -d.
        //
        // Pick the diagonal so the quad is split *across* its two darkest corners.
        // Without this an L-shaped AO corner interpolates asymmetrically and the
        // shading visibly bends along the default diagonal ("the AO flip").
        const bool flip = (m.ao[0] + m.ao[2]) > (m.ao[1] + m.ao[3]);
        const uint32_t b = base_index;
        if (m.positive) {
            if (!flip) {
                outI.insert(outI.end(),
                    {b + 0, b + 1, b + 2, b + 0, b + 2, b + 3});
            } else {
                outI.insert(outI.end(),
                    {b + 0, b + 1, b + 3, b + 1, b + 2, b + 3});
            }
        } else {
            if (!flip) {
                outI.insert(outI.end(),
                    {b + 0, b + 2, b + 1, b + 0, b + 3, b + 2});
            } else {
                outI.insert(outI.end(),
                    {b + 0, b + 3, b + 1, b + 1, b + 3, b + 2});
            }
        }
    };

    // Sweep each of the three axes.
    for (int d = 0; d < 3; ++d) {
        const int u = (d + 1) % 3;
        const int v = (d + 2) % 3;

        int x[3] = {0, 0, 0};
        int q[3] = {0, 0, 0};
        q[d] = 1; // step of one block along the sweep axis

        std::vector<Mask> mask(static_cast<size_t>(N) * N);

        // --- Smooth lighting (ambient occlusion) helpers ---------------------
        // A face exposed to air at d-layer L, in-plane cell (cu,cv): each corner
        // is darkened by how many of the three opaque blocks hugging it (two edge
        // neighbours + the diagonal) are present, all sampled on the air side.
        auto occ = [&](int L, int cu, int cv) -> int {
            int c[3];
            c[d] = L;
            c[u] = cu;
            c[v] = cv;
            return fill(c[0], c[1], c[2]) ? 1 : 0;
        };
        auto cornerAo = [&](int L, int cu, int cv, int du, int dv) -> uint8_t {
            const int s1 = occ(L, cu + du, cv);
            const int s2 = occ(L, cu, cv + dv);
            if (s1 && s2) return 0; // boxed in on both edges: darkest
            return static_cast<uint8_t>(3 - (s1 + s2 + occ(L, cu + du, cv + dv)));
        };
        // Light (sky + block) at an air-side cell, and the per-corner *smoothed*
        // light: the average over the up-to-four air-side blocks around that
        // corner (skipping opaque ones, whose light is meaningless), matching the
        // AO neighbourhood. Sky and block are averaged independently.
        auto lightAt = [&](int L, int cu, int cv) -> LightSample {
            int c[3];
            c[d] = L;
            c[u] = cu;
            c[v] = cv;
            return light(c[0], c[1], c[2]);
        };
        // Smoothed corner light + hue. Sky/block are averaged over the up-to-four
        // air-side cells (matching the AO neighbourhood); the hue is averaged
        // *weighted by each cell's block level*, so the brightest emitter dominates
        // the corner colour and unlit (colour 0) cells don't wash it toward black.
        struct CornerLight { glm::vec2 light; glm::vec3 col; };
        auto cornerLight = [&](int L, int cu, int cv, int du, int dv) -> CornerLight {
            const int offU[4] = {0, du, 0, du};
            const int offV[4] = {0, 0, dv, dv};
            int sky = 0, blk = 0, count = 0;
            glm::vec3 colSum(0.0f);
            float colW = 0.0f;
            for (int k = 0; k < 4; ++k) {
                if (!occ(L, cu + offU[k], cv + offV[k])) {
                    const LightSample ls = lightAt(L, cu + offU[k], cv + offV[k]);
                    sky += ls.sky;
                    blk += ls.block;
                    colSum += ls.blockColor * static_cast<float>(ls.block);
                    colW += static_cast<float>(ls.block);
                    ++count;
                }
            }
            const glm::vec2 lv = count ? glm::vec2(static_cast<float>(sky) / count,
                                                   static_cast<float>(blk) / count)
                                       : glm::vec2(0.0f);
            const glm::vec3 col = colW > 0.0f ? colSum / colW : glm::vec3(0.0f);
            return {lv, col};
        };
        auto setFace = [&](Mask& m, uint16_t blk, bool positive, int L, int cu, int cv) {
            m.present   = true;
            m.block     = blk;
            m.positive  = positive;
            // The solid cell owning this face: its d-coord is one step back from the
            // air-side plane L (L-1 for a +d face, L+1 for a -d face); u/v are cu/cv.
            // Hash its WORLD position to pick the texture variant, so a multi-texture
            // face (grass/stone) varies per block and merges only with like variants.
            int cell[3];
            cell[d] = positive ? L - 1 : L + 1;
            cell[u] = cu;
            cell[v] = cv;
            m.layer     = reg.faceLayer(blk, faceIndex(d, positive),
                                        variantHash(worldOrigin.x + cell[0],
                                                    worldOrigin.y + cell[1],
                                                    worldOrigin.z + cell[2]));
            // Biome vegetation tint (white for non-foliage). Sampled at the owning
            // cell's column; bakes into the vertices so grass varies by biome.
            m.tint = packColorRGBA8(tint(cell[0], cell[2], blk));
            m.baseShade = faceShade(d, positive);
            if (!smoothLighting) {
                // Simple mode: flat directional shade only (no AO, full sky light).
                m.ao[0] = m.ao[1] = m.ao[2] = m.ao[3] = 3;
                m.sky[0] = m.sky[1] = m.sky[2] = m.sky[3] = 15.0f;
                m.blk[0] = m.blk[1] = m.blk[2] = m.blk[3] = 0.0f;
                m.col[0] = m.col[1] = m.col[2] = m.col[3] = glm::vec3(0.0f);
                return;
            }
            const int du[4] = {-1, +1, +1, -1};
            const int dv[4] = {-1, -1, +1, +1};
            for (int k = 0; k < 4; ++k) {
                m.ao[k] = cornerAo(L, cu, cv, du[k], dv[k]);
                const CornerLight lc = cornerLight(L, cu, cv, du[k], dv[k]);
                m.sky[k] = lc.light.x;
                m.blk[k] = lc.light.y;
                m.col[k] = lc.col;
            }
        };

        // x[d] walks the boundaries between slices, from the -1|0 boundary up to
        // the (N-1)|N boundary, so both outer faces of the chunk are produced.
        for (x[d] = -1; x[d] < N;) {
            // --- Build the mask for this slice boundary ---------------------
            int n = 0;
            for (x[v] = 0; x[v] < N; ++x[v]) {
                for (x[u] = 0; x[u] < N; ++x[u], ++n) {
                    // Compare the block on each side of the boundary. The sample
                    // lambda reaches into the neighbouring chunk for the outermost
                    // boundaries (x[d] == -1 or N-1), so a face between two solid
                    // chunks is culled instead of emitted-and-hidden.
                    const bool a = fill(x[0], x[1], x[2]);
                    const bool b = fill(x[0] + q[0], x[1] + q[1], x[2] + q[2]);

                    Mask m;
                    if (a == b) {
                        // Both solid or both see-through: the face is hidden.
                        m.present = false;
                    } else if (a) {
                        // 'a' is the solid one; its face points toward +d (air is
                        // at x[d]+1). Only emit it if 'a' is *inside* this chunk — a
                        // solid block in the neighbour is that chunk's face to mesh,
                        // not ours (else the shared face would be emitted twice) —
                        // and only if this pass owns it (emit), so the water pass
                        // skips the terrain faces the opaque pass already drew.
                        const Block ob = sample(x[0], x[1], x[2]);
                        if (Chunk::inBounds(x[0], x[1], x[2]) && emit(ob)) {
                            setFace(m, ob.id, true, x[d] + 1, x[u], x[v]);
                        }
                    } else {
                        // 'b' is the solid one; its face points toward -d (air is at
                        // x[d]). Same ownership + emit rules as above.
                        const int bx = x[0] + q[0], by = x[1] + q[1], bz = x[2] + q[2];
                        const Block ob = sample(bx, by, bz);
                        if (Chunk::inBounds(bx, by, bz) && emit(ob)) {
                            setFace(m, ob.id, false, x[d], x[u], x[v]);
                        }
                    }
                    mask[n] = m;
                }
            }

            ++x[d]; // mask now describes faces lying on plane x[d]

            // --- Greedily merge equal mask cells into maximal rectangles ----
            n = 0;
            for (int j = 0; j < N; ++j) {
                for (int i = 0; i < N;) {
                    if (!mask[n].present) {
                        ++i;
                        ++n;
                        continue;
                    }

                    // Grow width along u while cells match.
                    int w = 1;
                    while (i + w < N && mask[n + w] == mask[n]) {
                        ++w;
                    }

                    // Grow height along v while every cell in the next row matches.
                    int h = 1;
                    bool stop = false;
                    while (j + h < N && !stop) {
                        for (int k = 0; k < w; ++k) {
                            if (mask[n + k + h * N] != mask[n]) {
                                stop = true;
                                break;
                            }
                        }
                        if (!stop) ++h;
                    }

                    addQuad(d, u, v, x[d], i, j, w, h, mask[n]);

                    // Mark the merged region consumed.
                    for (int l = 0; l < h; ++l) {
                        for (int k = 0; k < w; ++k) {
                            mask[n + k + l * N].present = false;
                        }
                    }

                    i += w;
                    n += w;
                }
            }
        }
    }
    }; // end sweep

    // Pass 1: opaque terrain + (opaque) lava -> the opaque batch.
    sweep(fillOpaque, emitAny, mesh.vertices, mesh.indices);
    // Pass 2: translucent SOURCE water surfaces -> the water batch (the renderer
    // alpha-blends it over the terrain drawn behind it). Flowing water is added
    // per-cell in the non-cube pass below.
    sweep(fillWater, emitWater, mesh.waterVertices, mesh.waterIndices);

    // -------------------------------------------------------------------------
    //  Second pass: non-cube blocks (Cross plants, Model posts).
    //  These bypass greedy meshing and emit their own geometry. They are
    //  non-opaque, so the sweep above already skipped them (and meshed the solid
    //  ground beneath them normally). Each is lit flatly by its own cell's light.
    // -------------------------------------------------------------------------
    // Append one double-sided quad (front + back winding, so it shows under
    // back-face culling). Corners go bottom-left, bottom-right, top-right,
    // top-left. Two UV modes:
    //   * FIT  — the texture maps 0..1 across the whole quad (one tile per quad).
    //            For sprite-like CROSS foliage planes (diagonal/vertical quads
    //            whose extent isn't a clean block axis); the sprite is authored
    //            to fill the quad.
    //   * TILED — block-unit faceUV() per corner, so the image keeps a true
    //            16-px-per-block density regardless of the quad's size (posts,
    //            slabs, stairs, walls, leaf-cube faces, the liquid surface).
    auto pushQuad = [&](std::vector<Vertex>& outV, std::vector<uint32_t>& outI,
                        const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2,
                        const glm::vec3& p3, uint32_t layer, const glm::vec2& lv,
                        uint32_t normal, uint32_t blockCol, bool tiled,
                        uint32_t tintc = 0xFFFFFFFFu) {
        const auto b = static_cast<uint32_t>(outV.size());
        if (tiled) {
            const int axis = static_cast<int>(normal) / 2; // Face -> axis (0/1/2)
            outV.push_back({p0, faceUV(p0, axis), layer, lv, normal, blockCol, tintc});
            outV.push_back({p1, faceUV(p1, axis), layer, lv, normal, blockCol, tintc});
            outV.push_back({p2, faceUV(p2, axis), layer, lv, normal, blockCol, tintc});
            outV.push_back({p3, faceUV(p3, axis), layer, lv, normal, blockCol, tintc});
        } else {
            outV.push_back({p0, {0.0f, 1.0f}, layer, lv, normal, blockCol, tintc});
            outV.push_back({p1, {1.0f, 1.0f}, layer, lv, normal, blockCol, tintc});
            outV.push_back({p2, {1.0f, 0.0f}, layer, lv, normal, blockCol, tintc});
            outV.push_back({p3, {0.0f, 0.0f}, layer, lv, normal, blockCol, tintc});
        }
        outI.insert(outI.end(),
                    {b + 0, b + 1, b + 2, b + 0, b + 2, b + 3,    // front
                     b + 0, b + 2, b + 1, b + 0, b + 3, b + 2});  // back
    };
    // Sprite-fit quad: CROSS foliage planes (texture stretched 0..1 to fill).
    auto addNonCubeQuad = [&](const glm::vec3& p0, const glm::vec3& p1,
                              const glm::vec3& p2, const glm::vec3& p3,
                              uint32_t layer, const glm::vec2& lv, uint32_t normal,
                              uint32_t blockCol = 0, uint32_t tintc = 0xFFFFFFFFu) {
        pushQuad(mesh.vertices, mesh.indices, p0, p1, p2, p3, layer, lv, normal,
                 blockCol, /*tiled=*/false, tintc);
    };
    // Axis-aligned box face: posts, slabs, stairs, walls, leaf-cube faces. The
    // texture tiles at 16 px/block so partial-size faces show the matching slice.
    auto addBoxQuad = [&](const glm::vec3& p0, const glm::vec3& p1,
                          const glm::vec3& p2, const glm::vec3& p3,
                          uint32_t layer, const glm::vec2& lv, uint32_t normal,
                          uint32_t blockCol = 0, uint32_t tintc = 0xFFFFFFFFu) {
        pushQuad(mesh.vertices, mesh.indices, p0, p1, p2, p3, layer, lv, normal,
                 blockCol, /*tiled=*/true, tintc);
    };

    for (int z = 0; z < N; ++z) {
        for (int y = 0; y < N; ++y) {
            for (int x = 0; x < N; ++x) {
                const Block cell = chunk.get(x, y, z);
                const uint16_t id = cell.id;
                if (id == 0) continue;
                const RenderType rt = reg.renderType(id);
                // Flowing liquid (metadata > 0) is a Cube-type block but renders here
                // as a partial-height box; a reshaped cube (slab/stairs/post/wall)
                // likewise emits its own boxes here; everything else Cube is greedy.
                const bool flowing = isFlowing(cell);
                const ShapeKind shape = cellShape(cell);
                const bool shaped = shape != ShapeKind::Cube;
                if (rt == RenderType::Cube && !flowing && !shaped) continue;

                // Flat per-cell light (no AO). Sky term is lit dynamically in the
                // shader against the sun direction (via the vertex normal); the
                // block term carries the directional shade like cube faces do.
                const LightSample ls = light(x, y, z);
                const float sky = static_cast<float>(ls.sky) / 15.0f;
                const float blk = static_cast<float>(ls.block) / 15.0f;
                const uint32_t col = packColorRGBA8(ls.blockColor); // emitter hue for this cell
                const glm::vec3 o(static_cast<float>(x), static_cast<float>(y),
                                  static_cast<float>(z));
                // One texture-variant choice for the whole cell (a hash of its world
                // position), so a multi-texture non-cube block — foliage cross/leaf
                // cubes — varies per block. `fl` resolves a face to its variant layer;
                // single-variant faces ignore the selector, so this is a no-op there.
                const uint32_t cellVariant = variantHash(worldOrigin.x + x,
                                                         worldOrigin.y + y,
                                                         worldOrigin.z + z);
                auto fl = [&](Face f) { return reg.faceLayer(id, f, cellVariant); };
                // Biome vegetation tint for this cell's foliage (white for non-foliage,
                // e.g. trunks/cactus/flowers, so only leaves & leafy plants tint).
                const uint32_t cellTint = packColorRGBA8(tint(x, z, id));

                if (flowing) {
                    // Corner-connected liquid surface (issue: invisible sides /
                    // stair-step puddles): every top corner is raised to the MAX fluid
                    // height of the up-to-4 liquid cells meeting at it, so neighbouring
                    // flowing cells — and the full-height source/ocean — share corner
                    // heights and form ONE continuous sloped sheet. Interior cells then
                    // contribute only their (sloped) top; a vertical side wall shows
                    // only at the puddle's edge, where water meets air.
                    auto fluidH = [&](int X, int Y, int Z) -> float {
                        const Block b = sample(X, Y, Z);
                        if (!isWater(b.id)) return -1.0f;                  // not water
                        if (isWater(sample(X, Y + 1, Z).id)) return 1.0f; // column → full
                        if (b.metadata == 0) return 1.0f;                 // source → full
                        return 1.0f - static_cast<float>(b.metadata) / 8.0f;
                    };
                    const float ownH = 1.0f - static_cast<float>(cell.metadata) / 8.0f;
                    auto cornerH = [&](int ix, int iz) -> float {
                        float m = -1.0f;
                        for (int ax = ix - 1; ax <= ix; ++ax)
                            for (int az = iz - 1; az <= iz; ++az)
                                m = std::max(m, fluidH(x + ax, y, z + az));
                        return (m >= 0.0f) ? m : ownH; // shared corner, so neighbours agree
                    };
                    const float h00 = cornerH(0, 0), h10 = cornerH(1, 0);
                    const float h11 = cornerH(1, 1), h01 = cornerH(0, 1);

                    // Liquid is opaque to the light flood, so this cell's own light is
                    // dark — take the brightest open neighbour (usually the air above)
                    // so the surface is lit like the cube faces around it are.
                    LightSample lq = ls;
                    const int ndx[6] = {0, 0, 1, -1, 0, 0};
                    const int ndy[6] = {1, -1, 0, 0, 0, 0};
                    const int ndz[6] = {0, 0, 0, 0, 1, -1};
                    for (int k = 0; k < 6; ++k) {
                        const LightSample n = light(x + ndx[k], y + ndy[k], z + ndz[k]);
                        if (n.sky > lq.sky) lq.sky = n.sky;
                        if (n.block > lq.block) { lq.block = n.block; lq.blockColor = n.blockColor; }
                    }
                    const float lsky = static_cast<float>(lq.sky) / 15.0f;
                    const float lblk = static_cast<float>(lq.block) / 15.0f;
                    const uint32_t lcol = packColorRGBA8(lq.blockColor);
                    // Flowing water joins the translucent water batch (alpha-blended);
                    // flowing lava is opaque, so it stays in the opaque batch.
                    std::vector<Vertex>&   tv = isWater(id) ? mesh.waterVertices : mesh.vertices;
                    std::vector<uint32_t>& ti = isWater(id) ? mesh.waterIndices  : mesh.indices;
                    auto emitFace = [&](const glm::vec3& a, const glm::vec3& b,
                                        const glm::vec3& c, const glm::vec3& d,
                                        int axis, bool pos, Face f) {
                        pushQuad(tv, ti, o + a, o + b, o + c, o + d, fl(f),
                                 glm::vec2(lsky, faceShade(axis, pos) * lblk),
                                 static_cast<uint32_t>(f), lcol, /*tiled=*/true);
                    };
                    // A face is hidden where it meets another liquid (surfaces connect)
                    // or an opaque block (terrain); only water-vs-air shows.
                    auto open = [&](int dx, int dy, int dz) {
                        const Block nb = sample(x + dx, y + dy, z + dz);
                        return !(isLiquid(nb.id) || reg.isOpaque(nb.id));
                    };
                    // Top surface — corners follow the connected heights. Hidden under a
                    // liquid column or a solid overhang; shown when open air is above.
                    if (open(0, 1, 0))
                        emitFace({0, h00, 0}, {0, h01, 1}, {1, h11, 1}, {1, h10, 0}, 1, true, FacePosY);
                    // Side walls — each top edge follows that edge's two corner heights.
                    if (open(-1, 0, 0))
                        emitFace({0, 0, 0}, {0, 0, 1}, {0, h01, 1}, {0, h00, 0}, 0, false, FaceNegX);
                    if (open(1, 0, 0))
                        emitFace({1, 0, 1}, {1, 0, 0}, {1, h10, 0}, {1, h11, 1}, 0, true, FacePosX);
                    if (open(0, 0, -1))
                        emitFace({1, 0, 0}, {0, 0, 0}, {0, h00, 0}, {1, h10, 0}, 2, false, FaceNegZ);
                    if (open(0, 0, 1))
                        emitFace({0, 0, 1}, {1, 0, 1}, {1, h11, 1}, {0, h01, 1}, 2, true, FacePosZ);
                    // Bottom — only when the cell below is open (water hanging in air).
                    if (open(0, -1, 0))
                        emitFace({0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}, 1, false, FaceNegY);
                } else if (shaped) {
                    // A reshaped cube (slab/stairs/post/wall): build the shape's box
                    // union (the SAME boxes collision uses) and emit each box's faces.
                    // A face flush on the cell boundary is culled only when the
                    // neighbour there is a solid full cube; interior faces always draw.
                    uint8_t wallMask = 0;
                    if (shape == ShapeKind::Wall) {
                        auto conn = [&](int nx, int ny, int nz) {
                            const Block nb = sample(x + nx, y + ny, z + nz);
                            return fillOpaque(x + nx, y + ny, z + nz) ||
                                   (reg.shapeable(nb.id) &&
                                    shapeKindOf(nb.metadata) == ShapeKind::Wall);
                        };
                        if (conn(0, 0, -1)) wallMask |= 0x1;
                        if (conn(1, 0, 0))  wallMask |= 0x2;
                        if (conn(0, 0, 1))  wallMask |= 0x4;
                        if (conn(-1, 0, 0)) wallMask |= 0x8;
                    }
                    // An opaque shaped block holds no light in its own cell, so light
                    // it by the brightest open neighbour (like cube faces / the liquid
                    // surface) instead of its dark own-cell value.
                    LightSample sl = ls;
                    const int sndx[6] = {0, 0, 1, -1, 0, 0};
                    const int sndy[6] = {1, -1, 0, 0, 0, 0};
                    const int sndz[6] = {0, 0, 0, 0, 1, -1};
                    for (int k = 0; k < 6; ++k) {
                        const LightSample n = light(x + sndx[k], y + sndy[k], z + sndz[k]);
                        if (n.sky > sl.sky) sl.sky = n.sky;
                        if (n.block > sl.block) { sl.block = n.block; sl.blockColor = n.blockColor; }
                    }
                    const float ssky = static_cast<float>(sl.sky) / 15.0f;
                    const float sblk = static_cast<float>(sl.block) / 15.0f;
                    const uint32_t scol = packColorRGBA8(sl.blockColor);

                    std::vector<ShapeBox> boxes;
                    shapeBoxes(shape, shapeOrientOf(cell.metadata), wallMask, boxes);
                    auto faceLv = [&](int axis, bool pos) {
                        return glm::vec2(ssky, faceShade(axis, pos) * sblk);
                    };
                    for (const ShapeBox& bx : boxes) {
                        const glm::vec3 lo = bx.lo, hi = bx.hi;
                        if (!(lo.x == 0.0f && fillOpaque(x - 1, y, z)))
                            addBoxQuad(o + glm::vec3(lo.x, lo.y, lo.z), o + glm::vec3(lo.x, lo.y, hi.z),
                                       o + glm::vec3(lo.x, hi.y, hi.z), o + glm::vec3(lo.x, hi.y, lo.z),
                                       fl(FaceNegX), faceLv(0, false),
                                       static_cast<uint32_t>(FaceNegX), scol);
                        if (!(hi.x == 1.0f && fillOpaque(x + 1, y, z)))
                            addBoxQuad(o + glm::vec3(hi.x, lo.y, hi.z), o + glm::vec3(hi.x, lo.y, lo.z),
                                       o + glm::vec3(hi.x, hi.y, lo.z), o + glm::vec3(hi.x, hi.y, hi.z),
                                       fl(FacePosX), faceLv(0, true),
                                       static_cast<uint32_t>(FacePosX), scol);
                        if (!(lo.z == 0.0f && fillOpaque(x, y, z - 1)))
                            addBoxQuad(o + glm::vec3(hi.x, lo.y, lo.z), o + glm::vec3(lo.x, lo.y, lo.z),
                                       o + glm::vec3(lo.x, hi.y, lo.z), o + glm::vec3(hi.x, hi.y, lo.z),
                                       fl(FaceNegZ), faceLv(2, false),
                                       static_cast<uint32_t>(FaceNegZ), scol);
                        if (!(hi.z == 1.0f && fillOpaque(x, y, z + 1)))
                            addBoxQuad(o + glm::vec3(lo.x, lo.y, hi.z), o + glm::vec3(hi.x, lo.y, hi.z),
                                       o + glm::vec3(hi.x, hi.y, hi.z), o + glm::vec3(lo.x, hi.y, hi.z),
                                       fl(FacePosZ), faceLv(2, true),
                                       static_cast<uint32_t>(FacePosZ), scol);
                        if (!(hi.y == 1.0f && fillOpaque(x, y + 1, z)))
                            addBoxQuad(o + glm::vec3(lo.x, hi.y, lo.z), o + glm::vec3(lo.x, hi.y, hi.z),
                                       o + glm::vec3(hi.x, hi.y, hi.z), o + glm::vec3(hi.x, hi.y, lo.z),
                                       fl(FacePosY), faceLv(1, true),
                                       static_cast<uint32_t>(FacePosY), scol);
                        if (!(lo.y == 0.0f && fillOpaque(x, y - 1, z)))
                            addBoxQuad(o + glm::vec3(lo.x, lo.y, lo.z), o + glm::vec3(hi.x, lo.y, lo.z),
                                       o + glm::vec3(hi.x, lo.y, hi.z), o + glm::vec3(lo.x, lo.y, hi.z),
                                       fl(FaceNegY), faceLv(1, false),
                                       static_cast<uint32_t>(FaceNegY), scol);
                    }
                } else if (rt == RenderType::Cross || rt == RenderType::LeafCube) {
                    // Two diagonal quads forming an X across the cell. Lit as if
                    // top-facing (FacePosY) so foliage tracks the sun's elevation.
                    const uint32_t layer = fl(FacePosX);
                    const glm::vec2 lv(sky, blk); // baseShade 1.0 for foliage
                    const uint32_t nrm = static_cast<uint32_t>(FacePosY);
                    // Ground plants (grass/bush/torch) fill exactly the cell, so the
                    // sprite reads at its native size instead of being blown up and
                    // blurry. Leaves keep a modest crown spilling past the cube faces
                    // (incl. below) so the canopy still looks full and bushy.
                    const bool leaf = (rt == RenderType::LeafCube);
                    const float e  = leaf ? 0.22f : 0.0f;         // horizontal overhang
                    const float p0 = -e, p1 = 1.0f + e;
                    const float pb = leaf ? -e : 0.0f;            // bottom (leaves spill down)
                    const float pt = leaf ? 1.0f + e : 1.0f;      // top overhang (plants flush)
                    addNonCubeQuad(o + glm::vec3(p0, pb, p0), o + glm::vec3(p1, pb, p1),
                                   o + glm::vec3(p1, pt, p1), o + glm::vec3(p0, pt, p0),
                                   layer, lv, nrm, col, cellTint);
                    addNonCubeQuad(o + glm::vec3(p0, pb, p1), o + glm::vec3(p1, pb, p0),
                                   o + glm::vec3(p1, pt, p0), o + glm::vec3(p0, pt, p1),
                                   layer, lv, nrm, col, cellTint);
                    if (rt == RenderType::LeafCube) {
                        // ...plus the full voxel-cube faces, so the canopy reads as
                        // solid leaf blocks with the cross fronds poking through.
                        // Cull a face against opaque neighbours and against other
                        // leaf cells of the same id (interior faces of a merged crown).
                        auto leafFace = [&](int nx, int ny, int nz, const glm::vec3& a,
                                            const glm::vec3& b, const glm::vec3& c,
                                            const glm::vec3& dd, int axis, bool pos, Face f) {
                            const uint16_t nb = sample(x + nx, y + ny, z + nz).id;
                            if (nb == id || reg.isOpaque(nb)) return;
                            addBoxQuad(o + a, o + b, o + c, o + dd, fl(f),
                                       glm::vec2(sky, faceShade(axis, pos) * blk),
                                       static_cast<uint32_t>(f), col, cellTint);
                        };
                        leafFace(-1, 0, 0, glm::vec3(0, 0, 0), glm::vec3(0, 0, 1),
                                 glm::vec3(0, 1, 1), glm::vec3(0, 1, 0), 0, false, FaceNegX);
                        leafFace(1, 0, 0, glm::vec3(1, 0, 1), glm::vec3(1, 0, 0),
                                 glm::vec3(1, 1, 0), glm::vec3(1, 1, 1), 0, true, FacePosX);
                        leafFace(0, 0, -1, glm::vec3(1, 0, 0), glm::vec3(0, 0, 0),
                                 glm::vec3(0, 1, 0), glm::vec3(1, 1, 0), 2, false, FaceNegZ);
                        leafFace(0, 0, 1, glm::vec3(0, 0, 1), glm::vec3(1, 0, 1),
                                 glm::vec3(1, 1, 1), glm::vec3(0, 1, 1), 2, true, FacePosZ);
                        leafFace(0, 1, 0, glm::vec3(0, 1, 0), glm::vec3(0, 1, 1),
                                 glm::vec3(1, 1, 1), glm::vec3(1, 1, 0), 1, true, FacePosY);
                        leafFace(0, -1, 0, glm::vec3(0, 0, 0), glm::vec3(1, 0, 0),
                                 glm::vec3(1, 0, 1), glm::vec3(0, 0, 1), 1, false, FaceNegY);
                    }
                } else { // RenderType::Model — a centred thin box (e.g. trunk)
                    const float lo = reg.modelInset(id), hi = 1.0f - lo;
                    // Per-face block-light shade so the box still reads as 3D.
                    auto faceLv = [&](int axis, bool pos) {
                        return glm::vec2(sky, faceShade(axis, pos) * blk);
                    };
                    // Side faces (always visible — the box is thinner than the cell).
                    addBoxQuad(o + glm::vec3(lo, 0, lo), o + glm::vec3(lo, 0, hi),
                               o + glm::vec3(lo, 1, hi), o + glm::vec3(lo, 1, lo),
                               fl(FaceNegX), faceLv(0, false),
                               static_cast<uint32_t>(FaceNegX), col);
                    addBoxQuad(o + glm::vec3(hi, 0, hi), o + glm::vec3(hi, 0, lo),
                               o + glm::vec3(hi, 1, lo), o + glm::vec3(hi, 1, hi),
                               fl(FacePosX), faceLv(0, true),
                               static_cast<uint32_t>(FacePosX), col);
                    addBoxQuad(o + glm::vec3(hi, 0, lo), o + glm::vec3(lo, 0, lo),
                               o + glm::vec3(lo, 1, lo), o + glm::vec3(hi, 1, lo),
                               fl(FaceNegZ), faceLv(2, false),
                               static_cast<uint32_t>(FaceNegZ), col);
                    addBoxQuad(o + glm::vec3(lo, 0, hi), o + glm::vec3(hi, 0, hi),
                               o + glm::vec3(hi, 1, hi), o + glm::vec3(lo, 1, hi),
                               fl(FacePosZ), faceLv(2, true),
                               static_cast<uint32_t>(FacePosZ), col);
                    // Top/bottom caps only where the neighbour isn't the same post
                    // (so a stacked trunk has no internal caps) and isn't opaque.
                    const bool capAbove = sample(x, y + 1, z).id != id &&
                                          !reg.isOpaque(sample(x, y + 1, z).id);
                    const bool capBelow = sample(x, y - 1, z).id != id &&
                                          !reg.isOpaque(sample(x, y - 1, z).id);
                    if (capAbove) {
                        addBoxQuad(o + glm::vec3(lo, 1, lo), o + glm::vec3(lo, 1, hi),
                                   o + glm::vec3(hi, 1, hi), o + glm::vec3(hi, 1, lo),
                                   fl(FacePosY), faceLv(1, true),
                                   static_cast<uint32_t>(FacePosY), col);
                    }
                    if (capBelow) {
                        addBoxQuad(o + glm::vec3(lo, 0, lo), o + glm::vec3(hi, 0, lo),
                                   o + glm::vec3(hi, 0, hi), o + glm::vec3(lo, 0, hi),
                                   fl(FaceNegY), faceLv(1, false),
                                   static_cast<uint32_t>(FaceNegY), col);
                    }
                }
            }
        }
    }

    return mesh;
}

} // namespace vg
