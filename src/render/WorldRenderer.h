#pragma once

#include "render/Buffer.h"

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace vg {

class VulkanContext;
class Pipeline;
class TextureArray;
class World;

// -----------------------------------------------------------------------------
//  WorldRenderer
// -----------------------------------------------------------------------------
//  Renders a whole World: greedy-meshes every chunk once, keeps a vertex/index
//  buffer per non-empty chunk, and draws them each frame with a per-chunk model
//  matrix (its world-space translation). The pipeline, texture array and
//  per-frame camera UBO/descriptor sets are shared across all chunks.
//
//  TODO(future): rebuild only dirtied chunks, and stream chunks in/out as the
//  player moves rather than meshing the entire (fixed) world up front.
// -----------------------------------------------------------------------------
class WorldRenderer {
public:
    WorldRenderer(VulkanContext& ctx, VkRenderPass renderPass, uint32_t framesInFlight,
                  const World& world, const std::string& shaderDir,
                  const std::string& textureDir);
    ~WorldRenderer();

    WorldRenderer(const WorldRenderer&) = delete;
    WorldRenderer& operator=(const WorldRenderer&) = delete;

    void record(VkCommandBuffer cmd, uint32_t frameIndex, VkExtent2D extent,
                const glm::mat4& view, const glm::mat4& proj);

    [[nodiscard]] std::size_t drawnChunkCount() const { return meshes_.size(); }
    [[nodiscard]] std::size_t triangleCount()   const { return totalTriangles_; }

private:
    struct CameraUBO {
        glm::mat4 view;
        glm::mat4 proj;
    };
    // GPU buffers + placement for one chunk's mesh.
    struct ChunkMesh {
        Buffer    vertexBuffer;
        Buffer    indexBuffer;
        uint32_t  indexCount = 0;
        glm::vec3 worldPos{0.0f};
    };

    void buildMeshes();
    void createUniformBuffers(uint32_t n);
    void createDescriptorSets(uint32_t n);

    VulkanContext& ctx_;
    const World&   world_;

    std::unique_ptr<TextureArray> textures_;
    std::unique_ptr<Pipeline>     pipeline_;

    std::vector<ChunkMesh> meshes_;          // one per non-empty chunk
    std::size_t            totalTriangles_ = 0;

    std::vector<Buffer>          uniformBuffers_; // one per frame in flight
    VkDescriptorPool             descriptorPool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptorSets_;
};

} // namespace vg
