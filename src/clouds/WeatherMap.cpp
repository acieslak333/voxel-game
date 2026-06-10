#include "clouds/WeatherMap.h"

#include "render/Buffer.h"
#include "render/VulkanContext.h"
#include "render/VulkanUtils.h"

#include <cmath>
#include <stdexcept>

namespace vg {

namespace {

float hash2(int x, int y, uint32_t seed) {
    uint32_t h = seed;
    h ^= static_cast<uint32_t>(x) * 0x8da6b343u;
    h ^= static_cast<uint32_t>(y) * 0xd8163841u;
    h = (h ^ (h >> 13)) * 0x9e3779b1u;
    h ^= h >> 16;
    return static_cast<float>(h & 0x00FFFFFFu) / static_cast<float>(0x01000000);
}

// Tiling 2D value noise over a `period` lattice.
float valueNoise(glm::vec2 p, int period, uint32_t seed) {
    auto wrap = [period](int v) { return ((v % period) + period) % period; };
    const glm::vec2 pf = glm::floor(p);
    const glm::vec2 f = p - pf;
    const int x = static_cast<int>(pf.x), y = static_cast<int>(pf.y);
    const glm::vec2 s = f * f * (3.0f - 2.0f * f);
    const float a = hash2(wrap(x), wrap(y), seed);
    const float b = hash2(wrap(x + 1), wrap(y), seed);
    const float c = hash2(wrap(x), wrap(y + 1), seed);
    const float d = hash2(wrap(x + 1), wrap(y + 1), seed);
    return glm::mix(glm::mix(a, b, s.x), glm::mix(c, d, s.x), s.y);
}

} // namespace

WeatherMap::WeatherMap(VulkanContext& ctx, uint32_t seed) : ctx_(ctx) {
    // Two independent low-frequency fields: coverage variation and type
    // variation, both tiling so wind can scroll them forever.
    std::vector<uint8_t> pixels(static_cast<size_t>(kSize) * kSize * 2);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const glm::vec2 p(static_cast<float>(x) / kSize, static_cast<float>(y) / kSize);
            const float cov = valueNoise(p * 4.0f, 4, seed) * 0.65f +
                              valueNoise(p * 8.0f, 8, seed ^ 0x77u) * 0.35f;
            const float typ = valueNoise(p * 3.0f, 3, seed ^ 0xABCDu);
            pixels[(static_cast<size_t>(y) * kSize + x) * 2 + 0] =
                static_cast<uint8_t>(glm::clamp(cov, 0.0f, 1.0f) * 255.0f + 0.5f);
            pixels[(static_cast<size_t>(y) * kSize + x) * 2 + 1] =
                static_cast<uint8_t>(glm::clamp(typ, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }

    // Upload as an R8G8 2D texture (the shader subtracts 0.5 to recover signs).
    Buffer staging(ctx_, pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    staging.upload(pixels.data(), pixels.size());

    vkutil::createImage(ctx_, kSize, kSize, 1, VK_FORMAT_R8G8_UNORM, VK_IMAGE_TILING_OPTIMAL,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, image_, memory_);
    vkutil::transitionImageLayout(ctx_, image_, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1);
    vkutil::copyBufferToImage(ctx_, staging.handle(), image_, kSize, kSize, 1);
    vkutil::transitionImageLayout(ctx_, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1);
    view_ = vkutil::createImageView(ctx_.device(), image_, VK_FORMAT_R8G8_UNORM,
                                    VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D, 1);
}

WeatherMap::~WeatherMap() {
    VkDevice device = ctx_.device();
    if (view_)   vkDestroyImageView(device, view_, nullptr);
    if (image_)  vkDestroyImage(device, image_, nullptr);
    if (memory_) vkFreeMemory(device, memory_, nullptr);
}

} // namespace vg
