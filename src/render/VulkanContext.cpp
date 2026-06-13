#include "render/VulkanContext.h"

#include "core/Window.h"
#include "render/GpuAllocator.h"

#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

namespace vg {

namespace {

// The standard validation layer bundle shipped with the Vulkan SDK.
const std::vector<const char*> kValidationLayers = {
    "VK_LAYER_KHRONOS_validation",
};

// Device extensions we require: the ability to present via a swapchain.
const std::vector<const char*> kDeviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};

// --- Debug messenger plumbing -----------------------------------------------
// vkCreateDebugUtilsMessengerEXT / vkDestroyDebugUtilsMessengerEXT are
// extension functions and must be looked up at runtime via vkGetInstanceProcAddr.

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* /*userData*/) {
    // Only surface warnings and errors; info/verbose would drown the console.
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::cerr << "[vulkan] " << data->pMessage << '\n';
    }
    return VK_FALSE; // do not abort the offending call
}

VkResult createDebugUtilsMessengerEXT(
    VkInstance instance,
    const VkDebugUtilsMessengerCreateInfoEXT* createInfo,
    const VkAllocationCallbacks* allocator,
    VkDebugUtilsMessengerEXT* messenger) {
    auto fn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    return fn ? fn(instance, createInfo, allocator, messenger)
              : VK_ERROR_EXTENSION_NOT_PRESENT;
}

void destroyDebugUtilsMessengerEXT(
    VkInstance instance,
    VkDebugUtilsMessengerEXT messenger,
    const VkAllocationCallbacks* allocator) {
    auto fn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (fn) {
        fn(instance, messenger, allocator);
    }
}

void populateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& info) {
    info = {};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = debugCallback;
}

bool validationLayersSupported() {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> available(count);
    vkEnumerateInstanceLayerProperties(&count, available.data());

    for (const char* wanted : kValidationLayers) {
        bool found = false;
        for (const auto& layer : available) {
            if (std::strcmp(wanted, layer.layerName) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

} // namespace

// -----------------------------------------------------------------------------

VulkanContext::VulkanContext(const Window& window) : window_(window) {
#ifdef VG_ENABLE_VALIDATION
    validationEnabled_ = validationLayersSupported();
    if (!validationEnabled_) {
        std::cerr << "[vulkan] validation layers requested but not available; "
                     "continuing without them\n";
    }
#endif

    createInstance();
    setupDebugMessenger();
    surface_ = window_.createSurface(instance_);
    pickPhysicalDevice();
    createLogicalDevice();
    createTransientCommandPool();
    allocator_ = std::make_unique<GpuAllocator>(*this);
}

VulkanContext::~VulkanContext() {
    // Tear down in reverse creation order. The allocator frees its device memory
    // blocks here, while the device is still alive and after every Buffer that
    // sub-allocated from it has been destroyed (renderers outlived by this ctx).
    allocator_.reset();
    if (transientPool_) {
        vkDestroyCommandPool(device_, transientPool_, nullptr);
    }
    if (device_) {
        vkDestroyDevice(device_, nullptr);
    }
    if (surface_) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
    }
    if (debugMessenger_) {
        destroyDebugUtilsMessengerEXT(instance_, debugMessenger_, nullptr);
    }
    if (instance_) {
        vkDestroyInstance(instance_, nullptr);
    }
}

GpuAllocator& VulkanContext::allocator() const {
    return *allocator_;
}

void VulkanContext::createInstance() {
    VkApplicationInfo appInfo{};
    appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName   = "Voxel Survival Game";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 3, 0);
    appInfo.pEngineName        = "No Engine";
    appInfo.engineVersion      = VK_MAKE_VERSION(0, 3, 0);
    appInfo.apiVersion         = VK_API_VERSION_1_2;

    // Assemble the required instance extensions: those GLFW needs to present,
    // plus the debug-utils extension when validation is on.
    std::vector<const char*> extensions = Window::requiredInstanceExtensions();
    if (validationEnabled_) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkInstanceCreateInfo createInfo{};
    createInfo.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo        = &appInfo;
    createInfo.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    // Chaining a debug messenger create-info here lets validation cover the
    // vkCreateInstance / vkDestroyInstance calls themselves.
    VkDebugUtilsMessengerCreateInfoEXT debugInfo{};
    if (validationEnabled_) {
        createInfo.enabledLayerCount   = static_cast<uint32_t>(kValidationLayers.size());
        createInfo.ppEnabledLayerNames = kValidationLayers.data();
        populateDebugMessengerCreateInfo(debugInfo);
        createInfo.pNext = &debugInfo;
    }

    if (vkCreateInstance(&createInfo, nullptr, &instance_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create Vulkan instance");
    }
}

void VulkanContext::setupDebugMessenger() {
    if (!validationEnabled_) {
        return;
    }
    VkDebugUtilsMessengerCreateInfoEXT info{};
    populateDebugMessengerCreateInfo(info);
    if (createDebugUtilsMessengerEXT(instance_, &info, nullptr, &debugMessenger_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to set up debug messenger");
    }
}

void VulkanContext::pickPhysicalDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) {
        throw std::runtime_error("No Vulkan-capable GPUs found");
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    for (VkPhysicalDevice candidate : devices) {
        if (isDeviceSuitable(candidate)) {
            physicalDevice_ = candidate;
            break;
        }
    }
    if (physicalDevice_ == VK_NULL_HANDLE) {
        throw std::runtime_error("No suitable GPU found (needs graphics + present + swapchain)");
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice_, &props);
    std::cout << "[vulkan] using GPU: " << props.deviceName << '\n';

    queueFamilies_ = findQueueFamilies(physicalDevice_);
}

void VulkanContext::createLogicalDevice() {
    // De-duplicate the queue family indices (graphics + present may coincide).
    std::set<uint32_t> uniqueFamilies = {
        queueFamilies_.graphics.value(),
        queueFamilies_.present.value(),
    };

    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    float priority = 1.0f;
    for (uint32_t family : uniqueFamilies) {
        VkDeviceQueueCreateInfo qi{};
        qi.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = family;
        qi.queueCount       = 1;
        qi.pQueuePriorities = &priority;
        queueInfos.push_back(qi);
    }

    // No special device features required yet; later milestones may enable
    // e.g. samplerAnisotropy here.
    VkPhysicalDeviceFeatures features{};

    VkDeviceCreateInfo createInfo{};
    createInfo.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount    = static_cast<uint32_t>(queueInfos.size());
    createInfo.pQueueCreateInfos       = queueInfos.data();
    createInfo.pEnabledFeatures        = &features;
    createInfo.enabledExtensionCount   = static_cast<uint32_t>(kDeviceExtensions.size());
    createInfo.ppEnabledExtensionNames = kDeviceExtensions.data();
    // Device-level layers are deprecated and have never done anything since Vulkan
    // 1.0; modern validation flags setting them (VUID-VkDeviceCreateInfo-
    // enabledLayerCount-12384). Leave enabledLayerCount at 0 — the validation layer
    // is already enabled at the instance level (createInstance), which covers all
    // device calls. (Old loaders that needed device layers are long gone.)

    if (vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create logical device");
    }

    vkGetDeviceQueue(device_, queueFamilies_.graphics.value(), 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, queueFamilies_.present.value(), 0, &presentQueue_);
}

QueueFamilyIndices VulkanContext::findQueueFamilies(VkPhysicalDevice device) const {
    QueueFamilyIndices indices;

    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

    for (uint32_t i = 0; i < count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            indices.graphics = i;
        }
        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &presentSupport);
        if (presentSupport) {
            indices.present = i;
        }
        if (indices.isComplete()) {
            break;
        }
    }
    return indices;
}

bool VulkanContext::isDeviceSuitable(VkPhysicalDevice device) const {
    QueueFamilyIndices indices = findQueueFamilies(device);
    if (!indices.isComplete()) {
        return false;
    }
    if (!checkDeviceExtensionSupport(device)) {
        return false;
    }
    // A device is only usable if its swapchain support is non-empty.
    SwapChainSupportDetails support = querySwapChainSupport(device);
    return !support.formats.empty() && !support.presentModes.empty();
}

bool VulkanContext::checkDeviceExtensionSupport(VkPhysicalDevice device) const {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, available.data());

    std::set<std::string> required(kDeviceExtensions.begin(), kDeviceExtensions.end());
    for (const auto& ext : available) {
        required.erase(ext.extensionName);
    }
    return required.empty();
}

SwapChainSupportDetails VulkanContext::querySwapChainSupport() const {
    return querySwapChainSupport(physicalDevice_);
}

SwapChainSupportDetails VulkanContext::querySwapChainSupport(VkPhysicalDevice device) const {
    SwapChainSupportDetails details;

    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface_, &details.capabilities);

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount, nullptr);
    details.formats.resize(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount, details.formats.data());

    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &presentModeCount, nullptr);
    details.presentModes.resize(presentModeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &presentModeCount,
                                              details.presentModes.data());

    return details;
}

void VulkanContext::createTransientCommandPool() {
    VkCommandPoolCreateInfo info{};
    info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    info.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    info.queueFamilyIndex = queueFamilies_.graphics.value();
    if (vkCreateCommandPool(device_, &info, nullptr, &transientPool_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create transient command pool");
    }
}

VkCommandBuffer VulkanContext::beginSingleTimeCommands() const {
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandPool        = transientPool_;
    alloc.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &alloc, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    return cmd;
}

void VulkanContext::endSingleTimeCommands(VkCommandBuffer cmd) const {
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cmd;

    // Simplicity over throughput: submit and wait for the GPU to finish. These
    // are one-off setup operations, not per-frame work.
    vkQueueSubmit(graphicsQueue_, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphicsQueue_);

    vkFreeCommandBuffers(device_, transientPool_, 1, &cmd);
}

uint32_t VulkanContext::findMemoryType(uint32_t typeFilter,
                                       VkMemoryPropertyFlags properties) const {
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        const bool typeOk = (typeFilter & (1u << i)) != 0;
        const bool propsOk =
            (memProps.memoryTypes[i].propertyFlags & properties) == properties;
        if (typeOk && propsOk) {
            return i;
        }
    }
    throw std::runtime_error("Failed to find a suitable memory type");
}

} // namespace vg
