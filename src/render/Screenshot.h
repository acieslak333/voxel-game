#pragma once

#include <vulkan/vulkan.h>

#include <string>

namespace vg {

class VulkanContext;

namespace screenshot {

// Copy a swapchain colour image (assumed to currently be in PRESENT_SRC layout)
// into host memory and write it out as a PNG. Used purely for verification /
// CI; not part of the normal render path. The device must be idle.
void saveImage(const VulkanContext& ctx, VkImage image, VkFormat format,
               VkExtent2D extent, const std::string& path);

} // namespace screenshot
} // namespace vg
