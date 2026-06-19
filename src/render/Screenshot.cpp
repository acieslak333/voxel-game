/**
 * @file Screenshot.cpp
 * @brief Implements screenshot::saveImage: GPU readback and PNG write via stb_image_write.
 *
 * Transitions the swapchain image from PRESENT_SRC to TRANSFER_SRC, copies it to a
 * host-visible Buffer, restores the layout, performs optional BGRA->RGBA swizzle,
 * and writes the result as a PNG. stb_image_write is compiled in this translation unit.
 */

#include "render/Screenshot.h"

#include "render/Buffer.h"
#include "render/VulkanContext.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <stb_image_write.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <cstring>
#include <iostream>
#include <vector>

namespace vg::screenshot {

namespace {
// True for the BGRA swapchain formats we might pick, so we know to swap R<->B
// before writing (PNG expects RGBA byte order).
bool isBgra(VkFormat f) {
    return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_B8G8R8A8_UNORM;
}
} // namespace

void saveImage(const VulkanContext& ctx, VkImage image, VkFormat format,
               VkExtent2D extent, const std::string& path) {
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;

    // Host-visible buffer to receive the copied pixels.
    Buffer dst(const_cast<VulkanContext&>(ctx), bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    VkCommandBuffer cmd = ctx.beginSingleTimeCommands();

    // PRESENT_SRC -> TRANSFER_SRC so we can copy out of it.
    VkImageMemoryBarrier toSrc{};
    toSrc.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.oldLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toSrc.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.image               = image;
    toSrc.srcAccessMask       = VK_ACCESS_MEMORY_READ_BIT;
    toSrc.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    toSrc.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent      = {extent.width, extent.height, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.handle(),
                           1, &region);

    // Restore the layout so presentation could continue if needed.
    VkImageMemoryBarrier toPresent = toSrc;
    toPresent.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toPresent.newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toPresent.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toPresent);

    ctx.endSingleTimeCommands(cmd);

    // Read back and (if needed) swizzle BGRA -> RGBA.
    std::vector<unsigned char> pixels(bytes);
    void* mapped = dst.map();
    std::memcpy(pixels.data(), mapped, static_cast<size_t>(bytes));
    dst.unmap();

    if (isBgra(format)) {
        for (size_t i = 0; i < pixels.size(); i += 4) {
            std::swap(pixels[i], pixels[i + 2]);
        }
    }

    if (stbi_write_png(path.c_str(), static_cast<int>(extent.width),
                       static_cast<int>(extent.height), 4, pixels.data(),
                       static_cast<int>(extent.width) * 4)) {
        std::cout << "[screenshot] wrote " << path << '\n';
    } else {
        std::cerr << "[screenshot] failed to write " << path << '\n';
    }
}

} // namespace vg::screenshot
