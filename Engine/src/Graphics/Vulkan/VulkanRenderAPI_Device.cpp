#include "VulkanRenderAPI.hpp"
#include "Utils/Log.hpp"
#include <stdio.h>

// SDL Vulkan headers
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

// vk-bootstrap
#include "VkBootstrap.h"

// VMA
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "vk_mem_alloc.h"

static VKAPI_ATTR VkBool32 VKAPI_CALL vulkanDebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* pUserData)
{
    (void)messageType;
    (void)pUserData;

    switch (messageSeverity) {
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
        LOG_ENGINE_ERROR("[Vulkan Validation] {}", pCallbackData->pMessage);
        break;
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
        LOG_ENGINE_WARN("[Vulkan Validation] {}", pCallbackData->pMessage);
        break;
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
        LOG_ENGINE_INFO("[Vulkan Validation] {}", pCallbackData->pMessage);
        break;
    default:
        LOG_ENGINE_TRACE("[Vulkan Validation] {}", pCallbackData->pMessage);
        break;
    }
    return VK_FALSE;
}

bool VulkanRenderAPI::createInstance()
{
#ifdef __APPLE__
    // macOS: ensure MoltenVK ICD is discoverable by the Vulkan loader
    if (!getenv("VK_ICD_FILENAMES") && !getenv("VK_DRIVER_FILES")) {
        setenv("VK_ICD_FILENAMES", "/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json", 0);
    }
#endif

    vkb::InstanceBuilder builder;

    auto inst_ret = builder
        .set_app_name("Garden Engine")
        .set_engine_name("Garden Engine")
        .require_api_version(1, 2, 0)
#ifdef __APPLE__
        .enable_extension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)
#endif
        .request_validation_layers(true)
        .set_debug_callback(vulkanDebugCallback)
        .build();

    if (!inst_ret) {
        printf("Failed to create Vulkan instance: %s\n", inst_ret.error().message().c_str());
        return false;
    }

    vkb_instance = inst_ret.value();
    instance = vkb_instance.instance;

    debug_messenger = vkb_instance.debug_messenger;

    printf("Vulkan instance created\n");
    return true;
}

bool VulkanRenderAPI::createSurface()
{
    if (!SDL_Vulkan_CreateSurface(window_handle, instance, NULL, &surface)) {
        printf("Failed to create Vulkan surface: %s\n", SDL_GetError());
        return false;
    }
    printf("Vulkan surface created\n");
    return true;
}

bool VulkanRenderAPI::selectPhysicalDevice()
{
    vkb::PhysicalDeviceSelector selector{ vkb_instance };

    VkPhysicalDeviceFeatures required_features{};
    required_features.samplerAnisotropy = VK_TRUE;

    auto phys_ret = selector
        .set_surface(surface)
        .set_minimum_version(1, 2)
        .set_required_features(required_features)
        .prefer_gpu_device_type(vkb::PreferredDeviceType::discrete)
        .select();

    if (!phys_ret) {
        printf("Failed to select physical device: %s\n", phys_ret.error().message().c_str());
        return false;
    }

    vkb_physical_device = phys_ret.value();
    physical_device = vkb_physical_device.physical_device;

    // Print device info
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical_device, &props);
    printf("Selected GPU: %s\n", props.deviceName);

    return true;
}

bool VulkanRenderAPI::createLogicalDevice()
{
    vkb::DeviceBuilder device_builder{ vkb_physical_device };
    auto dev_ret = device_builder.build();

    if (!dev_ret) {
        printf("Failed to create logical device: %s\n", dev_ret.error().message().c_str());
        return false;
    }

    vkb::Device vkb_device = dev_ret.value();
    device = vkb_device.device;

    // Get queues
    auto graphics_queue_ret = vkb_device.get_queue(vkb::QueueType::graphics);
    if (!graphics_queue_ret) {
        printf("Failed to get graphics queue\n");
        return false;
    }
    graphics_queue = graphics_queue_ret.value();

    auto present_queue_ret = vkb_device.get_queue(vkb::QueueType::present);
    if (!present_queue_ret) {
        present_queue = graphics_queue;  // Fallback to graphics queue
    } else {
        present_queue = present_queue_ret.value();
    }

    auto queue_index_ret = vkb_device.get_queue_index(vkb::QueueType::graphics);
    if (queue_index_ret) {
        graphics_queue_family = queue_index_ret.value();
    }

    printf("Vulkan device created\n");
    return true;
}

bool VulkanRenderAPI::createVmaAllocator()
{
    VmaVulkanFunctions vulkanFunctions = {};
    vulkanFunctions.vkGetInstanceProcAddr = &vkGetInstanceProcAddr;
    vulkanFunctions.vkGetDeviceProcAddr = &vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo allocatorCreateInfo = {};
    allocatorCreateInfo.vulkanApiVersion = VK_API_VERSION_1_2;
    allocatorCreateInfo.physicalDevice = physical_device;
    allocatorCreateInfo.device = device;
    allocatorCreateInfo.instance = instance;
    allocatorCreateInfo.pVulkanFunctions = &vulkanFunctions;

    if (vmaCreateAllocator(&allocatorCreateInfo, &vma_allocator) != VK_SUCCESS) {
        printf("Failed to create VMA allocator\n");
        return false;
    }

    printf("VMA allocator created\n");

    m_rgBackend.init(device, vma_allocator);
    return true;
}
