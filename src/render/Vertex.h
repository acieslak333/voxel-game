#pragma once

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace vg {

// -----------------------------------------------------------------------------
//  Vertex
// -----------------------------------------------------------------------------
//  One vertex of chunk geometry.
//
//   * uv is measured in *block units*, not 0..1. A greedy-meshed quad spanning
//     N blocks gets uv up to N, and the sampler uses REPEAT addressing, so the
//     texture tiles once per block instead of stretching across the whole quad.
//     This is the crux of combining greedy meshing with textures.
//   * layer selects which slice of the 2D texture *array* to sample.
//   * light is per-vertex brightness split by source: x = sky-lit, y = block-lit
//     (each already includes ambient occlusion). Keeping them separate lets the
//     shader tint sky light and block light different colours. The rasteriser
//     interpolates both, giving the soft "smooth lighting" gradient.
//   * normal is the face index (the Face enum, 0..5); the shader decodes it to a
//     unit normal and lights the face against the *current* sun/moon direction,
//     so shading sweeps around with the time of day without remeshing.
//   * blockColor is the *hue* of the block light reaching this vertex, packed
//     RGBA8 (the dominant nearby emitter's colour — a warm torch vs orange lava).
//     The shader tints the block-lit term by it, so different emitters glow in
//     different colours. Only meaningful where the block-light term (light.y) is
//     non-zero; elsewhere it is black and the shader ignores it.
// -----------------------------------------------------------------------------
struct Vertex {
    glm::vec3 pos;
    glm::vec2 uv;
    uint32_t  layer;
    glm::vec2 light;
    uint32_t  normal;
    uint32_t  blockColor = 0; // packed RGBA8 emitter hue (R low byte)
    // Biome vegetation tint, packed RGBA8 (white = no tint). The fragment shader
    // multiplies the sampled albedo by this, so grass/leaves/plants take on a
    // per-biome colour (lush forest, dry savanna, pale snow, dark swamp) while all
    // other blocks carry the default white and are unaffected.
    uint32_t  tint = 0xFFFFFFFFu;

    // How to fetch one Vertex from the vertex buffer.
    static VkVertexInputBindingDescription bindingDescription() {
        VkVertexInputBindingDescription b{};
        b.binding   = 0;
        b.stride    = sizeof(Vertex);
        b.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return b;
    }

    // How to interpret each field as a shader input attribute.
    static std::array<VkVertexInputAttributeDescription, 7> attributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 7> a{};
        a[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT,  offsetof(Vertex, pos)};
        a[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT,     offsetof(Vertex, uv)};
        a[2] = {2, 0, VK_FORMAT_R32_UINT,          offsetof(Vertex, layer)};
        a[3] = {3, 0, VK_FORMAT_R32G32_SFLOAT,     offsetof(Vertex, light)};
        a[4] = {4, 0, VK_FORMAT_R32_UINT,          offsetof(Vertex, normal)};
        // R8G8B8A8_UNORM: read in the shader as a normalised vec4 in [0,1].
        a[5] = {5, 0, VK_FORMAT_R8G8B8A8_UNORM,    offsetof(Vertex, blockColor)};
        a[6] = {6, 0, VK_FORMAT_R8G8B8A8_UNORM,    offsetof(Vertex, tint)};
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

} // namespace vg
