/**
 * @file LightAtlas.cpp
 * @brief Implements LightAtlas: 3D image creation, slot allocation, deferred recycling, writes.
 *
 * The constructor lays out slots as a 2D grid (~sqrt x sqrt) in X/Y, one slot deep in Z,
 * then transitions the image to VK_IMAGE_LAYOUT_GENERAL for its lifetime.
 * recordWrite() copies a PAD^3 RGBA8 block from a staging Buffer into the slot's sub-region
 * and inserts an ordering barrier (TRANSFER_WRITE -> SHADER_READ) so the draw pass can
 * sample the updated slot later in the same submission. The staging Buffer is retired
 * alongside the old slot (freeDeferred / retiredStaging_) and freed after framesInFlight+1
 * tick() calls.
 */

#include "render/LightAtlas.h"

#include "render/VulkanContext.h"

#include <cmath>
#include <stdexcept>

namespace vg {

LightAtlas::LightAtlas(VulkanContext& ctx, uint32_t slotCapacity, uint32_t framesInFlight)
    : ctx_(&ctx), framesInFlight_(framesInFlight) {
    if (slotCapacity == 0) slotCapacity = 1;

    // Lay the PAD³ slots out as a 2D grid in X (cols) and Y (rows); every slot is
    // one PAD deep in Z, so the image is PAD voxels deep. cols ~ sqrt keeps the
    // image roughly square and each side well under maxImageDimension3D.
    cols_ = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(slotCapacity))));
    if (cols_ < 1) cols_ = 1;
    rows_ = static_cast<int>((slotCapacity + cols_ - 1) / static_cast<uint32_t>(cols_));
    dimX_ = cols_ * kPad;
    dimY_ = rows_ * kPad;
    dimZ_ = kPad;

    const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;

    VkImageCreateInfo ii{};
    ii.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType     = VK_IMAGE_TYPE_3D;
    ii.extent        = {static_cast<uint32_t>(dimX_), static_cast<uint32_t>(dimY_),
                        static_cast<uint32_t>(dimZ_)};
    ii.mipLevels     = 1;
    ii.arrayLayers   = 1;
    ii.format        = format;
    ii.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ii.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ii.samples       = VK_SAMPLE_COUNT_1_BIT;
    if (vkCreateImage(ctx.device(), &ii, nullptr, &image_) != VK_SUCCESS) {
        throw std::runtime_error("LightAtlas: failed to create 3D image");
    }

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(ctx.device(), image_, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = ctx.findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(ctx.device(), &ai, nullptr, &memory_) != VK_SUCCESS) {
        throw std::runtime_error("LightAtlas: failed to allocate image memory");
    }
    vkBindImageMemory(ctx.device(), image_, memory_, 0);

    // One-time UNDEFINED -> GENERAL. The image then stays GENERAL for life: GENERAL
    // permits both sampling and transfer writes, so per-slot updates need only an
    // ordering barrier (in recordWrite), never a layout transition.
    {
        VkCommandBuffer cmd = ctx.beginSingleTimeCommands();
        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = image_;
        b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask       = 0;
        b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
        ctx.endSingleTimeCommands(cmd);
    }

    VkImageViewCreateInfo vi{};
    vi.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image                       = image_;
    vi.viewType                    = VK_IMAGE_VIEW_TYPE_3D;
    vi.format                      = format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(ctx.device(), &vi, nullptr, &view_) != VK_SUCCESS) {
        throw std::runtime_error("LightAtlas: failed to create image view");
    }

    VkSamplerCreateInfo si{};
    si.sType         = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter     = VK_FILTER_LINEAR; // trilinear interpolation = smooth lighting
    si.minFilter     = VK_FILTER_LINEAR;
    si.mipmapMode    = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.unnormalizedCoordinates = VK_FALSE;
    si.maxLod        = 0.0f;
    if (vkCreateSampler(ctx.device(), &si, nullptr, &sampler_) != VK_SUCCESS) {
        throw std::runtime_error("LightAtlas: failed to create sampler");
    }

    // Whole grid free initially (cols*rows may exceed slotCapacity — the extra
    // cells are usable spares).
    const int total = cols_ * rows_;
    freeSlots_.reserve(static_cast<size_t>(total));
    for (int s = total - 1; s >= 0; --s) freeSlots_.push_back(s);
}

LightAtlas::~LightAtlas() {
    if (!ctx_) return;
    VkDevice device = ctx_->device();
    if (sampler_) vkDestroySampler(device, sampler_, nullptr);
    if (view_)    vkDestroyImageView(device, view_, nullptr);
    if (image_)   vkDestroyImage(device, image_, nullptr);
    if (memory_)  vkFreeMemory(device, memory_, nullptr);
}

int LightAtlas::alloc() {
    if (freeSlots_.empty()) return -1;
    const int s = freeSlots_.back();
    freeSlots_.pop_back();
    return s;
}

void LightAtlas::freeDeferred(int slot) {
    if (slot < 0) return;
    retiredSlots_.push_back({static_cast<int>(framesInFlight_) + 1, slot});
}

void LightAtlas::recordWrite(VkCommandBuffer cmd, int slot, const uint8_t* rgba) {
    if (slot < 0) return;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(kPad) * kPad * kPad * 4;
    Buffer staging(*ctx_, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    staging.upload(rgba, bytes);

    int ox, oy, oz;
    slotOrigin(slot, ox, oy, oz);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageOffset      = {ox, oy, oz};
    c.imageExtent      = {static_cast<uint32_t>(kPad), static_cast<uint32_t>(kPad),
                          static_cast<uint32_t>(kPad)};
    vkCmdCopyBufferToImage(cmd, staging.handle(), image_, VK_IMAGE_LAYOUT_GENERAL, 1, &c);

    // Order this write before any fragment-shader sample of the atlas later in the
    // same submission (the render pass). Cross-frame safety comes from rotating
    // slots: a slot the GPU may still sample this frame is never the one written.
    VkImageMemoryBarrier b{};
    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = image_;
    b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

    retiredStaging_.push_back({static_cast<int>(framesInFlight_) + 1, std::move(staging)});
}

void LightAtlas::tick() {
    for (size_t i = 0; i < retiredSlots_.size();) {
        if (--retiredSlots_[i].framesLeft <= 0) {
            freeSlots_.push_back(retiredSlots_[i].slot);
            retiredSlots_[i] = retiredSlots_.back();
            retiredSlots_.pop_back();
        } else {
            ++i;
        }
    }
    for (size_t i = 0; i < retiredStaging_.size();) {
        if (--retiredStaging_[i].framesLeft <= 0) {
            retiredStaging_[i] = std::move(retiredStaging_.back());
            retiredStaging_.pop_back();
        } else {
            ++i;
        }
    }
}

} // namespace vg
