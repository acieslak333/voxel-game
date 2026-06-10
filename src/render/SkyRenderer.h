#pragma once

#include "clouds/CloudSystem.h"
#include "core/DayNight.h"
#include "render/Buffer.h"

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include <vector>

namespace vg {

class VulkanContext;

// -----------------------------------------------------------------------------
//  SkyRenderer
// -----------------------------------------------------------------------------
//  Draws the procedural sky (atmosphere gradient + sun + moon + the volumetric
//  cloud layer) as a fullscreen triangle at the start of the low-res scene
//  pass, with depth test/write disabled so the world geometry simply draws over
//  it. Per-frame state (view ray reconstruction matrix, the DayNight colours,
//  and the cloud system's parameter block) goes in one UBO per frame in flight;
//  the cloud noise / weather textures are bound once at construction.
// -----------------------------------------------------------------------------
class SkyRenderer {
public:
    SkyRenderer(VulkanContext& ctx, VkRenderPass sceneRenderPass, uint32_t framesInFlight,
                const std::string& shaderDir, VkImageView cloudBaseNoise,
                VkImageView cloudDetailNoise, VkImageView weatherMap,
                VkSampler cloudSampler);
    ~SkyRenderer();

    SkyRenderer(const SkyRenderer&) = delete;
    SkyRenderer& operator=(const SkyRenderer&) = delete;

    // Record the sky draw. Call first inside the scene render pass. `view`/`proj`
    // are the camera matrices used for the world (translation is stripped for the
    // ray reconstruction; `camPos` seeds the cloud raymarch).
    void record(VkCommandBuffer cmd, uint32_t frameIndex, VkExtent2D extent,
                const glm::mat4& view, const glm::mat4& proj, const glm::vec3& camPos,
                const DayNight::SkyState& state, const CloudSystem::GpuParams& clouds);

private:
    struct SkyUBO {
        glm::mat4 invViewProj;
        glm::vec4 sunDir;   // xyz + w = skyBlend (0 night .. 1 day)
        glm::vec4 moonDir;  // xyz + w = exposure
        glm::vec4 zenith;   // rgb night/legacy colour + w = analytic flag (1/0)
        glm::vec4 horizon;  // rgb night/legacy colour + w = sun intensity
        glm::vec4 sunDisc;  // rgb (above-atmosphere) + cosOuter
        glm::vec4 moonDisc; // rgb + cosOuter
        glm::vec4 params;   // glow, cosSunInner, cosMoonInner, mieG
        glm::vec4 betaR;    // rgb Rayleigh coefficients + w = betaM
        glm::vec4 tint;     // rgb daytime zenith tint (Sky option) + w = sunset strength
        glm::vec4 sunset;   // rgb horizon sunset band (gold) + w = band amount
        glm::vec4 sunsetMid;  // rgb mid sunset band (orange) + w spare
        glm::vec4 sunsetHigh; // rgb high-sky afterglow (pink/violet) + w spare
        glm::vec4 ozone;      // rgb Chappuis absorption coeff + w = strength
        glm::vec4 cloudDusk;  // rgb warm cloud-underside dusk tint + w = strength
        CloudSystem::GpuParams clouds; // 13 vec4s, mirrored in sky.frag
        glm::vec4 camPos;   // xyz camera world position
        glm::vec4 star;     // x siderealAngle, y latitude, z brightness, w milkyWay
        glm::vec4 star2;    // x twinkleSpeed, y starExtinction, z planets, w shootingStars
    };

    void createDescriptors(uint32_t framesInFlight, VkImageView base, VkImageView detail,
                           VkImageView weather, VkSampler sampler);
    void createPipeline(VkRenderPass renderPass, const std::string& shaderDir);

    VulkanContext& ctx_;

    std::vector<Buffer>          uniformBuffers_; // one per frame in flight
    VkDescriptorSetLayout        setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool             pool_      = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> sets_;
    VkPipelineLayout             pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline                   pipeline_       = VK_NULL_HANDLE;
};

} // namespace vg
