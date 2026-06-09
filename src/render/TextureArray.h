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
//  TODO(future): mipmaps, and a path for blocks that use custom models rather
//  than cube-face textures (e.g. a furnace) — those would register differently.
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

    VulkanContext* ctx_ = nullptr;

    VkImage        image_   = VK_NULL_HANDLE;
    VkDeviceMemory memory_  = VK_NULL_HANDLE;
    VkImageView    view_    = VK_NULL_HANDLE;
    VkSampler      sampler_ = VK_NULL_HANDLE;
};

} // namespace vg
