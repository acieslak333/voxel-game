#include "render/ChunkRenderer.h"

#include "render/Pipeline.h"
#include "render/TextureArray.h"
#include "render/VulkanContext.h"
#include "world/ChunkMesher.h"

// GLM_FORCE_RADIANS / GLM_FORCE_DEPTH_ZERO_TO_ONE are defined globally by CMake.
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <stdexcept>

namespace vg {

ChunkRenderer::ChunkRenderer(VulkanContext& ctx, VkRenderPass renderPass,
                             uint32_t framesInFlight, const std::string& shaderDir,
                             const std::string& textureDir)
    : ctx_(ctx) {
    // Load the textures named by the registry into the array, in layer order.
    textures_ = std::make_unique<TextureArray>(ctx_, registry_.texturePaths(), textureDir);

    pipeline_ = std::make_unique<Pipeline>(ctx_, renderPass,
                                           shaderDir + "/chunk.vert.spv",
                                           shaderDir + "/chunk.frag.spv");

    buildWorld();
    createUniformBuffers(framesInFlight);
    createDescriptorSets(framesInFlight);
}

ChunkRenderer::~ChunkRenderer() {
    // Descriptor sets are freed with the pool.
    if (descriptorPool_) {
        vkDestroyDescriptorPool(ctx_.device(), descriptorPool_, nullptr);
    }
}

void ChunkRenderer::buildWorld() {
    // ---- Hardcode a simple layered terrain ----------------------------------
    //   y 0-1 : stone
    //   y 2-4 : dirt
    //   y 5   : grass
    //   above : air
    // Flat layers make the greedy mesher's per-material merging and the
    // per-block texture tiling easy to see.
    auto place = [&](int x, int y, int z, BlockId id) {
        if (Chunk::inBounds(x, y, z)) {
            chunk_.set(x, y, z, Block{static_cast<uint16_t>(id), 0});
        }
    };

    for (int z = 0; z < Chunk::kSize; ++z) {
        for (int x = 0; x < Chunk::kSize; ++x) {
            for (int y = 0; y <= 5; ++y) {
                BlockId id = (y <= 1) ? BlockId::Stone
                           : (y <= 4) ? BlockId::Dirt
                                      : BlockId::Grass;
                place(x, y, z, id);
            }
        }
    }

    // A few features so the camera, collision and jumping have something to do
    // (Milestone 2): a staircase, a low wall, and a stone pillar.
    for (int s = 0; s < 4; ++s) {                 // staircase climbing in +x
        for (int y = 6; y <= 6 + s; ++y) {
            place(3 + s, y, 4, BlockId::Stone);
        }
    }
    for (int z = 8; z < 14; ++z) {                // a 2-high wall to walk into
        place(11, 6, z, BlockId::Dirt);
        place(11, 7, z, BlockId::Grass);
    }
    for (int y = 6; y <= 9; ++y) {                // a tall pillar landmark
        place(13, y, 3, BlockId::Stone);
    }

    // ---- Mesh it and upload to the GPU --------------------------------------
    MeshData mesh = ChunkMesher::greedyMesh(chunk_, registry_);
    if (mesh.empty()) {
        throw std::runtime_error("Chunk produced an empty mesh");
    }

    vertexBuffer_ = Buffer::createDeviceLocal(
        ctx_, mesh.vertices.data(), sizeof(Vertex) * mesh.vertices.size(),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    indexBuffer_ = Buffer::createDeviceLocal(
        ctx_, mesh.indices.data(), sizeof(uint32_t) * mesh.indices.size(),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    indexCount_ = static_cast<uint32_t>(mesh.indices.size());
}

void ChunkRenderer::createUniformBuffers(uint32_t n) {
    uniformBuffers_.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        uniformBuffers_.emplace_back(ctx_, sizeof(CameraUBO),
                                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
}

void ChunkRenderer::createDescriptorSets(uint32_t n) {
    // Pool big enough for n sets, each with one UBO and one sampler.
    std::array<VkDescriptorPoolSize, 2> sizes{};
    sizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, n};
    sizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, n};

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
    poolInfo.pPoolSizes    = sizes.data();
    poolInfo.maxSets       = n;
    if (vkCreateDescriptorPool(ctx_.device(), &poolInfo, nullptr, &descriptorPool_) !=
        VK_SUCCESS) {
        throw std::runtime_error("Failed to create descriptor pool");
    }

    std::vector<VkDescriptorSetLayout> layouts(n, pipeline_->descriptorSetLayout());
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool     = descriptorPool_;
    alloc.descriptorSetCount = n;
    alloc.pSetLayouts        = layouts.data();

    descriptorSets_.resize(n);
    if (vkAllocateDescriptorSets(ctx_.device(), &alloc, descriptorSets_.data()) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate descriptor sets");
    }

    // Point each set at its own UBO + the shared texture array.
    for (uint32_t i = 0; i < n; ++i) {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = uniformBuffers_[i].handle();
        bufferInfo.offset = 0;
        bufferInfo.range  = sizeof(CameraUBO);

        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView   = textures_->view();
        imageInfo.sampler     = textures_->sampler();

        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = descriptorSets_[i];
        writes[0].dstBinding      = 0;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo     = &bufferInfo;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = descriptorSets_[i];
        writes[1].dstBinding      = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo      = &imageInfo;

        vkUpdateDescriptorSets(ctx_.device(), static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }
}

void ChunkRenderer::record(VkCommandBuffer cmd, uint32_t frameIndex, VkExtent2D extent,
                           const glm::mat4& view, const glm::mat4& proj) {
    // --- Update this frame's camera UBO --------------------------------------
    CameraUBO ubo{};
    ubo.view = view;
    ubo.proj = proj;
    uniformBuffers_[frameIndex].upload(&ubo, sizeof(ubo));

    // --- Bind pipeline + dynamic state ---------------------------------------
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->handle());

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width  = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = extent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->layout(),
                            0, 1, &descriptorSets_[frameIndex], 0, nullptr);

    // Single chunk at the origin -> identity model matrix. In Milestone 3 this
    // becomes per-chunk world translation.
    glm::mat4 model(1.0f);
    vkCmdPushConstants(cmd, pipeline_->layout(), VK_SHADER_STAGE_VERTEX_BIT, 0,
                       Pipeline::kPushConstantSize, &model);

    VkBuffer vbs[] = {vertexBuffer_.handle()};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, vbs, offsets);
    vkCmdBindIndexBuffer(cmd, indexBuffer_.handle(), 0, VK_INDEX_TYPE_UINT32);

    vkCmdDrawIndexed(cmd, indexCount_, 1, 0, 0, 0);
}

bool ChunkRenderer::isSolidAt(int x, int y, int z) const {
    return registry_.isSolid(chunk_.getOrAir(x, y, z).id);
}

} // namespace vg
