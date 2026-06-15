#pragma once

#include "render/Buffer.h"

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <future>
#include <string>
#include <utility>
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
        int   underlap   = 32;   // blocks the shell reaches in under the window edge
                                 // (covers chunks still meshing at the leading edge)
        float yBias      = 1.5f; // shell sits this far below the true surface so
                                 // the near chunks always win the seam overlap
        bool  trees      = true; // scatter low-poly tree impostors (cones/blobs)
        int   treeDist   = 240;  // blocks past the window edge that impostors fill
        float forestTint = 0.72f;// ground darkens toward this where biomes are forested
    };

    FarTerrainRenderer(VulkanContext& ctx, VkRenderPass renderPass, uint32_t framesInFlight,
                       const std::string& shaderDir, VkImageView textureView,
                       VkSampler textureSampler, const Config& config);
    ~FarTerrainRenderer();

    FarTerrainRenderer(const FarTerrainRenderer&) = delete;
    FarTerrainRenderer& operator=(const FarTerrainRenderer&) = delete;

    [[nodiscard]] bool enabled() const { return config_.enabled; }
    // Runtime on/off (the LOD toggle in the Esc menu). When turned back on, the
    // next update() rebuilds the shell mesh, so it reappears within a few frames.
    void setEnabled(bool e) { config_.enabled = e; }

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

    // Upload the shell mesh (only when it changed) and draw it. Same sun/sky
    // inputs as WorldRenderer::record so the far ground lights identically. Draw
    // BEFORE the near terrain so the chunks occlude the shell at the seam. The
    // shell dissolves into `hazeColor` between fadeStart..fadeEnd world distance
    // so its outer edge never reads as a hard cutoff.
    void record(VkCommandBuffer cmd, uint32_t frameIndex, VkExtent2D extent,
                const glm::mat4& view, const glm::mat4& proj,
                const glm::vec4& sunDirAmbient, const glm::vec4& sunColIntensity,
                const glm::vec3& camPos, const glm::vec3& hazeColor,
                float fadeStart, float fadeEnd);

    // PS1 vertex-jitter grid resolution for the far shell (0 = off).
    void setRetro(float jitter) { retroJitter_ = jitter; }

private:
    float retroJitter_ = 0.0f; // PS1 vertex-jitter grid resolution (0 = off)

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
        glm::vec4 camPos;  // xyz cam, w = haze fade start
        glm::vec4 haze;    // rgb haze colour, w = haze fade end
        glm::vec4 lodFade; // x = impostor dissolve-near dist, y = dissolve band
    };

    // What the shell needs to know about one column (cached per built vertex).
    struct Surf {
        float    y;     // world Y of the surface top face (minus yBias)
        uint32_t layer; // surface texture array layer
        uint32_t tint;  // packed RGBA8 biome veg tint (0xFFFFFFFF = none)
    };

    // Build and RETURN the shell mesh centred on `center` (base-snapped world
    // coords). `winBox` is the loaded voxel window as world block bounds
    // (x0,z0,x1,z1), snapshotted on the main thread so the worker doesn't race
    // recenter() — tree impostors are skipped inside it (real voxel trees there).
    // Pure read of the generator/registry — safe to run on a worker.
    [[nodiscard]] std::vector<FarVertex> buildMesh(const World& world, glm::ivec2 center,
                                                   glm::ivec4 winBox) const;
    [[nodiscard]] Surf sampleSurf(const World& world, int wx, int wz) const;

    void uploadMesh();   // (re)build the device-local vertex buffer from mesh_
    void tickRetired();  // age + free deferred old vertex buffers (once per frame)
    void createPipeline(VkRenderPass renderPass, const std::string& shaderDir);
    void createUniformBuffers(uint32_t n);
    void createDescriptorSets(uint32_t n, VkImageView view, VkSampler sampler);
    VkShaderModule loadShader(const std::string& path) const;

    static constexpr uint32_t kMaxVerts = 1u << 20; // 1048576 verts cap: headroom so a
                                                    // dense-forest shell + impostors never
                                                    // hit the cap and silently drop geometry

    VulkanContext& ctx_;
    Config         config_;
    uint32_t       framesInFlight_ = 0;

    // Resolved once on the first update (needs a registry). leafLayer_ is indexed
    // by TreeKind (oak/birch/pine), trunkLayer_ shared.
    mutable uint32_t waterLayer_ = 0;
    mutable uint32_t leafLayer_[3] = {0, 0, 0};
    mutable uint32_t trunkLayer_ = 0;
    mutable bool     layersResolved_ = false;

    std::vector<FarVertex> mesh_;            // current CPU shell (last finished build)
    std::future<std::vector<FarVertex>> buildFuture_; // background rebuild in flight
    glm::ivec2             lastCenter_{1 << 30, 1 << 30}; // centre of the launched build
    bool                   built_ = false;
    float                  fadeNear_ = 0.0f;  // window half-extent: where impostors dissolve

    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout      pipelineLayout_      = VK_NULL_HANDLE;
    VkPipeline            pipeline_            = VK_NULL_HANDLE;

    std::vector<Buffer>          uniformBuffers_;
    // The shell mesh lives in ONE device-local buffer (the GPU re-fetches it every
    // frame for the draw, so host-visible was far too slow for ~400k verts). It is
    // rebuilt only when the shell changes (every few blocks); the old buffer is
    // retired for framesInFlight+1 frames before freeing (in-flight frames may use it).
    Buffer                       deviceVB_;
    uint32_t                     deviceVertCount_ = 0;
    std::vector<std::pair<int, Buffer>> retired_;
    VkDescriptorPool             descriptorPool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptorSets_;
};

} // namespace vg
