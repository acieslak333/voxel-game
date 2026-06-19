/**
 * @file TextureArray.cpp
 * @brief Implements TextureArray: layer loading, staging upload, mip chain generation, sampler.
 *
 * uploadPixels() stages all layers into a device-local 2D_ARRAY image. If the format
 * supports linear blit filtering, generateMipmaps() blit-downsizes each level per layer
 * in a single command buffer (level i-1 -> TRANSFER_SRC, blit, -> SHADER_READ_ONLY).
 * The sampler uses NEAREST mag/min (crisp voxel look) with LINEAR mipmap blending capped
 * at LOD 3 to avoid washing distant terrain to flat average colour.
 * stb_image is compiled in this translation unit.
 */

#include "render/TextureArray.h"

#include "render/Buffer.h"
#include "render/VulkanContext.h"
#include "render/VulkanUtils.h"

// stb_image: single-header PNG/JPG/... loader. Define the implementation here
// (exactly one translation unit must). Silence warnings from the third-party
// header without relaxing them for our own code.
#define STB_IMAGE_IMPLEMENTATION
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include <stb_image.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <cstring>
#include <stdexcept>

namespace vg {

TextureArray::TextureArray(VulkanContext& ctx, const std::vector<std::string>& filenames,
                           const std::string& textureDir)
    : ctx_(&ctx) {
    if (filenames.empty()) {
        throw std::runtime_error("TextureArray: no textures to load");
    }

    const auto layerCount = static_cast<uint32_t>(filenames.size());

    // --- Load every layer into one tightly-packed CPU buffer -----------------
    int width = 0, height = 0;
    std::vector<unsigned char> pixels; // layer 0, layer 1, ... (RGBA8)

    for (uint32_t layer = 0; layer < layerCount; ++layer) {
        const std::string path = textureDir + "/" + filenames[layer];
        int w = 0, h = 0, channels = 0;
        // Force 4 channels (RGBA) regardless of the file's actual channel count.
        stbi_uc* data = stbi_load(path.c_str(), &w, &h, &channels, STBI_rgb_alpha);
        if (!data) {
            throw std::runtime_error("Failed to load texture: " + path);
        }

        if (layer == 0) {
            width = w;
            height = h;
            pixels.resize(static_cast<size_t>(w) * h * 4 * layerCount);
        } else if (w != width || h != height) {
            stbi_image_free(data);
            throw std::runtime_error("Texture array layers must share dimensions: " + path);
        }

        const size_t layerBytes = static_cast<size_t>(width) * height * 4;
        std::memcpy(pixels.data() + layer * layerBytes, data, layerBytes);
        stbi_image_free(data);
    }

    uploadPixels(pixels, width, height, layerCount);
}

// In-memory constructor: each `layers[i]` is a tightly-packed width*height RGBA8
// image (all the same size). Lets callers build an array from resized/generated
// pixels (e.g. the model skin atlas) without going through files of equal size.
TextureArray::TextureArray(VulkanContext& ctx,
                           const std::vector<std::vector<unsigned char>>& layers,
                           int width, int height)
    : ctx_(&ctx) {
    if (layers.empty() || width <= 0 || height <= 0) {
        throw std::runtime_error("TextureArray: empty in-memory layers");
    }
    const auto layerCount = static_cast<uint32_t>(layers.size());
    const size_t layerBytes = static_cast<size_t>(width) * height * 4;
    std::vector<unsigned char> pixels(layerBytes * layerCount);
    for (uint32_t i = 0; i < layerCount; ++i) {
        if (layers[i].size() != layerBytes) {
            throw std::runtime_error("TextureArray: in-memory layer has wrong size");
        }
        std::memcpy(pixels.data() + i * layerBytes, layers[i].data(), layerBytes);
    }
    uploadPixels(pixels, width, height, layerCount);
}

void TextureArray::uploadPixels(const std::vector<unsigned char>& pixels, int width,
                                int height, uint32_t layerCount) {
    VulkanContext& ctx = *ctx_;
    // --- Stage and upload to a device-local array image ----------------------
    const VkDeviceSize imageBytes = pixels.size();
    Buffer staging(ctx, imageBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    staging.upload(pixels.data(), imageBytes);

    const VkFormat format = VK_FORMAT_R8G8B8A8_SRGB; // sampled into linear space

    // Mip chain: minifying a 16px tile via REPEAT shimmers/moirés at distance and
    // at grazing angles. We build a full chain by linear-blitting each level down
    // from the previous, then sample NEAREST_MIPMAP_LINEAR (crisp texels, smooth
    // level blend). The sampler caps maxLod (see createSampler) so the 1-2px mips
    // — which are just the tile's average colour — don't turn the world to mush.
    // Blit-down needs the format to support a LINEAR filter as a blit source.
    VkFormatProperties fmtProps{};
    vkGetPhysicalDeviceFormatProperties(ctx.physicalDevice(), format, &fmtProps);
    const bool canMip =
        (fmtProps.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    mipLevels_ = canMip ? mipLevelsFor(width, height) : 1;

    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (mipLevels_ > 1) {
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT; // each level is blitted from the one above
    }
    vkutil::createImage(ctx, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                        layerCount, format, VK_IMAGE_TILING_OPTIMAL, usage,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, image_, memory_, mipLevels_);

    // Move every mip level to TRANSFER_DST, fill level 0 from the staging buffer.
    vkutil::transitionImageLayout(ctx, image_, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layerCount, mipLevels_);
    vkutil::copyBufferToImage(ctx, staging.handle(), image_,
                              static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                              layerCount);

    if (mipLevels_ > 1) {
        generateMipmaps(width, height, layerCount); // blits + leaves all levels SHADER_READ
    } else {
        vkutil::transitionImageLayout(ctx, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, layerCount);
    }

    view_ = vkutil::createImageView(ctx.device(), image_, format,
                                    VK_IMAGE_ASPECT_COLOR_BIT,
                                    VK_IMAGE_VIEW_TYPE_2D_ARRAY, layerCount, mipLevels_);
    createSampler();
}

uint32_t TextureArray::mipLevelsFor(int width, int height) {
    uint32_t levels = 1;
    int dim = width > height ? width : height;
    while (dim > 1) {
        dim >>= 1;
        ++levels;
    }
    return levels; // 16px tile -> 5 levels (16,8,4,2,1)
}

// Generate the mip chain in place: blit each level down from the previous with a
// LINEAR filter, transitioning levels through TRANSFER_SRC and ending the whole
// image (every level) in SHADER_READ_ONLY. All array layers are blitted together.
void TextureArray::generateMipmaps(int width, int height, uint32_t layerCount) {
    VkCommandBuffer cmd = ctx_->beginSingleTimeCommands();

    VkImageMemoryBarrier barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = image_;
    barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount     = layerCount;
    barrier.subresourceRange.levelCount     = 1;

    int mipW = width, mipH = height;
    for (uint32_t i = 1; i < mipLevels_; ++i) {
        // Level i-1: TRANSFER_DST -> TRANSFER_SRC so we can read it as the blit source.
        barrier.subresourceRange.baseMipLevel = i - 1;
        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);

        VkImageBlit blit{};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {mipW, mipH, 1};
        blit.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel       = i - 1;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount     = layerCount;
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {mipW > 1 ? mipW / 2 : 1, mipH > 1 ? mipH / 2 : 1, 1};
        blit.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel       = i;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount     = layerCount;
        vkCmdBlitImage(cmd, image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

        // Level i-1 is done being read: TRANSFER_SRC -> SHADER_READ_ONLY.
        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);

        if (mipW > 1) mipW /= 2;
        if (mipH > 1) mipH /= 2;
    }

    // The last level was never read, so it's still TRANSFER_DST -> SHADER_READ_ONLY.
    barrier.subresourceRange.baseMipLevel = mipLevels_ - 1;
    barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    ctx_->endSingleTimeCommands(cmd);
}

TextureArray::~TextureArray() {
    if (!ctx_) {
        return;
    }
    VkDevice device = ctx_->device();
    if (sampler_) vkDestroySampler(device, sampler_, nullptr);
    if (view_)    vkDestroyImageView(device, view_, nullptr);
    if (image_)   vkDestroyImage(device, image_, nullptr);
    if (memory_)  vkFreeMemory(device, memory_, nullptr);
}

void TextureArray::createSampler() {
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    // NEAREST gives the crisp, pixelated look typical of voxel games.
    info.magFilter = VK_FILTER_NEAREST;
    info.minFilter = VK_FILTER_NEAREST;
    // REPEAT is what makes per-block UVs (0..N) tile the texture across a quad.
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.anisotropyEnable = VK_FALSE; // device feature not enabled; keep it simple
    info.maxAnisotropy    = 1.0f;
    info.borderColor   = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    info.unnormalizedCoordinates = VK_FALSE;
    info.compareEnable = VK_FALSE;
    // NEAREST in-plane (crisp texels) but LINEAR across mip levels (smooth fade
    // between levels, no popping). maxLod is capped well below the full chain: a
    // 16px tile's 1-2px mips are just its average colour, and LOD is computed at
    // the coarse offscreen resolution so mips already bite early — letting it run
    // to the tiniest level would wash distant terrain to flat colour. Cap at 3
    // (down to the 2px level) for a little anti-shimmer without the mush.
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.minLod = 0.0f;
    info.maxLod = mipLevels_ > 1 ? 3.0f : 0.0f;

    if (vkCreateSampler(ctx_->device(), &info, nullptr, &sampler_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create texture sampler");
    }
}

} // namespace vg
