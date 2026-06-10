#include "clouds/CloudNoise.h"

#include "render/Buffer.h"
#include "render/VulkanContext.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace vg {

namespace {

// Deterministic integer hash (wrapped to the noise period) -> [0,1).
float hash3(int x, int y, int z, int period, uint32_t seed) {
    auto wrap = [period](int v) { return ((v % period) + period) % period; };
    uint32_t h = seed;
    h ^= static_cast<uint32_t>(wrap(x)) * 0x8da6b343u;
    h ^= static_cast<uint32_t>(wrap(y)) * 0xd8163841u;
    h ^= static_cast<uint32_t>(wrap(z)) * 0xcb1ab31fu;
    h = (h ^ (h >> 13)) * 0x9e3779b1u;
    h ^= h >> 16;
    return static_cast<float>(h & 0x00FFFFFFu) / static_cast<float>(0x01000000);
}

// Gradient for tiling Perlin: 12 edge directions picked by hash.
glm::vec3 gradient(int x, int y, int z, int period, uint32_t seed) {
    static const glm::vec3 g[12] = {
        {1, 1, 0},  {-1, 1, 0},  {1, -1, 0}, {-1, -1, 0}, {1, 0, 1},  {-1, 0, 1},
        {1, 0, -1}, {-1, 0, -1}, {0, 1, 1},  {0, -1, 1},  {0, 1, -1}, {0, -1, -1}};
    const int i = static_cast<int>(hash3(x, y, z, period, seed) * 12.0f) % 12;
    return g[i];
}

float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

// Tiling 3D Perlin noise, period `period` lattice cells over [0, period).
float perlin(glm::vec3 p, int period, uint32_t seed) {
    const glm::vec3 pf = glm::floor(p);
    const glm::vec3 f = p - pf;
    const int xi = static_cast<int>(pf.x), yi = static_cast<int>(pf.y),
              zi = static_cast<int>(pf.z);
    float n[8];
    for (int c = 0; c < 8; ++c) {
        const int dx = c & 1, dy = (c >> 1) & 1, dz = (c >> 2) & 1;
        const glm::vec3 grad = gradient(xi + dx, yi + dy, zi + dz, period, seed);
        n[c] = glm::dot(grad, f - glm::vec3(dx, dy, dz));
    }
    const float u = fade(f.x), v = fade(f.y), w = fade(f.z);
    const float x00 = glm::mix(n[0], n[1], u), x10 = glm::mix(n[2], n[3], u);
    const float x01 = glm::mix(n[4], n[5], u), x11 = glm::mix(n[6], n[7], u);
    const float y0 = glm::mix(x00, x10, v), y1 = glm::mix(x01, x11, v);
    return glm::mix(y0, y1, w); // ~[-0.7, 0.7]
}

// Tiling 3D Worley noise (inverted F1): 1 at feature points, 0 far away.
float worley(glm::vec3 p, int period, uint32_t seed) {
    const glm::vec3 pf = glm::floor(p);
    const int xi = static_cast<int>(pf.x), yi = static_cast<int>(pf.y),
              zi = static_cast<int>(pf.z);
    float best = 1e9f;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const int cx = xi + dx, cy = yi + dy, cz = zi + dz;
                const glm::vec3 feature =
                    glm::vec3(cx, cy, cz) +
                    glm::vec3(hash3(cx, cy, cz, period, seed),
                              hash3(cx, cy, cz, period, seed ^ 0x1234567u),
                              hash3(cx, cy, cz, period, seed ^ 0x89abcdefu));
                best = std::min(best, glm::dot(p - feature, p - feature));
            }
        }
    }
    return std::max(0.0f, 1.0f - std::sqrt(best)); // inverted: blobs at features
}

float remapf(float x, float a, float b, float c, float d) {
    return c + (x - a) * (d - c) / (b - a);
}

constexpr uint32_t kCacheMagic   = 0x434C4431u; // "CLD1"
constexpr uint32_t kCacheVersion = 3u; // bumped: domain-warp + ridged base, 3-oct detail

} // namespace

CloudNoise::CloudNoise(VulkanContext& ctx, const std::string& cacheFile) : ctx_(ctx) {
    if (!loadCache(cacheFile)) {
        std::cout << "[clouds] generating noise textures (cached for next launch)\n";
        generate();
        saveCache(cacheFile);
    }
    upload3D(base_, kBaseSize, baseImage_, baseMem_, baseView_);
    upload3D(detail_, kDetailSize, detailImage_, detailMem_, detailView_);

    VkSamplerCreateInfo s{};
    s.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    s.magFilter    = VK_FILTER_LINEAR;
    s.minFilter    = VK_FILTER_LINEAR;
    s.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    s.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    s.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    s.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (vkCreateSampler(ctx_.device(), &s, nullptr, &sampler_) != VK_SUCCESS) {
        throw std::runtime_error("CloudNoise: sampler creation failed");
    }
}

CloudNoise::~CloudNoise() {
    VkDevice device = ctx_.device();
    if (sampler_)     vkDestroySampler(device, sampler_, nullptr);
    if (baseView_)    vkDestroyImageView(device, baseView_, nullptr);
    if (baseImage_)   vkDestroyImage(device, baseImage_, nullptr);
    if (baseMem_)     vkFreeMemory(device, baseMem_, nullptr);
    if (detailView_)  vkDestroyImageView(device, detailView_, nullptr);
    if (detailImage_) vkDestroyImage(device, detailImage_, nullptr);
    if (detailMem_)   vkFreeMemory(device, detailMem_, nullptr);
}

void CloudNoise::generate() {
    // --- Base: Perlin-Worley --------------------------------------------------
    // Low-frequency Perlin fBm dilated by Worley blobs: connected billowy shapes
    // with cauliflower lobes (the standard "Perlin-Worley" cloud base).
    base_.resize(static_cast<size_t>(kBaseSize) * kBaseSize * kBaseSize);
    const float inv = 1.0f / kBaseSize;
    for (int z = 0; z < kBaseSize; ++z) {
        for (int y = 0; y < kBaseSize; ++y) {
            for (int x = 0; x < kBaseSize; ++x) {
                const glm::vec3 p(x * inv, y * inv, z * inv);
                // Domain warp: offset the sample point by a low-frequency Perlin
                // vector (tiles, period 2) so the masses swirl and shear instead
                // of sitting on an obvious lattice. (issue #10 B: more varied
                // noise summing.)
                const glm::vec3 warp(perlin(p * 2.0f, 2, 0xA17Au),
                                     perlin(p * 2.0f, 2, 0xB29Bu),
                                     perlin(p * 2.0f, 2, 0xC3ACu));
                const glm::vec3 pWarp = p + 0.14f * warp;
                // Perlin fBm, 3 octaves, periods 4/8/16 (all divide the tile), on
                // the warped coordinate.
                float pn = 0.0f, amp = 0.5f;
                for (int o = 0; o < 3; ++o) {
                    const int period = 4 << o;
                    pn += amp * perlin(pWarp * static_cast<float>(period), period, 0xC10Du + o);
                    amp *= 0.5f;
                }
                pn = glm::clamp(pn * 0.85f + 0.5f, 0.0f, 1.0f); // -> [0,1]
                // Ridged octave: 1 - |perlin| sharpens billow edges into
                // filaments, blended in so shapes range from soft blobs to
                // wispy/stringy depending on position.
                const float ridged =
                    glm::clamp(1.0f - std::fabs(perlin(pWarp * 8.0f, 8, 0x71D6u)) * 1.6f, 0.0f, 1.0f);
                pn = glm::clamp(pn * (0.72f + 0.28f * ridged), 0.0f, 1.0f);
                // Worley fBm, 2 octaves, periods 6/12 (warped domain too).
                const float w0 = worley(pWarp * 6.0f, 6, 0x5EEDu);
                const float w1 = worley(pWarp * 12.0f, 12, 0xF00Du);
                const float wn = glm::clamp(w0 * 0.65f + w1 * 0.35f, 0.0f, 1.0f);
                const float pw =
                    glm::clamp(remapf(pn, glm::clamp(wn - 1.0f, -1.0f, 0.0f) * 0.4f + 0.4f,
                                      1.0f, 0.0f, 1.0f),
                               0.0f, 1.0f);
                base_[(static_cast<size_t>(z) * kBaseSize + y) * kBaseSize + x] =
                    static_cast<uint8_t>(pw * 255.0f + 0.5f);
            }
        }
    }

    // --- Detail: Worley fBm -----------------------------------------------------
    detail_.resize(static_cast<size_t>(kDetailSize) * kDetailSize * kDetailSize);
    const float invD = 1.0f / kDetailSize;
    for (int z = 0; z < kDetailSize; ++z) {
        for (int y = 0; y < kDetailSize; ++y) {
            for (int x = 0; x < kDetailSize; ++x) {
                const glm::vec3 p(x * invD, y * invD, z * invD);
                const float w0 = worley(p * 4.0f, 4, 0xBEEFu);
                const float w1 = worley(p * 8.0f, 8, 0xCAFEu);
                const float w2 = worley(p * 16.0f, 16, 0xD00Du);
                float wn = glm::clamp(w0 * 0.55f + w1 * 0.3f + w2 * 0.15f, 0.0f, 1.0f);
                // Ridged Perlin sharpen: pulls the erosion into stringy filaments
                // (wispier cloud edges / cirrus shredding). (issue #10 B.)
                const float ridged =
                    glm::clamp(1.0f - std::fabs(perlin(p * 8.0f, 8, 0x2C7Du)) * 1.7f, 0.0f, 1.0f);
                wn = glm::clamp(glm::mix(wn, wn * (0.4f + 0.6f * ridged), 0.35f), 0.0f, 1.0f);
                detail_[(static_cast<size_t>(z) * kDetailSize + y) * kDetailSize + x] =
                    static_cast<uint8_t>(wn * 255.0f + 0.5f);
            }
        }
    }
}

bool CloudNoise::loadCache(const std::string& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in.is_open()) return false;
    uint32_t magic = 0, version = 0, baseSize = 0, detailSize = 0;
    in.read(reinterpret_cast<char*>(&magic), 4);
    in.read(reinterpret_cast<char*>(&version), 4);
    in.read(reinterpret_cast<char*>(&baseSize), 4);
    in.read(reinterpret_cast<char*>(&detailSize), 4);
    if (!in || magic != kCacheMagic || version != kCacheVersion ||
        baseSize != kBaseSize || detailSize != kDetailSize) {
        return false;
    }
    base_.resize(static_cast<size_t>(kBaseSize) * kBaseSize * kBaseSize);
    detail_.resize(static_cast<size_t>(kDetailSize) * kDetailSize * kDetailSize);
    in.read(reinterpret_cast<char*>(base_.data()), static_cast<std::streamsize>(base_.size()));
    in.read(reinterpret_cast<char*>(detail_.data()),
            static_cast<std::streamsize>(detail_.size()));
    return static_cast<bool>(in);
}

void CloudNoise::saveCache(const std::string& file) const {
    std::ofstream out(file, std::ios::binary);
    if (!out.is_open()) return; // best effort: regenerate next launch
    const uint32_t header[4] = {kCacheMagic, kCacheVersion, kBaseSize, kDetailSize};
    out.write(reinterpret_cast<const char*>(header), sizeof(header));
    out.write(reinterpret_cast<const char*>(base_.data()),
              static_cast<std::streamsize>(base_.size()));
    out.write(reinterpret_cast<const char*>(detail_.data()),
              static_cast<std::streamsize>(detail_.size()));
}

void CloudNoise::upload3D(const std::vector<uint8_t>& voxels, int size, VkImage& image,
                          VkDeviceMemory& memory, VkImageView& view) {
    VkDevice device = ctx_.device();

    // 3D image (vkutil's helpers are 2D/array-specific, so this is local).
    VkImageCreateInfo info{};
    info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType     = VK_IMAGE_TYPE_3D;
    info.extent        = {static_cast<uint32_t>(size), static_cast<uint32_t>(size),
                          static_cast<uint32_t>(size)};
    info.mipLevels     = 1;
    info.arrayLayers   = 1;
    info.format        = VK_FORMAT_R8_UNORM;
    info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.samples       = VK_SAMPLE_COUNT_1_BIT;
    info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(device, &info, nullptr, &image) != VK_SUCCESS) {
        throw std::runtime_error("CloudNoise: 3D image creation failed");
    }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, image, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = req.size;
    alloc.memoryTypeIndex =
        ctx_.findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device, &alloc, nullptr, &memory) != VK_SUCCESS) {
        throw std::runtime_error("CloudNoise: 3D image memory allocation failed");
    }
    vkBindImageMemory(device, image, memory, 0);

    Buffer staging(ctx_, voxels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    staging.upload(voxels.data(), voxels.size());

    VkCommandBuffer cmd = ctx_.beginSingleTimeCommands();
    VkImageMemoryBarrier b{};
    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = image;
    b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask       = 0;
    b.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent      = info.extent;
    vkCmdCopyBufferToImage(cmd, staging.handle(), image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &copy);

    b.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &b);
    ctx_.endSingleTimeCommands(cmd);

    VkImageViewCreateInfo vi{};
    vi.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image            = image;
    vi.viewType         = VK_IMAGE_VIEW_TYPE_3D;
    vi.format           = VK_FORMAT_R8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device, &vi, nullptr, &view) != VK_SUCCESS) {
        throw std::runtime_error("CloudNoise: 3D image view creation failed");
    }
}

} // namespace vg
