#pragma once

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
struct QueueFamilyIndices {
    std::optional<uint32_t> graphics;
    std::optional<uint32_t> present;
    [[nodiscard]] bool isComplete() const {
        return graphics.has_value() && present.has_value();
    }
};

// Everything we need to know to configure a swapchain for our surface.
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
