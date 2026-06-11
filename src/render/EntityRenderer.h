#pragma once

#include "entity/Armature.h"
#include "render/Buffer.h"

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace vg {

class VulkanContext;

// -----------------------------------------------------------------------------
//  EntityRenderer (ISSUES #13E)
// -----------------------------------------------------------------------------
//  Draws animated box-rig entities (mobs, dropped items later) into the same
//  low-res scene pass as the world, so they share its depth buffer and the
//  composite/fog. Kept entirely separate from WorldRenderer: chunk meshes are
//  static and streamed, whereas an entity is re-baked from its posed skeleton
//  every frame (cheap for the small box counts a mob has) into a per-frame
//  host-visible vertex buffer.
//
//  Geometry comes from entity/Armature.h bakeMesh(); this class only owns the
//  Vulkan pipeline, the camera UBO/descriptor (reusing the block texture array),
//  and the dynamic vertex buffers. One draw call per entity with a per-entity
//  model push constant.
// -----------------------------------------------------------------------------
class EntityRenderer {
public:
    // One entity to draw this frame: its baked (model-space) mesh + world placement.
    struct Draw {
        const std::vector<EntityVertex>* mesh;  // not owned; valid for the record call
        glm::mat4                        model; // world transform of the whole rig
    };

    EntityRenderer(VulkanContext& ctx, VkRenderPass renderPass, uint32_t framesInFlight,
                   const std::string& shaderDir, VkImageView textureView,
                   VkSampler textureSampler);
    ~EntityRenderer();

    EntityRenderer(const EntityRenderer&) = delete;
    EntityRenderer& operator=(const EntityRenderer&) = delete;

    // Record all entity draws for this frame. Same sun/sky inputs as
    // WorldRenderer::record so entities light identically to the terrain.
    void record(VkCommandBuffer cmd, uint32_t frameIndex, VkExtent2D extent,
                const glm::mat4& view, const glm::mat4& proj,
                const glm::vec4& sunDirAmbient, const glm::vec4& sunColIntensity,
                const std::vector<Draw>& draws);

private:
    struct CameraUBO {
        glm::mat4 view;
        glm::mat4 proj;
        glm::vec4 sunDir;
        glm::vec4 sunCol;
    };

    void createPipeline(VkRenderPass renderPass, const std::string& shaderDir);
    void createUniformBuffers(uint32_t n);
    void createDescriptorSets(uint32_t n, VkImageView view, VkSampler sampler);
    VkShaderModule loadShader(const std::string& path) const;

    // Per-frame host-visible vertex buffer, sized to kMaxVerts up front so it is
    // never reallocated (a reallocation could free a buffer an in-flight frame is
    // still reading). Excess geometry beyond the cap is dropped.
    static constexpr uint32_t kMaxVerts = 1u << 16; // 65536 verts/frame

    VulkanContext& ctx_;
    uint32_t       framesInFlight_ = 0;

    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout      pipelineLayout_      = VK_NULL_HANDLE;
    VkPipeline            pipeline_            = VK_NULL_HANDLE;

    std::vector<Buffer>          uniformBuffers_; // one per frame in flight
    std::vector<Buffer>          vertexBuffers_;  // one host-visible buffer per frame
    VkDescriptorPool             descriptorPool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptorSets_;
};

} // namespace vg
