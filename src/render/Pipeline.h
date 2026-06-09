#pragma once

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
class Pipeline {
public:
    // Size of the push constant block: a single 4x4 model matrix.
    static constexpr uint32_t kPushConstantSize = 16 * sizeof(float);

    Pipeline(VulkanContext& ctx, VkRenderPass renderPass,
             const std::string& vertSpvPath, const std::string& fragSpvPath);
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

    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout      pipelineLayout_      = VK_NULL_HANDLE;
    VkPipeline            pipeline_            = VK_NULL_HANDLE;
};

} // namespace vg
