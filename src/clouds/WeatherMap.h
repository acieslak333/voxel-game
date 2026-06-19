#pragma once

/**
 * @file WeatherMap.h
 * @brief Declares WeatherMap, a static 2D spatial-variation field for the cloud system.
 *
 * Generates a seed-driven 64x64 R8G8 texture holding per-cell signed offsets for
 * cloud coverage (R) and type (G) around the global base values that CloudSystem
 * evolves over time. The texture is uploaded once and sampled with REPEAT so wind
 * scroll can drift it indefinitely.
 * @see docs/CODE_INDEX.md
 */

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace vg {

class VulkanContext;

/**
 * @brief Static 2D weather variation field for the volumetric cloud shader.
 *
 * Two independent value-noise fields (coverage and type variation) are packed into
 * an R8G8 texture at kSize x kSize resolution. The shader subtracts 0.5 to recover
 * signed offsets from the global base values. Static for the session; wind scroll
 * and base values are uniforms in the sky UBO.
 */
class WeatherMap {
public:
    static constexpr int kSize = 64; ///< Texture resolution (cells per edge).

    /**
     * @brief Generate and upload the weather field.
     * @param ctx   Vulkan device context.
     * @param seed  World seed; produces a deterministic but unique spatial pattern.
     */
    WeatherMap(VulkanContext& ctx, uint32_t seed);
    ~WeatherMap();

    WeatherMap(const WeatherMap&) = delete;
    WeatherMap& operator=(const WeatherMap&) = delete;

    /// R8G8 2D image view, sampled by the cloud shader to retrieve spatial variation.
    [[nodiscard]] VkImageView view() const { return view_; }

private:
    VulkanContext& ctx_;

    VkImage        image_  = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView    view_   = VK_NULL_HANDLE;
};

} // namespace vg
