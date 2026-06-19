#pragma once

/**
 * @file Vertex.h
 * @brief Packed 24-byte chunk vertex layout and helper packing utilities.
 *
 * Defines the Vertex struct used by the greedy mesher and the chunk draw pipeline.
 * The layout (pos, uv, layer, light, normal, blockColor, tint) is fixed and MUST
 * match the attribute descriptions returned by attributeDescriptions(); any change
 * to the struct fields requires a matching update to that function and to the shaders.
 *
 * Also provides toHalf(), toUnorm8(), packColorRGBA8(), and withAlpha() — small
 * inline helpers for packing linear float values into the GPU-friendly formats.
 * @warning The Vertex memory layout must stay byte-identical to attributeDescriptions().
 * @see docs/CODE_INDEX.md
 */

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp> // packHalf2x16

#include <array>
#include <cstdint>

namespace vg {

/// @brief Convert a 32-bit float to IEEE 754 half precision (16-bit).
// Float -> IEEE half (16-bit). packHalf2x16 packs (x,y) with x in the low 16 bits,
// so masking off the low half gives the half-encoding of a single float.
[[nodiscard]] inline uint16_t toHalf(float f) {
    return static_cast<uint16_t>(glm::packHalf2x16(glm::vec2(f, 0.0f)) & 0xFFFFu);
}
/// @brief Clamp a float to [0,1] and quantise to an 8-bit unsigned normalised integer.
[[nodiscard]] inline uint8_t toUnorm8(float f) {
    const float c = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    return static_cast<uint8_t>(c * 255.0f + 0.5f);
}

// -----------------------------------------------------------------------------
//  Vertex
// -----------------------------------------------------------------------------
//  One vertex of chunk geometry, packed to 24 bytes (was 44). The mesher still
//  constructs it from the same logical fields (the packing constructor below has
//  the same argument order the call sites already use); the GPU vertex-input
//  formats unpack each field, so neither the mesher nor the shaders change.
//
//   * pos  — chunk-local position, half-float. The chunk is 16 blocks plus small
//     sub-block shape fractions/overhangs (slabs, leaf crowns), all well within
//     half precision (~0.008-block error at 16).
//   * uv   — block-unit UV (tiles via REPEAT), half-float.
//   * layer— texture-array slice (uint16).
//   * light— x = the per-corner ambient-occlusion factor (the fragment shader
//     multiplies it into the sky+block light it samples from the light atlas, S7);
//     y is unused. UNORM8.
//   * normal — low 3 bits = Face index (0..5); bit 3 (|8) marks an atlas-lit cube
//     face. Decoded in the shader against the time-of-day sun. UINT8.
//   * blockColor — packed RGBA8 block-light hue (kept per-vertex; only meaningful
//     near emitters and for non-cube geometry).
//   * tint — biome vegetation tint, packed RGBA8 (white = none; alpha < 1 marks
//     swayable foliage).
// -----------------------------------------------------------------------------
/**
 * @brief Packed 24-byte chunk vertex consumed by the chunk draw pipeline.
 *
 * Fields are packed to GPU-native formats (half-float, UNORM8, UINT8, RGBA8).
 * The packing constructor accepts the same float/vec arguments the mesher
 * already uses, converting them transparently.
 * @warning The struct layout must stay byte-identical to attributeDescriptions().
 */
struct Vertex {
    uint16_t pos[3];      // half-float chunk-local position
    uint16_t uv[2];       // half-float block-unit UV
    uint16_t layer;       // texture-array slice
    uint8_t  light[2];    // x = AO factor (UNORM8), y unused
    uint8_t  normal;      // Face index | (atlas-lit << 3)
    uint8_t  pad;         // keep blockColor 4-byte aligned
    uint32_t blockColor;  // RGBA8 emitter hue (R low byte)
    uint32_t tint;        // RGBA8 biome tint (white = none)

    Vertex() = default;
    // Same argument order as the old aggregate fields, so every existing
    // push_back({pos, uv, layer, light, normal, color, tint}) site calls this and
    // packs transparently.
    Vertex(const glm::vec3& p, const glm::vec2& u, uint32_t lyr, const glm::vec2& lt,
           uint32_t nrm, uint32_t color = 0, uint32_t tnt = 0xFFFFFFFFu)
        : pos{toHalf(p.x), toHalf(p.y), toHalf(p.z)},
          uv{toHalf(u.x), toHalf(u.y)},
          layer(static_cast<uint16_t>(lyr)),
          light{toUnorm8(lt.x), toUnorm8(lt.y)},
          normal(static_cast<uint8_t>(nrm)),
          pad(0),
          blockColor(color),
          tint(tnt) {}

    // How to fetch one Vertex from the vertex buffer.
    static VkVertexInputBindingDescription bindingDescription() {
        VkVertexInputBindingDescription b{};
        b.binding   = 0;
        b.stride    = sizeof(Vertex);
        b.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return b;
    }

    // How to interpret each packed field as a shader input attribute. The shader
    // declarations (vec3 inPos, vec2 inUV, uint inLayer, vec2 inLight, uint
    // inNormal, vec4 inBlockColor, vec4 inTint) are unchanged — the format does the
    // widening (half->float, unorm8->float, uint8->uint).
    static std::array<VkVertexInputAttributeDescription, 7> attributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 7> a{};
        a[0] = {0, 0, VK_FORMAT_R16G16B16_SFLOAT, offsetof(Vertex, pos)};
        a[1] = {1, 0, VK_FORMAT_R16G16_SFLOAT,    offsetof(Vertex, uv)};
        a[2] = {2, 0, VK_FORMAT_R16_UINT,         offsetof(Vertex, layer)};
        a[3] = {3, 0, VK_FORMAT_R8G8_UNORM,       offsetof(Vertex, light)};
        a[4] = {4, 0, VK_FORMAT_R8_UINT,          offsetof(Vertex, normal)};
        a[5] = {5, 0, VK_FORMAT_R8G8B8A8_UNORM,   offsetof(Vertex, blockColor)};
        a[6] = {6, 0, VK_FORMAT_R8G8B8A8_UNORM,   offsetof(Vertex, tint)};
        return a;
    }
};

// Pack a linear RGB colour (0..1 per channel) into RGBA8 with R in the low byte,
// matching VK_FORMAT_R8G8B8A8_UNORM. Alpha is set opaque (unused by the shader).
[[nodiscard]] inline uint32_t packColorRGBA8(const glm::vec3& c) {
    auto q = [](float v) -> uint32_t {
        const float cl = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<uint32_t>(cl * 255.0f + 0.5f);
    };
    return q(c.r) | (q(c.g) << 8) | (q(c.b) << 16) | (0xFFu << 24);
}

// Replace the alpha byte of a packed RGBA8 value. The chunk shader reads the tint
// alpha as a "sway" marker (1.0 = rigid; < 1.0 = foliage that bends in the wind),
// so this stamps a sway amount onto a tint without touching its RGB.
[[nodiscard]] inline uint32_t withAlpha(uint32_t rgba, uint8_t a) {
    return (rgba & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24);
}

} // namespace vg
