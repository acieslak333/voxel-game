#pragma once

/**
 * @file EntityRenderer.h
 * @brief Declares EntityRenderer, which draws animated box-rig entities into the scene pass.
 *
 * Each frame all entity meshes are baked from their posed skeletons (Armature::bakeMesh)
 * and concatenated into a single per-frame host-visible vertex buffer; one draw call
 * is issued per entity. Shares the block texture array with WorldRenderer and adds its
 * own Blockbench skin atlas binding.
 * @see docs/CODE_INDEX.md
 */

#include "entity/Armature.h"
#include "render/Buffer.h"

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace vg {

class VulkanContext;

/**
 * @brief Renders animated box-rig entities into the shared low-res scene pass.
 *
 * Entities share the scene depth buffer and composite/fog with the terrain.
 * Geometry is re-baked every frame from Armature::bakeMesh() into a per-frame
 * host-visible vertex buffer; one vkCmdDraw is issued per entity. Reuses the
 * block texture array (binding 1) and adds a Blockbench skin atlas (binding 2).
 *
 * @note The vertex buffer is pre-allocated to kMaxVerts and never reallocated,
 *       ensuring an in-flight frame's buffer is never freed mid-use.
 */
class EntityRenderer {
public:
    /// One entity to draw this frame: its baked (model-space) mesh + world placement.
    struct Draw {
        const std::vector<EntityVertex>* mesh;  // not owned; valid for the record call
        glm::mat4                        model; // world transform of the whole rig
        uint32_t                         useSkin = 0; // 0 = block atlas, 1 = skin atlas
    };

    /**
     * @brief Construct the entity rendering pipeline and pre-allocate vertex buffers.
     * @param ctx            Vulkan device context.
     * @param renderPass     The scene render pass entities draw into.
     * @param framesInFlight Number of frames in flight.
     * @param shaderDir      Directory containing entity.vert/frag SPIR-V.
     * @param textureView    Block texture array image view (binding 1).
     * @param textureSampler Sampler for the block texture array.
     * @param skinView       Blockbench skin atlas image view (binding 2).
     * @param skinSampler    Sampler for the skin atlas.
     */
    EntityRenderer(VulkanContext& ctx, VkRenderPass renderPass, uint32_t framesInFlight,
                   const std::string& shaderDir, VkImageView textureView,
                   VkSampler textureSampler, VkImageView skinView, VkSampler skinSampler);
    ~EntityRenderer();

    EntityRenderer(const EntityRenderer&) = delete;
    EntityRenderer& operator=(const EntityRenderer&) = delete;

    /**
     * @brief Record all entity draw commands for this frame.
     *
     * Concatenates every entity's baked mesh into the per-frame vertex buffer and
     * issues one vkCmdDraw per entity. No-op if @p draws is empty.
     *
     * @param cmd             Command buffer (must be inside the scene render pass).
     * @param frameIndex      Current frame-in-flight index.
     * @param extent          Render extent.
     * @param view            Camera view matrix.
     * @param proj            Camera projection matrix.
     * @param sunDirAmbient   Same as WorldRenderer::record — xyz direction, w ambient.
     * @param sunColIntensity Same as WorldRenderer::record — rgb tint, a sky intensity.
     * @param draws           Entities to draw this frame.
     */
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
        glm::vec4 retro; // reserved (was PS1 vertex-jitter grid; effect removed)
    };

    void createPipeline(VkRenderPass renderPass, const std::string& shaderDir);
    void createUniformBuffers(uint32_t n);
    void createDescriptorSets(uint32_t n, VkImageView view, VkSampler sampler,
                              VkImageView skinView, VkSampler skinSampler);
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
