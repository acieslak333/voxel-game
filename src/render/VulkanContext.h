#pragma once

/**
 * @file VulkanContext.h
 * @brief Device-level Vulkan context: instance, surface, physical/logical device, queues.
 *
 * Owns the long-lived objects (instance -> debug messenger -> surface ->
 * physicalDevice -> device) that outlive the swapchain and are shared by every
 * renderer subsystem. Also owns the GpuAllocator (shared block sub-allocator)
 * and the transient command pool used for one-off GPU operations.
 * @warning All Vulkan object creation and destruction must happen on the main thread.
 * @see docs/CODE_INDEX.md
 */

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace vg {

class Window;
class GpuAllocator;

// -----------------------------------------------------------------------------
//  Queue families
// -----------------------------------------------------------------------------
//  Vulkan exposes work queues grouped into "families". We need a family that
//  supports graphics commands and one that can present to our window surface.
//  On most GPUs these are the same family, but the API allows them to differ,
//  so we track both indices.
// -----------------------------------------------------------------------------
/** @brief Resolved graphics and present queue-family indices for the chosen GPU. */
struct QueueFamilyIndices {
    std::optional<uint32_t> graphics;
    std::optional<uint32_t> present;
    [[nodiscard]] bool isComplete() const {
        return graphics.has_value() && present.has_value();
    }
};

/** @brief Swapchain capabilities, formats, and present modes for the current surface. */
struct SwapChainSupportDetails {
    VkSurfaceCapabilitiesKHR        capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR>   presentModes;
};

// -----------------------------------------------------------------------------
//  VulkanContext
// -----------------------------------------------------------------------------
//  Owns the long-lived, "device level" Vulkan objects that the rest of the
//  renderer builds on top of:
//      instance -> debug messenger -> surface -> physical device -> device
//  These outlive the swapchain (which is rebuilt on every resize), so they
//  live here in their own object with strict RAII teardown.
// -----------------------------------------------------------------------------
/**
 * @brief Owns the core Vulkan device-level objects shared by the entire renderer.
 *
 * Constructs the Vulkan instance, optional debug messenger, window surface,
 * physical device selection, logical device, graphics/present queues, a transient
 * command pool for one-off GPU work, and the shared GpuAllocator block pool.
 * Destroyed in reverse order; all Buffers drawing from the allocator must be
 * freed before this object is torn down.
 * @warning Must be created and destroyed on the main thread only.
 */
class VulkanContext {
public:
    explicit VulkanContext(const Window& window);
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    [[nodiscard]] VkInstance       instance()       const { return instance_; }
    [[nodiscard]] VkSurfaceKHR     surface()        const { return surface_; }
    [[nodiscard]] VkPhysicalDevice physicalDevice() const { return physicalDevice_; }
    [[nodiscard]] VkDevice         device()         const { return device_; }
    [[nodiscard]] VkQueue          graphicsQueue()  const { return graphicsQueue_; }
    [[nodiscard]] VkQueue          presentQueue()   const { return presentQueue_; }
    [[nodiscard]] const QueueFamilyIndices& queueFamilies() const { return queueFamilies_; }

    // Re-query swapchain support for the chosen device + surface. The surface
    // capabilities (notably current extent) change on resize, so this is called
    // every time the swapchain is rebuilt rather than cached.
    [[nodiscard]] SwapChainSupportDetails querySwapChainSupport() const;

    // Pick a memory type index matching a type filter + desired properties.
    // Used by buffer/image allocation in later milestones.
    [[nodiscard]] uint32_t findMemoryType(uint32_t typeFilter,
                                          VkMemoryPropertyFlags properties) const;

    // Shared block sub-allocator backing every Buffer's device memory (removes the
    // per-buffer vkAllocateMemory ceiling). Defined out-of-line so the header only
    // needs a forward declaration. Lives as long as the device.
    [[nodiscard]] GpuAllocator& allocator() const;

    // Allocate + begin a primary command buffer for a one-off operation (buffer
    // copies, image layout transitions). Pair with endSingleTimeCommands, which
    // submits it and blocks until it has finished.
    [[nodiscard]] VkCommandBuffer beginSingleTimeCommands() const;
    void endSingleTimeCommands(VkCommandBuffer cmd) const;

private:
    void createInstance();
    void setupDebugMessenger();
    void pickPhysicalDevice();
    void createLogicalDevice();
    void createTransientCommandPool();

    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device) const;
    bool isDeviceSuitable(VkPhysicalDevice device) const;
    bool checkDeviceExtensionSupport(VkPhysicalDevice device) const;
    SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device) const;

    const Window& window_;

    VkInstance               instance_       = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR             surface_        = VK_NULL_HANDLE;
    VkPhysicalDevice         physicalDevice_ = VK_NULL_HANDLE; // implicitly freed with instance
    VkDevice                 device_         = VK_NULL_HANDLE;

    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_  = VK_NULL_HANDLE;

    // Small pool for short-lived one-off command buffers (staging copies, etc.).
    VkCommandPool transientPool_ = VK_NULL_HANDLE;

    // Block sub-allocator for all device memory behind Buffers. Created after the
    // logical device, destroyed first in ~VulkanContext (while the device is still
    // alive) — after every Buffer that drew from it has already been freed.
    std::unique_ptr<GpuAllocator> allocator_;

    QueueFamilyIndices queueFamilies_;

    bool validationEnabled_ = false;
};

} // namespace vg
