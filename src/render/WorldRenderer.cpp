#include "render/WorldRenderer.h"

#include "render/Pipeline.h"
#include "render/TextureArray.h"
#include "render/VulkanContext.h"
#include "world/ChunkMesher.h"
#include "world/World.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <iostream>
#include <stdexcept>

namespace vg {

WorldRenderer::WorldRenderer(VulkanContext& ctx, VkRenderPass renderPass,
                             uint32_t framesInFlight, const World& world,
                             const std::string& shaderDir, const std::string& textureDir)
    : ctx_(ctx), world_(world) {
    textures_ = std::make_unique<TextureArray>(ctx_, world_.registry().texturePaths(),
                                               textureDir);
    pipeline_ = std::make_unique<Pipeline>(ctx_, renderPass,
                                           shaderDir + "/chunk.vert.spv",
                                           shaderDir + "/chunk.frag.spv");
    buildMeshes();
    createUniformBuffers(framesInFlight);
    createDescriptorSets(framesInFlight);
}

WorldRenderer::~WorldRenderer() {
    if (descriptorPool_) {
        vkDestroyDescriptorPool(ctx_.device(), descriptorPool_, nullptr);
    }
}

void WorldRenderer::buildMeshes() {
    const glm::ivec3 counts = world_.chunkCounts();
    for (int cz = 0; cz < counts.z; ++cz) {
        for (int cy = 0; cy < counts.y; ++cy) {
            for (int cx = 0; cx < counts.x; ++cx) {
                MeshData mesh = ChunkMesher::greedyMesh(world_.chunk(cx, cy, cz),
                                                        world_.registry());
                if (mesh.empty()) {
                    continue; // air (or fully-occluded) chunk: nothing to draw
                }

                ChunkMesh cm;
                cm.vertexBuffer = Buffer::createDeviceLocal(
                    ctx_, mesh.vertices.data(), sizeof(Vertex) * mesh.vertices.size(),
                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
                cm.indexBuffer = Buffer::createDeviceLocal(
                    ctx_, mesh.indices.data(), sizeof(uint32_t) * mesh.indices.size(),
                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
                cm.indexCount = static_cast<uint32_t>(mesh.indices.size());
                cm.worldPos = glm::vec3(cx, cy, cz) * static_cast<float>(Chunk::kSize);

                totalTriangles_ += cm.indexCount / 3;
                meshes_.push_back(std::move(cm));
            }
        }
    }
    std::cout << "[world] meshed " << meshes_.size() << " chunks, "
              << totalTriangles_ << " triangles\n";
}

void WorldRenderer::createUniformBuffers(uint32_t n) {
    uniformBuffers_.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        uniformBuffers_.emplace_back(ctx_, sizeof(CameraUBO),
                                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
}

void WorldRenderer::createDescriptorSets(uint32_t n) {
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

void WorldRenderer::record(VkCommandBuffer cmd, uint32_t frameIndex, VkExtent2D extent,
                           const glm::mat4& view, const glm::mat4& proj) {
    CameraUBO ubo{view, proj};
    uniformBuffers_[frameIndex].upload(&ubo, sizeof(ubo));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->handle());

    VkViewport viewport{};
    viewport.width    = static_cast<float>(extent.width);
    viewport.height   = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = extent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->layout(),
                            0, 1, &descriptorSets_[frameIndex], 0, nullptr);

    // One draw per chunk, each translated to its world position via push constant.
    for (const ChunkMesh& m : meshes_) {
        glm::mat4 model = glm::translate(glm::mat4(1.0f), m.worldPos);
        vkCmdPushConstants(cmd, pipeline_->layout(), VK_SHADER_STAGE_VERTEX_BIT, 0,
                           Pipeline::kPushConstantSize, &model);

        VkBuffer vbs[] = {m.vertexBuffer.handle()};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(cmd, 0, 1, vbs, offsets);
        vkCmdBindIndexBuffer(cmd, m.indexBuffer.handle(), 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, m.indexCount, 1, 0, 0, 0);
    }
}

} // namespace vg
