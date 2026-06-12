#pragma once

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

namespace vg {

class VulkanContext;

// -----------------------------------------------------------------------------
//  TextureArray
// -----------------------------------------------------------------------------
//  A VK_IMAGE_VIEW_TYPE_2D_ARRAY texture: one image with N layers, one layer per
//  source PNG. Block faces index into it by layer (see BlockRegistry).
//
//  The sampler uses REPEAT addressing so that, combined with UVs expressed in
//  block units, each block tile of a greedy-merged quad shows one full copy of
//  the texture instead of the texture being stretched across the whole quad.
//
//  All source images must share the same dimensions (array layers must match).
//
//  A capped mip chain is built at load (linear blit-down per array layer) to kill
//  the minification shimmer/moiré on distant tiled tiles; the sampler stays NEAREST
//  in-plane for the crisp voxel look. See TextureArray.cpp.
//
//  TODO(future): a path for blocks that use custom models rather than cube-face
//  textures (e.g. a furnace) — those would register differently.
// -----------------------------------------------------------------------------
class TextureArray {
public:
    // Loads `filenames` (resolved relative to `textureDir`) into the array.
    TextureArray(VulkanContext& ctx, const std::vector<std::string>& filenames,
                 const std::string& textureDir);
    ~TextureArray();

    TextureArray(const TextureArray&) = delete;
    TextureArray& operator=(const TextureArray&) = delete;

    [[nodiscard]] VkImageView view()    const { return view_; }
    [[nodiscard]] VkSampler   sampler() const { return sampler_; }

private:
    void createSampler();
    void generateMipmaps(int width, int height, uint32_t layerCount);
    static uint32_t mipLevelsFor(int width, int height);

    VulkanContext* ctx_ = nullptr;

    VkImage        image_     = VK_NULL_HANDLE;
    VkDeviceMemory memory_    = VK_NULL_HANDLE;
    VkImageView    view_      = VK_NULL_HANDLE;
    VkSampler      sampler_   = VK_NULL_HANDLE;
    uint32_t       mipLevels_ = 1;
};

} // namespace vg
