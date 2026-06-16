#pragma once

#include "render/Buffer.h"

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace vg {

class VulkanContext;

// -----------------------------------------------------------------------------
//  LightAtlas
// -----------------------------------------------------------------------------
//  A single VK_IMAGE_TYPE_3D texture tiled into fixed PAD³ per-chunk light slots,
//  sampled per-pixel by chunk.frag so lighting is DECOUPLED from mesh geometry
//  (S7): the mesher no longer bakes per-corner sky/block/hue into vertices, so
//  greedy quads merge across light gradients (far fewer triangles, no relight
//  remesh of light). Each voxel stores  RGBA8 = [sky<<4 | block, hueR, hueG, hueB].
//
//  Each slot is PAD = kCellsPerChunk + 2 wide on every axis: the chunk's 16 cells
//  plus a one-voxel border of the neighbours' light, so hardware trilinear
//  filtering across the chunk's interior never samples stale/zero edge data.
//
//  Slots ROTATE on update. A chunk that relights is written to a freshly allocated
//  slot; its previous slot is retired for framesInFlight frames before being
//  recycled — so a slot the GPU may still be sampling in an in-flight frame is
//  never overwritten in place. This mirrors the mesh arena's deferred-free spans
//  (WorldRenderer::retiredAllocs_), giving sync-safety without per-update image
//  barriers. The live light-slot for each chunk is published to the shader via the
//  per-chunk draw-data SSBO (ChunkDraw.posPad.w).
//
//  The image stays in VK_IMAGE_LAYOUT_GENERAL for its whole life (both sampled and
//  transfer-written), so slot writes need only an ordering barrier, never a layout
//  transition. Staging buffers for each write are owned here and retired the same
//  deferred way, so callers just hand over the PAD³ pixel block.
// -----------------------------------------------------------------------------
class LightAtlas {
public:
    static constexpr int kPad = 18; // 16 chunk cells + 1 border voxel each side

    // `slotCapacity` is the maximum number of LIVE + retired light slots; size it
    // at the renderer's chunk-slot count plus headroom for in-flight rotations.
    LightAtlas(VulkanContext& ctx, uint32_t slotCapacity, uint32_t framesInFlight);
    ~LightAtlas();

    LightAtlas(const LightAtlas&)            = delete;
    LightAtlas& operator=(const LightAtlas&) = delete;

    // Reserve a free slot (its texel origin is slotOrigin(slot)). Returns -1 if the
    // pool is exhausted (caller should fall back to leaving the chunk's old slot).
    [[nodiscard]] int alloc();
    // Retire `slot` for `framesInFlight` frames, then recycle it. Pass the slot a
    // chunk is leaving behind after its new slot is written.
    void freeDeferred(int slot);

    // Record a copy of one PAD³ RGBA8 light block (x fastest, then y, then z) into
    // `slot`'s sub-region of the image, into `cmd`. A host-visible staging buffer is
    // allocated internally and retained until the write's frames elapse.
    void recordWrite(VkCommandBuffer cmd, int slot, const uint8_t* rgba);

    // Advance one frame: recycle retired slots and free staging buffers whose
    // in-flight frames have completed. Call once per recorded frame.
    void tick();

    [[nodiscard]] VkImageView view()    const { return view_; }
    [[nodiscard]] VkSampler   sampler() const { return sampler_; }
    // Shader needs these to map (lightSlot, chunk-local pos) -> normalised texcoord:
    // x = slots per row (cols), yzw = atlas texel dimensions.
    [[nodiscard]] glm::vec4 shaderParams() const {
        return glm::vec4(static_cast<float>(cols_), static_cast<float>(dimX_),
                         static_cast<float>(dimY_), static_cast<float>(dimZ_));
    }

private:
    void slotOrigin(int slot, int& ox, int& oy, int& oz) const {
        ox = (slot % cols_) * kPad;
        oy = (slot / cols_) * kPad;
        oz = 0;
    }

    VulkanContext* ctx_ = nullptr;
    uint32_t framesInFlight_ = 2;

    VkImage        image_   = VK_NULL_HANDLE;
    VkDeviceMemory memory_  = VK_NULL_HANDLE;
    VkImageView    view_    = VK_NULL_HANDLE;
    VkSampler      sampler_ = VK_NULL_HANDLE;

    int cols_ = 1, rows_ = 1;      // slot grid in X (cols) and Y (rows); Z = 1 slot deep
    int dimX_ = 0, dimY_ = 0, dimZ_ = 0; // image texel dimensions

    std::vector<int> freeSlots_;   // recycle pool
    struct Retired { int framesLeft; int slot; };
    std::vector<Retired> retiredSlots_;
    struct StagingHold { int framesLeft; Buffer buf; };
    std::vector<StagingHold> retiredStaging_;
};

} // namespace vg
