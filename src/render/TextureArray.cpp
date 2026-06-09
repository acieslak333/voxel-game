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

    // --- Stage and upload to a device-local array image ----------------------
    const VkDeviceSize imageBytes = pixels.size();
    Buffer staging(ctx, imageBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    staging.upload(pixels.data(), imageBytes);

    const VkFormat format = VK_FORMAT_R8G8B8A8_SRGB; // sampled into linear space

    vkutil::createImage(ctx, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                        layerCount, format, VK_IMAGE_TILING_OPTIMAL,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, image_, memory_);

    vkutil::transitionImageLayout(ctx, image_, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layerCount);
    vkutil::copyBufferToImage(ctx, staging.handle(), image_,
                              static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                              layerCount);
    vkutil::transitionImageLayout(ctx, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, layerCount);

    view_ = vkutil::createImageView(ctx.device(), image_, format,
                                    VK_IMAGE_ASPECT_COLOR_BIT,
                                    VK_IMAGE_VIEW_TYPE_2D_ARRAY, layerCount);
    createSampler();
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
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.minLod = 0.0f;
    info.maxLod = 0.0f;

    if (vkCreateSampler(ctx_->device(), &info, nullptr, &sampler_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create texture sampler");
    }
}

} // namespace vg
