#pragma once

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace vg {

class VulkanContext;

// -----------------------------------------------------------------------------
//  WeatherMap
// -----------------------------------------------------------------------------
//  A low-res 2D field over world XZ holding *spatial variation* of the weather:
//  per cell, signed offsets for (coverage, type) around the global base values
//  CloudSystem evolves over time. The pattern is seeded value-noise, static for
//  a session; wind scroll and the global bases are uniforms, so the texture is
//  uploaded once and sampled by the volumetric cloud shader.
// -----------------------------------------------------------------------------
class WeatherMap {
public:
    static constexpr int kSize = 64; // cells per edge

    WeatherMap(VulkanContext& ctx, uint32_t seed);
    ~WeatherMap();

    WeatherMap(const WeatherMap&) = delete;
    WeatherMap& operator=(const WeatherMap&) = delete;

    [[nodiscard]] VkImageView view() const { return view_; }

private:
    VulkanContext& ctx_;

    VkImage        image_  = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView    view_   = VK_NULL_HANDLE;
};

} // namespace vg
