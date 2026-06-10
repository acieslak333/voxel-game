#pragma once

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include <array>

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
// -----------------------------------------------------------------------------
struct Vertex {
    glm::vec3 pos;
    glm::vec2 uv;
    uint32_t  layer;
    glm::vec2 light;
    uint32_t  normal;

    // How to fetch one Vertex from the vertex buffer.
    static VkVertexInputBindingDescription bindingDescription() {
        VkVertexInputBindingDescription b{};
        b.binding   = 0;
        b.stride    = sizeof(Vertex);
        b.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return b;
    }

    // How to interpret each field as a shader input attribute.
    static std::array<VkVertexInputAttributeDescription, 5> attributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 5> a{};
        a[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos)};
        a[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(Vertex, uv)};
        a[2] = {2, 0, VK_FORMAT_R32_UINT,         offsetof(Vertex, layer)};
        a[3] = {3, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(Vertex, light)};
        a[4] = {4, 0, VK_FORMAT_R32_UINT,         offsetof(Vertex, normal)};
        return a;
    }
};

} // namespace vg
