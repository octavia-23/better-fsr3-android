// Copyright © 2026 Arm Limited.
// SPDX-License-Identifier: MIT

#include "vk_context.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace {

bool hasExtension(const std::vector<VkExtensionProperties>& list, const char* name)
{
    for (const auto& e : list) {
        if (strcmp(e.extensionName, name) == 0)
            return true;
    }
    return false;
}

bool hasLayer(const std::vector<VkLayerProperties>& list, const char* name)
{
    for (const auto& l : list) {
        if (strcmp(l.layerName, name) == 0)
            return true;
    }
    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void*)
{
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        LOGE("validation: %s", data->pMessage);
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        LOGW("validation: %s", data->pMessage);
    return VK_FALSE;
}

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

} // namespace

bool VkContext::createInstance(bool enableValidation)
{
    uint32_t loaderVersion = VK_API_VERSION_1_0;
    auto enumerateInstanceVersion = (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(
        VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    if (enumerateInstanceVersion) {
        VK_CHECK(enumerateInstanceVersion(&loaderVersion));
    }
    LOGI("Vulkan loader reports instance version %u.%u.%u",
         VK_API_VERSION_MAJOR(loaderVersion), VK_API_VERSION_MINOR(loaderVersion),
         VK_API_VERSION_PATCH(loaderVersion));

    VkApplicationInfo appInfo = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "Arm ASR Sample";
    appInfo.apiVersion       = loaderVersion >= VK_API_VERSION_1_2 ? VK_API_VERSION_1_2
                                                                  : VK_API_VERSION_1_1;

    std::vector<const char*> extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };
    std::vector<const char*> layers;

    // The desktop is the one place the validation layer actually runs against
    // the Arm ASR backend, so make it easy to switch on - but never require it.
    if (enableValidation) {
        uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> availableLayers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

        uint32_t instExtCount = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &instExtCount, nullptr);
        std::vector<VkExtensionProperties> availableExts(instExtCount);
        vkEnumerateInstanceExtensionProperties(nullptr, &instExtCount, availableExts.data());

        if (hasLayer(availableLayers, kValidationLayer) &&
            hasExtension(availableExts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            layers.push_back(kValidationLayer);
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            validationEnabled_ = true;
            LOGI("Validation layer enabled");
        } else {
            LOGW("Validation requested but %s / %s is not installed - continuing without it",
                 kValidationLayer, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
    }

    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo        = &appInfo;
    ci.enabledExtensionCount   = (uint32_t)extensions.size();
    ci.ppEnabledExtensionNames = extensions.data();
    ci.enabledLayerCount       = (uint32_t)layers.size();
    ci.ppEnabledLayerNames     = layers.empty() ? nullptr : layers.data();

    VK_CHECK(vkCreateInstance(&ci, nullptr, &instance_));

    if (validationEnabled_)
        createDebugMessenger();
    return true;
}

void VkContext::createDebugMessenger()
{
    auto create = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
        instance_, "vkCreateDebugUtilsMessengerEXT");
    if (!create)
        return;

    VkDebugUtilsMessengerCreateInfoEXT ci = {
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    ci.pfnUserCallback = debugCallback;
    create(instance_, &ci, nullptr, &debugMessenger_);
}

void VkContext::destroyDebugMessenger()
{
    if (debugMessenger_ == VK_NULL_HANDLE)
        return;
    auto destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
        instance_, "vkDestroyDebugUtilsMessengerEXT");
    if (destroy)
        destroy(instance_, debugMessenger_, nullptr);
    debugMessenger_ = VK_NULL_HANDLE;
}

bool VkContext::createSurface(HWND window)
{
    VkWin32SurfaceCreateInfoKHR ci = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    ci.hinstance = GetModuleHandleW(nullptr);
    ci.hwnd      = window;
    VK_CHECK(vkCreateWin32SurfaceKHR(instance_, &ci, nullptr, &surface_));
    return true;
}

bool VkContext::pickPhysicalDevice()
{
    uint32_t count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(instance_, &count, nullptr));
    if (count == 0) {
        LOGE("No Vulkan physical devices");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    VK_CHECK(vkEnumeratePhysicalDevices(instance_, &count, devices.data()));

    for (VkPhysicalDevice candidate : devices) {
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
        std::vector<VkQueueFamilyProperties> props(families);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, props.data());

        for (uint32_t i = 0; i < families; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface_, &present);
            const bool graphics = (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
            const bool compute  = (props[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
            if (graphics && compute && present) {
                physicalDevice_ = candidate;
                queueFamily_    = i;
                break;
            }
        }
        if (physicalDevice_ != VK_NULL_HANDLE)
            break;
    }

    if (physicalDevice_ == VK_NULL_HANDLE) {
        LOGE("No physical device with a graphics+compute+present queue");
        return false;
    }

    VkPhysicalDeviceProperties deviceProps = {};
    vkGetPhysicalDeviceProperties(physicalDevice_, &deviceProps);
    LOGI("Using device '%s' (Vulkan %u.%u.%u)", deviceProps.deviceName,
         VK_API_VERSION_MAJOR(deviceProps.apiVersion),
         VK_API_VERSION_MINOR(deviceProps.apiVersion),
         VK_API_VERSION_PATCH(deviceProps.apiVersion));

    // Arm ASR ships prebuilt SPIR-V 1.4 blobs built with -enable-16bit-types.
    // Establish up front whether this device can consume them at all.
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extCount, exts.data());

    caps_.apiVersion       = deviceProps.apiVersion;
    caps_.spirv14          = deviceProps.apiVersion >= VK_API_VERSION_1_2 ||
                             hasExtension(exts, VK_KHR_SPIRV_1_4_EXTENSION_NAME);
    caps_.subgroupSizeCtrl = hasExtension(exts, VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);

    VkPhysicalDevice16BitStorageFeatures storage16 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
    VkPhysicalDeviceShaderFloat16Int8Features f16 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
    VkPhysicalDeviceFeatures2 features2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};

    void** ppNext = &features2.pNext;
    if (hasExtension(exts, VK_KHR_16BIT_STORAGE_EXTENSION_NAME) ||
        deviceProps.apiVersion >= VK_API_VERSION_1_1) {
        *ppNext = &storage16;
        ppNext = &storage16.pNext;
    }
    if (hasExtension(exts, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME) ||
        deviceProps.apiVersion >= VK_API_VERSION_1_2) {
        *ppNext = &f16;
        ppNext = &f16.pNext;
    }
    vkGetPhysicalDeviceFeatures2(physicalDevice_, &features2);

    caps_.storage16bit              = storage16.storageBuffer16BitAccess == VK_TRUE;
    caps_.shaderFloat16             = f16.shaderFloat16 == VK_TRUE;
    caps_.storageExtendedFormats    = features2.features.shaderStorageImageExtendedFormats == VK_TRUE;
    caps_.storageWriteWithoutFormat = features2.features.shaderStorageImageWriteWithoutFormat == VK_TRUE;

    auto formatName = [](VkFormat format) -> const char* {
        switch (format) {
        case VK_FORMAT_R32_SFLOAT: return "R32_SFLOAT";
        case VK_FORMAT_R16G16_SFLOAT: return "R16G16_SFLOAT";
        case VK_FORMAT_R32_UINT: return "R32_UINT";
        case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
        case VK_FORMAT_R8_UNORM: return "R8_UNORM";
        case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case VK_FORMAT_R16_SFLOAT: return "R16_SFLOAT";
        case VK_FORMAT_R32G32B32A32_SFLOAT: return "R32G32B32A32_SFLOAT";
        case VK_FORMAT_D32_SFLOAT: return "D32_SFLOAT";
        case VK_FORMAT_R16_SNORM: return "R16_SNORM";
        case VK_FORMAT_R32G32_SFLOAT: return "R32G32_SFLOAT";
        default: return "UNKNOWN_FORMAT";
        }
    };

    // Check required storage image format support across all 19 surfaces + external inputs
    const VkFormat storageFormats[] = {
        VK_FORMAT_R32_SFLOAT,
        VK_FORMAT_R16G16_SFLOAT,
        VK_FORMAT_R32_UINT,
        VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_R8_UNORM,
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_R16_SFLOAT,
        VK_FORMAT_R32G32B32A32_SFLOAT
    };
    bool storageOk = true;
    for (VkFormat f : storageFormats) {
        VkFormatProperties props = {};
        vkGetPhysicalDeviceFormatProperties(physicalDevice_, f, &props);
        if ((props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0) {
            LOGE("Missing VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT for format %s (%d)", formatName(f), (int)f);
            storageOk = false;
        }
    }
    caps_.storageFormatsSupported = storageOk;

    // Check required sampled image format support across all inputs and pipeline bindings
    const VkFormat sampledFormats[] = {
        VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_R16G16_SFLOAT,
        VK_FORMAT_R32_SFLOAT,
        VK_FORMAT_R32_UINT,
        VK_FORMAT_R8_UNORM,
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_R16_SNORM,
        VK_FORMAT_R32G32_SFLOAT,
        VK_FORMAT_R16_SFLOAT,
        VK_FORMAT_R32G32B32A32_SFLOAT
    };
    bool sampledOk = true;
    for (VkFormat f : sampledFormats) {
        VkFormatProperties props = {};
        vkGetPhysicalDeviceFormatProperties(physicalDevice_, f, &props);
        if ((props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0) {
            LOGE("Missing VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT for format %s (%d)", formatName(f), (int)f);
            sampledOk = false;
        }
    }
    caps_.sampledFormatsSupported = sampledOk;

    LOGI("Arm ASR / FSR 3.1.4 device gate: SPIR-V 1.4 %s | shaderFloat16 %s | 16-bit storage %s | "
         "extended formats %s | write without format %s | storage formats %s | sampled formats %s",
         caps_.spirv14 ? "yes" : "NO", caps_.shaderFloat16 ? "yes" : "NO",
         caps_.storage16bit ? "yes" : "NO", caps_.storageExtendedFormats ? "yes" : "NO",
         caps_.storageWriteWithoutFormat ? "yes" : "NO",
         caps_.storageFormatsSupported ? "yes" : "NO",
         caps_.sampledFormatsSupported ? "yes" : "NO");

    return true;
}

bool VkContext::createDevice()
{
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> available(extCount);
    vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extCount, available.data());

    std::vector<const char*> enabled;
    enabled.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    // On a Vulkan 1.2 device these are core and must not be requested again.
    const bool core12 = caps_.apiVersion >= VK_API_VERSION_1_2;
    auto tryEnable = [&](const char* name) {
        if (!core12 && hasExtension(available, name))
            enabled.push_back(name);
    };
    tryEnable(VK_KHR_SPIRV_1_4_EXTENSION_NAME);
    tryEnable(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
    tryEnable(VK_KHR_16BIT_STORAGE_EXTENSION_NAME);
    tryEnable(VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);
    if (caps_.subgroupSizeCtrl)
        enabled.push_back(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);

    VkPhysicalDevice16BitStorageFeatures storage16 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
    storage16.storageBuffer16BitAccess           = VK_TRUE;
    storage16.uniformAndStorageBuffer16BitAccess = VK_TRUE;

    VkPhysicalDeviceShaderFloat16Int8Features f16 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
    f16.shaderFloat16 = caps_.shaderFloat16 ? VK_TRUE : VK_FALSE;
    f16.pNext         = &storage16;

    VkPhysicalDeviceFeatures2 features2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features2.pNext = &f16;
    vkGetPhysicalDeviceFeatures2(physicalDevice_, &features2);
    // Keep only what the sample and Arm ASR actually need.
    VkPhysicalDeviceFeatures wanted = {};
    wanted.shaderInt16                          = features2.features.shaderInt16;
    wanted.samplerAnisotropy                    = features2.features.samplerAnisotropy;
    wanted.shaderStorageImageExtendedFormats    = features2.features.shaderStorageImageExtendedFormats;
    wanted.shaderStorageImageWriteWithoutFormat = features2.features.shaderStorageImageWriteWithoutFormat;
    features2.features                          = wanted;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueCi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueCi.queueFamilyIndex = queueFamily_;
    queueCi.queueCount       = 1;
    queueCi.pQueuePriorities = &priority;

    VkDeviceCreateInfo ci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pNext                   = &features2;
    ci.queueCreateInfoCount    = 1;
    ci.pQueueCreateInfos       = &queueCi;
    ci.enabledExtensionCount   = (uint32_t)enabled.size();
    ci.ppEnabledExtensionNames = enabled.data();

    VK_CHECK(vkCreateDevice(physicalDevice_, &ci, nullptr, &device_));
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    return true;
}

bool VkContext::createSwapchain(HWND window)
{
    VkSurfaceCapabilitiesKHR surfaceCaps = {};
    VkResult capsRes = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &surfaceCaps);
    if (capsRes != VK_SUCCESS) {
        LOGE("vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed with VkResult %d", (int)capsRes);
        if (capsRes == VK_ERROR_SURFACE_LOST_KHR) {
            sync_.notifySurfaceLost();
        } else if (capsRes == VK_ERROR_DEVICE_LOST) {
            sync_.notifyDeviceLost();
        }
        return false;
    }

    uint32_t formatCount = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, nullptr));
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount,
                                                  formats.data()));

    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM) {
            chosen = f;
            break;
        }
    }

    // Unlike Android, Win32 WSI is allowed to report 0xFFFFFFFF here, meaning
    // "whatever the swapchain asks for". Fall back to the client rect.
    swapchainExtent_ = surfaceCaps.currentExtent;
    if (swapchainExtent_.width == UINT32_MAX || swapchainExtent_.height == UINT32_MAX) {
        RECT client = {};
        GetClientRect(window, &client);
        swapchainExtent_.width  = (uint32_t)(client.right - client.left);
        swapchainExtent_.height = (uint32_t)(client.bottom - client.top);
    }
    swapchainExtent_.width  = std::clamp(swapchainExtent_.width,
                                         surfaceCaps.minImageExtent.width,
                                         surfaceCaps.maxImageExtent.width);
    swapchainExtent_.height = std::clamp(swapchainExtent_.height,
                                         surfaceCaps.minImageExtent.height,
                                         surfaceCaps.maxImageExtent.height);
    if (swapchainExtent_.width == 0 || swapchainExtent_.height == 0) {
        LOGE("Swapchain extent is zero - window is minimised");
        return false;
    }
    swapchainFormat_ = chosen.format;

    uint32_t imageCount = surfaceCaps.minImageCount + 1;
    if (surfaceCaps.maxImageCount > 0 && imageCount > surfaceCaps.maxImageCount)
        imageCount = surfaceCaps.maxImageCount;

    // INHERIT is an Android composite-alpha mode; Win32 WSI normally advertises
    // only OPAQUE, so take whatever the surface actually supports.
    VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (VkCompositeAlphaFlagBitsKHR candidate :
         {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
          VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR}) {
        if (surfaceCaps.supportedCompositeAlpha & candidate) {
            compositeAlpha = candidate;
            break;
        }
    }

    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface          = surface_;
    ci.minImageCount    = imageCount;
    ci.imageFormat      = chosen.format;
    ci.imageColorSpace  = chosen.colorSpace;
    ci.imageExtent      = swapchainExtent_;
    ci.imageArrayLayers = 1;
    ci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform     = surfaceCaps.currentTransform;
    ci.compositeAlpha   = compositeAlpha;
    ci.presentMode      = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped          = VK_TRUE;

    VkResult createRes = vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_);
    if (createRes != VK_SUCCESS) {
        LOGE("vkCreateSwapchainKHR failed with VkResult %d", (int)createRes);
        if (createRes == VK_ERROR_DEVICE_LOST) {
            sync_.notifyDeviceLost();
        } else if (createRes == VK_ERROR_SURFACE_LOST_KHR) {
            sync_.notifySurfaceLost();
        }
        return false;
    }

    uint32_t count = 0;
    VK_CHECK(vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr));
    swapchainImages_.resize(count);
    VK_CHECK(vkGetSwapchainImagesKHR(device_, swapchain_, &count, swapchainImages_.data()));

    swapchainImageViews_.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo viewCi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewCi.image                       = swapchainImages_[i];
        viewCi.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
        viewCi.format                      = swapchainFormat_;
        viewCi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewCi.subresourceRange.levelCount = 1;
        viewCi.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(device_, &viewCi, nullptr, &swapchainImageViews_[i]));
    }

    LOGI("Swapchain: %ux%u, %u images, format %d", swapchainExtent_.width,
         swapchainExtent_.height, count, (int)swapchainFormat_);
    return true;
}

void VkContext::destroySwapchain()
{
    for (VkImageView view : swapchainImageViews_)
        vkDestroyImageView(device_, view, nullptr);
    swapchainImageViews_.clear();
    swapchainImages_.clear();

    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

bool VkContext::createFrameResources()
{
    VkCommandPoolCreateInfo poolCi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolCi.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolCi.queueFamilyIndex = queueFamily_;
    VK_CHECK(vkCreateCommandPool(device_, &poolCi, nullptr, &commandPool_));

    VkCommandBufferAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool        = commandPool_;
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = kFramesInFlight;
    VK_CHECK(vkAllocateCommandBuffers(device_, &allocInfo, commandBuffers_));

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        VkSemaphoreCreateInfo semCi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(device_, &semCi, nullptr, &acquireSemaphores_[i]));
        VK_CHECK(vkCreateSemaphore(device_, &semCi, nullptr, &releaseSemaphores_[i]));

        VkFenceCreateInfo fenceCi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceCi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(device_, &fenceCi, nullptr, &inFlightFences_[i]));
    }

    sync_.init(device_, queue_, swapchain_, swapchainImages_, commandBuffers_, acquireSemaphores_, releaseSemaphores_, inFlightFences_);
    sync_.initStandardFunctions();
    return true;
}

bool VkContext::init(HWND window, bool enableValidation)
{
    return createInstance(enableValidation) && createSurface(window) && pickPhysicalDevice() &&
           createDevice() && createSwapchain(window) && createFrameResources();
}

bool VkContext::recreateSwapchain(HWND window)
{
    if (sync_.isTerminalError() || device_ == VK_NULL_HANDLE) {
        return false;
    }

    VkResult waitRes = vkDeviceWaitIdle(device_);
    if (waitRes != VK_SUCCESS) {
        LOGE("vkDeviceWaitIdle failed during swapchain recreation with VkResult %d", (int)waitRes);
        if (waitRes == VK_ERROR_DEVICE_LOST) {
            sync_.notifyDeviceLost();
        } else {
            sync_.notifyRecoveryFailed();
        }
        return false;
    }

    destroySwapchain();
    bool ok = createSwapchain(window);
    if (ok) {
        sync_.init(device_, queue_, swapchain_, swapchainImages_, commandBuffers_, acquireSemaphores_, releaseSemaphores_, inFlightFences_);
        sync_.onSwapchainRecreated();
    }
    return ok;
}

bool VkContext::beginFrame(uint32_t* outImageIndex)
{
    return sync_.beginFrame(outImageIndex);
}

bool VkContext::endFrame(uint32_t imageIndex)
{
    return sync_.endFrame(imageIndex);
}

bool VkContext::abortFrame(uint32_t imageIndex)
{
    return sync_.abortFrame(imageIndex);
}

uint32_t VkContext::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const
{
    VkPhysicalDeviceMemoryProperties memProps = {};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return UINT32_MAX;
}

bool VkContext::waitIdle() const
{
    VK_CHECK(vkDeviceWaitIdle(device_));
    return true;
}

void VkContext::destroy()
{
    if (device_ == VK_NULL_HANDLE) {
        // createInstance may have succeeded before a later stage failed.
        if (instance_ != VK_NULL_HANDLE) {
            destroyDebugMessenger();
            if (surface_ != VK_NULL_HANDLE)
                vkDestroySurfaceKHR(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
            vkDestroyInstance(instance_, nullptr);
            instance_ = VK_NULL_HANDLE;
        }
        return;
    }

    vkDeviceWaitIdle(device_);

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        vkDestroySemaphore(device_, acquireSemaphores_[i], nullptr);
        vkDestroySemaphore(device_, releaseSemaphores_[i], nullptr);
        vkDestroyFence(device_, inFlightFences_[i], nullptr);
    }
    vkDestroyCommandPool(device_, commandPool_, nullptr);
    destroySwapchain();
    vkDestroyDevice(device_, nullptr);
    device_ = VK_NULL_HANDLE;

    destroyDebugMessenger();
    vkDestroySurfaceKHR(instance_, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
    vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
}
