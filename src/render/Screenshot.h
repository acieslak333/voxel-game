#pragma once

/**
 * @file Screenshot.h
 * @brief Utility to copy a presented swapchain image to a PNG file.
 *
 * Used by the --screenshot flag for headless verification. Not part of the
 * normal render path; the device must be idle before calling saveImage().
 * @see docs/CODE_INDEX.md
 */

#include <vulkan/vulkan.h>

#include <string>

namespace vg {

class VulkanContext;

namespace screenshot {

/**
 * @brief Copy a swapchain colour image to a PNG file on disk.
 *
 * Transitions the image from PRESENT_SRC_KHR to TRANSFER_SRC_OPTIMAL, copies
 * it into a host-visible buffer, restores the layout, swizzles BGRA to RGBA if
 * needed, and writes the result via stb_image_write.
 * @param ctx     The device context (must have the device idle).
 * @param image   The swapchain image to capture (PRESENT_SRC_KHR layout).
 * @param format  The swapchain image format (used to detect BGRA swizzle).
 * @param extent  Pixel dimensions of the image.
 * @param path    Output file path (PNG).
 * @warning The device must be idle before this call.
 */
// Copy a swapchain colour image (assumed to currently be in PRESENT_SRC layout)
// into host memory and write it out as a PNG. Used purely for verification /
// CI; not part of the normal render path. The device must be idle.
void saveImage(const VulkanContext& ctx, VkImage image, VkFormat format,
               VkExtent2D extent, const std::string& path);

} // namespace screenshot
} // namespace vg
