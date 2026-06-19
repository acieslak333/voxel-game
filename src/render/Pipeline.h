#pragma once

/**
 * @file Pipeline.h
 * @brief Vulkan graphics pipeline for chunk geometry (opaque and translucent variants).
 *
 * Builds a VkPipeline from compiled SPIR-V shaders against a given render pass.
 * The descriptor set layout declares four bindings: camera UBO (b0), block texture
 * array (b1), per-chunk draw-data SSBO (b2), and the 3D light atlas (b3). A push
 * constant carries the per-draw model matrix plus a vec4 of parameters.
 * Viewport and scissor are dynamic state, so the pipeline survives window resizes.
 * @see docs/CODE_INDEX.md
 */

#include <vulkan/vulkan.h>

#include <string>

namespace vg {

class VulkanContext;

// -----------------------------------------------------------------------------
//  Pipeline
// -----------------------------------------------------------------------------
//  The graphics pipeline used to draw chunk geometry, plus the two layout
//  objects it depends on:
//    * a descriptor set layout (binding 0: camera UBO, binding 1: texture array)
//    * a pipeline layout (that set + a per-draw model-matrix push constant)
//
//  Viewport and scissor are dynamic state, so the pipeline survives window
//  resizes without being rebuilt.
// -----------------------------------------------------------------------------
/**
 * @brief RAII wrapper for a VkPipeline, VkPipelineLayout, and VkDescriptorSetLayout.
 *
 * Construct with `translucent=true` to obtain the water variant: alpha blending
 * enabled, depth writes disabled, back-face culling disabled. Both variants use
 * VK_COMPARE_OP_GREATER (reversed-Z, near=1 far=0).
 */
class Pipeline {
public:
    // Size of the push constant block: a 4x4 model matrix + a vec4 of params
    // (params.x = output alpha for the translucent pass).
    static constexpr uint32_t kPushConstantSize = (16 + 4) * sizeof(float);

    // translucent: build the water variant — alpha blending on, depth WRITE off
    // (depth test stays on), back-face culling off — so liquid surfaces blend
    // over the opaque terrain already drawn behind them and read from both sides.
    Pipeline(VulkanContext& ctx, VkRenderPass renderPass,
             const std::string& vertSpvPath, const std::string& fragSpvPath,
             bool translucent = false);
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    [[nodiscard]] VkPipeline            handle()              const { return pipeline_; }
    [[nodiscard]] VkPipelineLayout      layout()              const { return pipelineLayout_; }
    [[nodiscard]] VkDescriptorSetLayout descriptorSetLayout() const { return descriptorSetLayout_; }

private:
    void createDescriptorSetLayout();
    void createPipeline(VkRenderPass renderPass, const std::string& vertSpv,
                        const std::string& fragSpv);
    VkShaderModule loadShaderModule(const std::string& path) const;

    VulkanContext* ctx_ = nullptr;
    bool           translucent_ = false;

    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout      pipelineLayout_      = VK_NULL_HANDLE;
    VkPipeline            pipeline_            = VK_NULL_HANDLE;
};

} // namespace vg
