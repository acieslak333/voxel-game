#pragma once

/**
 * @file CloudNoise.h
 * @brief Declares CloudNoise, which generates and uploads the two tiling 3D cloud noise textures.
 *
 * The base texture is 64^3 R8 Perlin-Worley (connected billowy shapes); the detail
 * texture is 32^3 R8 Worley fBm (erosion / cauliflower edges). Both are generated
 * once on the CPU, cached to disk, and uploaded as REPEAT 3D textures for the
 * volumetric cloud raymarch in sky.frag.
 * @see docs/CODE_INDEX.md
 */

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace vg {

class VulkanContext;

/**
 * @brief Generates, caches, and uploads the two tiling 3D cloud noise textures.
 *
 * Construction attempts to load @c cacheFile; on a miss it generates both volumes
 * on the CPU (a few seconds in Debug), saves the cache, then uploads. The textures
 * use tileable noise (trilinear blend of period-shifted copies) so REPEAT sampling
 * produces seamless results as wind scrolls the UV coordinates.
 */
class CloudNoise {
public:
    static constexpr int kBaseSize   = 64; ///< Base texture edge length (voxels).
    static constexpr int kDetailSize = 32; ///< Detail texture edge length (voxels).

    /**
     * @brief Load (or generate) and upload the noise textures.
     * @param ctx        Vulkan device context.
     * @param cacheFile  Path to the binary noise cache file (generated on first run).
     */
    CloudNoise(VulkanContext& ctx, const std::string& cacheFile);
    ~CloudNoise();

    CloudNoise(const CloudNoise&) = delete;
    CloudNoise& operator=(const CloudNoise&) = delete;

    /// Image view for the 64^3 Perlin-Worley base noise texture.
    [[nodiscard]] VkImageView baseView()    const { return baseView_; }
    /// Image view for the 32^3 Worley-fBm detail noise texture.
    [[nodiscard]] VkImageView detailView()  const { return detailView_; }
    /// Shared REPEAT trilinear sampler for both noise textures.
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
