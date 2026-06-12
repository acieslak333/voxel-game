#pragma once

#include "render/Buffer.h"

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <future>
#include <string>
#include <vector>

namespace vg {

class VulkanContext;
class World;

// -----------------------------------------------------------------------------
//  FarTerrainRenderer (ISSUES #3 / #13D — LOD for distant chunks)
// -----------------------------------------------------------------------------
//  A coarse heightmap "shell" drawn beyond the high-detail voxel window so the
//  player sees terrain far past the streamed chunks without paying per-voxel
//  generation / lighting / triangles for it. Geometry is sampled directly from
//  the deterministic TerrainGenerator (columnInfo height + surface block), not
//  from voxel data, so it costs almost nothing and always matches the near
//  terrain at the seam.
//
//  Structure: concentric geo-clipmap rings centred on the player. Ring L uses a
//  cell size of baseStep<<L blocks, so each ring out is half the resolution and
//  covers four times the area. T-junction cracks between rings (and where the
//  shell meets the voxel window) are hidden by vertical skirts dropped along
//  every ring boundary, plus a small downward bias + window underlap so the near
//  chunks always occlude the shell at the seam (no z-fight, no gap).
//
//  Rendering mirrors EntityRenderer: its own pipeline in the scene render pass
//  (shared depth + composite/fog), reusing the block texture array, with a
//  per-frame host-visible vertex buffer re-filled from a CPU mesh that is only
//  rebuilt when the player crosses a cell. One draw call.
// -----------------------------------------------------------------------------
class FarTerrainRenderer {
public:
    struct Config {
        bool  enabled    = true;
        int   baseStep   = 4;    // blocks per cell in the innermost ring (>=1)
        int   ringCells  = 14;   // cells from inner to outer edge, per ring side
        int   ringCount  = 4;    // number of LOD rings (each 2x coarser)
        float skirtDepth = 28.0f;// how far ring-boundary skirts drop (blocks)
        int   underlap   = 24;   // blocks the shell reaches back under the window
        float yBias      = 1.5f; // shell sits this far below the true surface so
                                 // the near chunks always win the seam overlap
    };

    FarTerrainRenderer(VulkanContext& ctx, VkRenderPass renderPass, uint32_t framesInFlight,
                       const std::string& shaderDir, VkImageView textureView,
                       VkSampler textureSampler, const Config& config);
    ~FarTerrainRenderer();

    FarTerrainRenderer(const FarTerrainRenderer&) = delete;
    FarTerrainRenderer& operator=(const FarTerrainRenderer&) = delete;

    [[nodiscard]] bool enabled() const { return config_.enabled; }

    // The shell's outer Chebyshev half-extent in blocks (how far the LOD reaches
    // from the player). The app uses it to push the scene's far clip plane out so
    // the distant rings aren't clipped. 0 when disabled.
    [[nodiscard]] int outerExtentBlocks(const World& world) const;

    // Collect a finished background rebuild and, if the player has crossed a base
    // cell since the last one, kick a new one. The rebuild samples the immutable
    // generator/registry on a worker thread (the ~20-30k columnInfo calls would
    // hitch the frame on the main thread), so update() itself does no heavy work
    // and no GPU work. Call from the gameplay update.
    void update(const World& world, const glm::vec3& camPos);

    // Upload this frame's shell mesh and draw it. Same sun/sky inputs as
    // WorldRenderer::record so the far ground lights identically. Draw BEFORE the
    // near terrain so the chunks occlude the shell at the seam.
    void record(VkCommandBuffer cmd, uint32_t frameIndex, VkExtent2D extent,
                const glm::mat4& view, const glm::mat4& proj,
                const glm::vec4& sunDirAmbient, const glm::vec4& sunColIntensity);

private:
    // Interleaved far-terrain vertex (matches farterrain.vert attributes).
    struct FarVertex {
        glm::vec3 pos;
        glm::vec3 normal;
        glm::vec2 uv;
        uint32_t  layer;
        uint32_t  tint; // packed RGBA8
    };

    struct CameraUBO {
        glm::mat4 view;
        glm::mat4 proj;
        glm::vec4 sunDir;
        glm::vec4 sunCol;
    };

    // What the shell needs to know about one column (cached per built vertex).
    struct Surf {
        float    y;     // world Y of the surface top face (minus yBias)
        uint32_t layer; // surface texture array layer
        uint32_t tint;  // packed RGBA8 biome veg tint (0xFFFFFFFF = none)
    };

    // Build and RETURN the shell mesh centred on `center` (base-snapped world
    // coords). Pure read of the generator/registry — safe to run on a worker.
    [[nodiscard]] std::vector<FarVertex> buildMesh(const World& world, glm::ivec2 center) const;
    [[nodiscard]] Surf sampleSurf(const World& world, int wx, int wz) const;

    void createPipeline(VkRenderPass renderPass, const std::string& shaderDir);
    void createUniformBuffers(uint32_t n);
    void createDescriptorSets(uint32_t n, VkImageView view, VkSampler sampler);
    VkShaderModule loadShader(const std::string& path) const;

    static constexpr uint32_t kMaxVerts = 1u << 18; // 262144 verts/frame cap

    VulkanContext& ctx_;
    Config         config_;
    uint32_t       framesInFlight_ = 0;

    // Resolved once on the first update (needs a registry).
    mutable uint32_t waterLayer_ = 0;
    mutable bool     waterResolved_ = false;

    std::vector<FarVertex> mesh_;            // current CPU shell (last finished build)
    std::future<std::vector<FarVertex>> buildFuture_; // background rebuild in flight
    glm::ivec2             lastCenter_{1 << 30, 1 << 30}; // centre of the launched build
    bool                   built_ = false;

    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout      pipelineLayout_      = VK_NULL_HANDLE;
    VkPipeline            pipeline_            = VK_NULL_HANDLE;

    std::vector<Buffer>          uniformBuffers_;
    std::vector<Buffer>          vertexBuffers_; // one host-visible buffer per frame
    VkDescriptorPool             descriptorPool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptorSets_;
};

} // namespace vg
