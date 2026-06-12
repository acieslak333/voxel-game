#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace vg {

class VulkanContext;

// Small free-function helpers for the repetitive image/format work shared by the
// texture array and the depth buffer. Kept out of VulkanContext so that class
// stays focused on device setup.
namespace vkutil {

// Create a (possibly array) image plus backing device memory. `mipLevels` > 1
// allocates a full mip chain (the texture array fills it via blits); the default
// of 1 keeps every other caller (depth buffers, the font atlas, ...) unchanged.
void createImage(const VulkanContext& ctx, uint32_t width, uint32_t height,
                 uint32_t arrayLayers, VkFormat format, VkImageTiling tiling,
                 VkImageUsageFlags usage, VkMemoryPropertyFlags properties,
                 VkImage& outImage, VkDeviceMemory& outMemory,
                 uint32_t mipLevels = 1);

VkImageView createImageView(VkDevice device, VkImage image, VkFormat format,
                            VkImageAspectFlags aspect, VkImageViewType viewType,
                            uint32_t layerCount, uint32_t mipLevels = 1);

// Insert a pipeline barrier that moves `image` from one layout to another. Only
// the transitions this project needs are implemented. `mipLevels` covers the
// whole chain in one barrier (used before filling mips).
void transitionImageLayout(const VulkanContext& ctx, VkImage image,
                           VkImageLayout oldLayout, VkImageLayout newLayout,
                           uint32_t layerCount, uint32_t mipLevels = 1);

// Copy a tightly-packed buffer (layer 0, layer 1, ...) into an image's layers.
void copyBufferToImage(const VulkanContext& ctx, VkBuffer buffer, VkImage image,
                       uint32_t width, uint32_t height, uint32_t layerCount);

// First of `candidates` that supports `features` with the given tiling.
VkFormat findSupportedFormat(VkPhysicalDevice physical,
                             const std::vector<VkFormat>& candidates,
                             VkImageTiling tiling, VkFormatFeatureFlags features);

// A depth format supported as a depth-stencil attachment.
VkFormat findDepthFormat(VkPhysicalDevice physical);

} // namespace vkutil
} // namespace vg
