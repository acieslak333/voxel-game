#pragma once

/**
 * @file SkyRenderer.h
 * @brief Declares SkyRenderer, the procedural sky and volumetric cloud renderer.
 *
 * Draws a fullscreen triangle at the start of the scene pass with depth test and
 * write disabled, so world geometry simply renders over it. Per-frame state (view
 * ray reconstruction, DayNight colours, and the CloudSystem parameter block) is
 * packed into one UBO per frame in flight. Cloud noise and the weather texture are
 * bound once at construction and remain static for the session.
 * @see docs/CODE_INDEX.md
 */

#include "clouds/CloudSystem.h"
#include "core/DayNight.h"
#include "render/Buffer.h"

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include <vector>

namespace vg {

class VulkanContext;

/**
 * @brief Procedural sky renderer: analytic atmosphere, sun/moon discs, stars, and volumetric clouds.
 *
 * Renders a fullscreen triangle (sky.vert / sky.frag) at the start of the scene pass.
 * Depth test and write are both disabled; the world draws over it afterwards. The
 * SkyUBO packs the full DayNight::SkyState plus 13 vec4s of CloudSystem::GpuParams.
 */
class SkyRenderer {
public:
    /**
     * @brief Construct the sky pipeline and bind the cloud noise textures.
     * @param ctx               Vulkan device context.
     * @param sceneRenderPass   The scene render pass the sky draws into.
     * @param framesInFlight    Number of frames in flight.
     * @param shaderDir         Directory containing sky.vert/frag SPIR-V.
     * @param cloudBaseNoise    3D Perlin-Worley base noise image view (64^3).
     * @param cloudDetailNoise  3D Worley-fBm detail noise image view (32^3).
     * @param weatherMap        2D R8G8 weather coverage/type image view (64^2).
     * @param cloudSampler      Repeating trilinear sampler shared by all three cloud textures.
     */
    SkyRenderer(VulkanContext& ctx, VkRenderPass sceneRenderPass, uint32_t framesInFlight,
                const std::string& shaderDir, VkImageView cloudBaseNoise,
                VkImageView cloudDetailNoise, VkImageView weatherMap,
                VkSampler cloudSampler);
    ~SkyRenderer();

    SkyRenderer(const SkyRenderer&) = delete;
    SkyRenderer& operator=(const SkyRenderer&) = delete;

    /**
     * @brief Record the sky draw. Must be called first inside the scene render pass.
     *
     * Builds and uploads the SkyUBO, then issues vkCmdDraw for the fullscreen triangle.
     * Translation is stripped from @p view for the ray reconstruction; the cloud raymarch
     * uses @p camPos directly.
     *
     * @param cmd       Command buffer (inside the scene render pass).
     * @param frameIndex Current frame-in-flight index.
     * @param extent    Render extent.
     * @param view      Camera view matrix (translation stripped internally).
     * @param proj      Camera projection matrix.
     * @param camPos    World-space camera position for the cloud raymarch.
     * @param state     DayNight sky state (sun/moon directions, colours, etc.).
     * @param clouds    CloudSystem GPU parameter block (13 vec4s).
     */
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
