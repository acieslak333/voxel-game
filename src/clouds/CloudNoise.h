#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace vg {

class VulkanContext;

// -----------------------------------------------------------------------------
//  CloudNoise
// -----------------------------------------------------------------------------
//  The two tiling 3D noise fields the cloud system is built on:
//    * base   — Perlin-Worley (billowy connected shapes), low frequency;
//    * detail — Worley fBm (erosion / cauliflower edges), higher frequency.
//  Both are generated once on the CPU (cached to disk because Debug generation
//  takes a few seconds) and uploaded as repeating 3D textures for the
//  volumetric raymarch.
// -----------------------------------------------------------------------------
class CloudNoise {
public:
    static constexpr int kBaseSize   = 64; // base texture edge (voxels)
    static constexpr int kDetailSize = 32; // detail texture edge

    // Generates (or loads from `cacheFile`) and uploads the textures.
    CloudNoise(VulkanContext& ctx, const std::string& cacheFile);
    ~CloudNoise();

    CloudNoise(const CloudNoise&) = delete;
    CloudNoise& operator=(const CloudNoise&) = delete;

    [[nodiscard]] VkImageView baseView()    const { return baseView_; }
    [[nodiscard]] VkImageView detailView()  const { return detailView_; }
    [[nodiscard]] VkSampler   sampler()     const { return sampler_; }

private:
    void generate();
    bool loadCache(const std::string& file);
    void saveCache(const std::string& file) const;
    void upload3D(const std::vector<uint8_t>& voxels, int size, VkImage& image,
                  VkDeviceMemory& memory, VkImageView& view);

    VulkanContext& ctx_;

    std::vector<uint8_t> base_;   // kBaseSize^3, R8 (retained for the disk cache)
    std::vector<uint8_t> detail_; // kDetailSize^3, R8

    VkImage        baseImage_  = VK_NULL_HANDLE;
    VkDeviceMemory baseMem_    = VK_NULL_HANDLE;
    VkImageView    baseView_   = VK_NULL_HANDLE;
    VkImage        detailImage_ = VK_NULL_HANDLE;
    VkDeviceMemory detailMem_   = VK_NULL_HANDLE;
    VkImageView    detailView_  = VK_NULL_HANDLE;
    VkSampler      sampler_     = VK_NULL_HANDLE;
};

} // namespace vg
