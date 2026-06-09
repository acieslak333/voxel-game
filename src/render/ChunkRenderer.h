#pragma once

#include "render/Buffer.h"
#include "world/BlockRegistry.h"
#include "world/Chunk.h"

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <vector>

namespace vg {

class VulkanContext;
class Pipeline;
class TextureArray;

// -----------------------------------------------------------------------------
//  ChunkRenderer
// -----------------------------------------------------------------------------
//  Milestone 1 "scene": owns a single hardcoded chunk, its greedy-meshed GPU
//  buffers, the block texture array, the graphics pipeline, and the per-frame
//  uniform buffers / descriptor sets. record() draws it.
//
//  The camera here is a fixed 3/4 view of the chunk — interactive camera control
//  arrives in Milestone 2, and procedural multi-chunk worlds in Milestone 3
//  (at which point the single Chunk/buffers here become a collection).
// -----------------------------------------------------------------------------
class ChunkRenderer {
public:
    ChunkRenderer(VulkanContext& ctx, VkRenderPass renderPass, uint32_t framesInFlight,
                  const std::string& shaderDir, const std::string& textureDir);
    ~ChunkRenderer();

    ChunkRenderer(const ChunkRenderer&) = delete;
    ChunkRenderer& operator=(const ChunkRenderer&) = delete;

    // Record draw commands inside the active render pass for the given frame.
    void record(VkCommandBuffer cmd, uint32_t frameIndex, VkExtent2D extent);

private:
    // Matches the std140 layout of CameraUBO in the vertex shader.
    struct CameraUBO {
        glm::mat4 view;
        glm::mat4 proj;
    };

    void buildWorld();                       // hardcode the chunk + upload its mesh
    void createUniformBuffers(uint32_t n);
    void createDescriptorSets(uint32_t n);

    VulkanContext& ctx_;

    BlockRegistry registry_;
    Chunk         chunk_;

    std::unique_ptr<TextureArray> textures_;
    std::unique_ptr<Pipeline>     pipeline_;

    Buffer   vertexBuffer_;
    Buffer   indexBuffer_;
    uint32_t indexCount_ = 0;

    std::vector<Buffer>          uniformBuffers_; // one per frame in flight
    VkDescriptorPool             descriptorPool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptorSets_; // one per frame in flight
};

} // namespace vg
