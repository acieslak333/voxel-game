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
//   * shade is a cheap per-face brightness (top brighter than sides than
//     bottom) so the blocky geometry reads clearly without real lighting.
// -----------------------------------------------------------------------------
struct Vertex {
    glm::vec3 pos;
    glm::vec2 uv;
    uint32_t  layer;
    float     shade;

    // How to fetch one Vertex from the vertex buffer.
    static VkVertexInputBindingDescription bindingDescription() {
        VkVertexInputBindingDescription b{};
        b.binding   = 0;
        b.stride    = sizeof(Vertex);
        b.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return b;
    }

    // How to interpret each field as a shader input attribute.
    static std::array<VkVertexInputAttributeDescription, 4> attributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 4> a{};
        a[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos)};
        a[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(Vertex, uv)};
        a[2] = {2, 0, VK_FORMAT_R32_UINT,         offsetof(Vertex, layer)};
        a[3] = {3, 0, VK_FORMAT_R32_SFLOAT,       offsetof(Vertex, shade)};
        return a;
    }
};

} // namespace vg
