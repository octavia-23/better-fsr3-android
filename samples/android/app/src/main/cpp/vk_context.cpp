// Copyright © 2026 Arm Limited.
// SPDX-License-Identifier: MIT

#include "vk_context.h"

#if defined(__ANDROID__)
#include <android/native_window.h>
#endif
#include <vulkan/vulkan_android.h>

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

} // namespace

VkContext::VkContext() = default;

VkContext::~VkContext()
{
    destroy();
}

bool VkContext::createInstance()
{
#if defined(FFXM_VK_CONTEXT_TESTING)
    if (testSeam_) {
        if (testSeam_->failInstanceCreation) {
            return false;
        }
        instance_ = (VkInstance)(uintptr_t)0x1;
        return true;
    }
#endif

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

    const char* extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };

    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo        = &appInfo;
    ci.enabledExtensionCount   = (uint32_t)(sizeof(extensions) / sizeof(extensions[0]));
    ci.ppEnabledExtensionNames = extensions;

    VK_CHECK(vkCreateInstance(&ci, nullptr, &instance_));
    return true;
}

bool VkContext::createSurface(ANativeWindow* window)
{
#if defined(FFXM_VK_CONTEXT_TESTING)
    if (testSeam_) {
        if (testSeam_->failSurfaceCreation || !window) {
            return false;
        }
        surface_ = (VkSurfaceKHR)(uintptr_t)0x2;
        return true;
    }
#endif

    VkAndroidSurfaceCreateInfoKHR ci = {VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    ci.window = window;
    VK_CHECK(vkCreateAndroidSurfaceKHR(instance_, &ci, nullptr, &surface_));
    return true;
}

bool VkContext::pickPhysicalDevice()
{
#if defined(FFXM_VK_CONTEXT_TESTING)
    if (testSeam_) {
        if (testSeam_->failPhysicalDevice) {
            return false;
        }
        physicalDevice_ = (VkPhysicalDevice)(uintptr_t)0x3;
        queueFamily_    = 0;
        caps_.apiVersion               = VK_API_VERSION_1_2;
        caps_.spirv14                  = true;
        caps_.shaderFloat16            = true;
        caps_.storage16bit             = true;
        caps_.storageExtendedFormats   = true;
        caps_.storageWriteWithoutFormat= true;
        caps_.storageFormatsSupported  = true;
        caps_.sampledFormatsSupported  = true;
        if (testSeam_->timestampValidBitsOverride >= 0) {
            timestampValidBits_ = static_cast<uint32_t>(testSeam_->timestampValidBitsOverride);
        } else {
            timestampValidBits_ = 64;
        }
        timestampPeriod_ = 1.0f;
        deviceName_      = "Mock Device";
        driverVersion_   = VK_MAKE_VERSION(1, 0, 0);
        apiVersion_      = VK_API_VERSION_1_2;
        return true;
    }
#endif

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

    deviceName_      = deviceProps.deviceName;
    driverVersion_   = deviceProps.driverVersion;
    apiVersion_      = deviceProps.apiVersion;
    timestampPeriod_ = deviceProps.limits.timestampPeriod;

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &qfCount, nullptr);
    if (qfCount > 0) {
        std::vector<VkQueueFamilyProperties> qfProps(qfCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &qfCount, qfProps.data());
        if (queueFamily_ < qfCount) {
            timestampValidBits_ = qfProps[queueFamily_].timestampValidBits;
        }
    }

    if (timestampValidBits_ == 0) {
        LOGI("GPU timestamp profiling: unsupported (timestampValidBits == 0)");
    } else {
        LOGI("GPU timestamp profiling: supported (%u valid bits, period %.3f ns)",
             timestampValidBits_, timestampPeriod_);
    }

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
#if defined(FFXM_VK_CONTEXT_TESTING)
    if (testSeam_) {
        if (testSeam_->failDeviceCreation) {
            return false;
        }
        device_ = (VkDevice)(uintptr_t)0x4;
        queue_  = (VkQueue)(uintptr_t)0x5;
        return true;
    }
#endif

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

bool VkContext::createSwapchain()
{
#if defined(FFXM_VK_CONTEXT_TESTING)
    if (testSeam_) {
        if (testSeam_->failSwapchainCreation) {
            return false;
        }
        swapchain_           = (VkSwapchainKHR)(uintptr_t)0x10;
        swapchainExtent_     = {1920, 1080};
        swapchainFormat_     = VK_FORMAT_R8G8B8A8_UNORM;
        swapchainImages_     = {(VkImage)(uintptr_t)0x20, (VkImage)(uintptr_t)0x21};
        swapchainImageViews_ = {(VkImageView)(uintptr_t)0x30, (VkImageView)(uintptr_t)0x31};
        return true;
    }
#endif

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

    swapchainExtent_ = surfaceCaps.currentExtent;
    swapchainFormat_ = chosen.format;

    uint32_t imageCount = surfaceCaps.minImageCount + 1;
    if (surfaceCaps.maxImageCount > 0 && imageCount > surfaceCaps.maxImageCount)
        imageCount = surfaceCaps.maxImageCount;

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
    ci.compositeAlpha   = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
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

    swapchainImageViews_.assign(count, VK_NULL_HANDLE);
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
    if (device_ != VK_NULL_HANDLE) {
        for (VkImageView view : swapchainImageViews_) {
            if (view != VK_NULL_HANDLE) {
#if defined(FFXM_VK_CONTEXT_TESTING)
                if (testSeam_) {
                    testSeam_->imageViewDestructions++;
                } else
#endif
                {
                    vkDestroyImageView(device_, view, nullptr);
                }
            }
        }
        if (swapchain_ != VK_NULL_HANDLE) {
#if defined(FFXM_VK_CONTEXT_TESTING)
            if (testSeam_) {
                testSeam_->swapchainDestructions++;
            } else
#endif
            {
                vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            }
        }
    }
    swapchainImageViews_.clear();
    swapchainImages_.clear();
    swapchain_       = VK_NULL_HANDLE;
    swapchainFormat_ = VK_FORMAT_UNDEFINED;
    swapchainExtent_ = {0, 0};
}

bool VkContext::createFrameResources()
{
#if defined(FFXM_VK_CONTEXT_TESTING)
    if (testSeam_) {
        if (testSeam_->failFrameResources) {
            return false;
        }
        commandPool_ = (VkCommandPool)(uintptr_t)0x50;
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            commandBuffers_[i] = (VkCommandBuffer)(uintptr_t)(0x60 + i);
            if (testSeam_->failSemaphoreIndex >= 0 && (int)i == testSeam_->failSemaphoreIndex) {
                return false;
            }
            acquireSemaphores_[i] = (VkSemaphore)(uintptr_t)(0x70 + i);
            releaseSemaphores_[i] = (VkSemaphore)(uintptr_t)(0x80 + i);
            if (testSeam_->failFenceIndex >= 0 && (int)i == testSeam_->failFenceIndex) {
                return false;
            }
            inFlightFences_[i] = (VkFence)(uintptr_t)(0x90 + i);
            if (timestampValidBits_ > 0) {
                queryPools_[i] = (VkQueryPool)(uintptr_t)(0x100 + i);
            } else {
                queryPools_[i] = VK_NULL_HANDLE;
            }
        }
        sync_.init(device_, queue_, swapchain_, swapchainImages_, commandBuffers_,
                   acquireSemaphores_, releaseSemaphores_, inFlightFences_);
        return true;
    }
#endif

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

    if (timestampValidBits_ > 0) {
        VkQueryPoolCreateInfo qpCi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpCi.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        qpCi.queryCount = 32;
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkResult qpRes = vkCreateQueryPool(device_, &qpCi, nullptr, &queryPools_[i]);
            if (qpRes != VK_SUCCESS) {
                LOGW("vkCreateQueryPool failed for frame slot %u (VkResult %d); profiling disabled", i, (int)qpRes);
                for (uint32_t j = 0; j < i; ++j) {
                    vkDestroyQueryPool(device_, queryPools_[j], nullptr);
                    queryPools_[j] = VK_NULL_HANDLE;
                }
                timestampValidBits_ = 0;
                break;
            }
        }
    }

    sync_.init(device_, queue_, swapchain_, swapchainImages_, commandBuffers_, acquireSemaphores_, releaseSemaphores_, inFlightFences_);
    sync_.initStandardFunctions();
    return true;
}

bool VkContext::init(ANativeWindow* window)
{
    if (instance_ != VK_NULL_HANDLE || device_ != VK_NULL_HANDLE || surface_ != VK_NULL_HANDLE) {
        LOGW("VkContext::init called on existing or partial context; resetting prior state");
        destroy();
    }

    if (!window) {
        LOGE("VkContext::init failed: invalid window pointer");
        return false;
    }

    if (!createInstance()) {
        LOGE("VkContext::init failed at stage: createInstance");
        destroy();
        return false;
    }

    if (!createSurface(window)) {
        LOGE("VkContext::init failed at stage: createSurface");
        destroy();
        return false;
    }

    if (!pickPhysicalDevice()) {
        LOGE("VkContext::init failed at stage: pickPhysicalDevice");
        destroy();
        return false;
    }

    if (!createDevice()) {
        LOGE("VkContext::init failed at stage: createDevice");
        destroy();
        return false;
    }

    if (!createSwapchain()) {
        LOGE("VkContext::init failed at stage: createSwapchain");
        destroy();
        return false;
    }

    if (!createFrameResources()) {
        LOGE("VkContext::init failed at stage: createFrameResources");
        destroy();
        return false;
    }

    return true;
}

bool VkContext::recreateSwapchain(ANativeWindow*)
{
    if (sync_.isTerminalError() || device_ == VK_NULL_HANDLE) {
        return false;
    }

#if defined(FFXM_VK_CONTEXT_TESTING)
    if (testSeam_) {
        testSeam_->waitIdleCalls++;
    } else
#endif
    {
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
    }

    destroySwapchain();
    bool ok = createSwapchain();
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
    if (device_ == VK_NULL_HANDLE) {
        return false;
    }
#if defined(FFXM_VK_CONTEXT_TESTING)
    if (testSeam_) {
        testSeam_->waitIdleCalls++;
        return true;
    }
#endif
    VK_CHECK(vkDeviceWaitIdle(device_));
    return true;
}

void VkContext::destroy()
{
    // 1. Wait for the device to become idle when a valid device exists.
    if (device_ != VK_NULL_HANDLE) {
#if defined(FFXM_VK_CONTEXT_TESTING)
        if (testSeam_) {
            testSeam_->waitIdleCalls++;
        } else
#endif
        {
            vkDeviceWaitIdle(device_);
        }
    }

    // 2. Destroy valid device-owned synchronization objects and command resources.
    if (device_ != VK_NULL_HANDLE) {
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (acquireSemaphores_[i] != VK_NULL_HANDLE) {
#if defined(FFXM_VK_CONTEXT_TESTING)
                if (testSeam_) {
                    testSeam_->semaphoreDestructions++;
                } else
#endif
                {
                    vkDestroySemaphore(device_, acquireSemaphores_[i], nullptr);
                }
                acquireSemaphores_[i] = VK_NULL_HANDLE;
            }
            if (releaseSemaphores_[i] != VK_NULL_HANDLE) {
#if defined(FFXM_VK_CONTEXT_TESTING)
                if (testSeam_) {
                    testSeam_->semaphoreDestructions++;
                } else
#endif
                {
                    vkDestroySemaphore(device_, releaseSemaphores_[i], nullptr);
                }
                releaseSemaphores_[i] = VK_NULL_HANDLE;
            }
            if (inFlightFences_[i] != VK_NULL_HANDLE) {
#if defined(FFXM_VK_CONTEXT_TESTING)
                if (testSeam_) {
                    testSeam_->fenceDestructions++;
                } else
#endif
                {
                    vkDestroyFence(device_, inFlightFences_[i], nullptr);
                }
                inFlightFences_[i] = VK_NULL_HANDLE;
            }
            commandBuffers_[i] = VK_NULL_HANDLE;
        }

        if (commandPool_ != VK_NULL_HANDLE) {
#if defined(FFXM_VK_CONTEXT_TESTING)
            if (testSeam_) {
                testSeam_->commandPoolDestructions++;
            } else
#endif
            {
                vkDestroyCommandPool(device_, commandPool_, nullptr);
            }
            commandPool_ = VK_NULL_HANDLE;
        }

        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (queryPools_[i] != VK_NULL_HANDLE) {
#if defined(FFXM_VK_CONTEXT_TESTING)
                if (testSeam_) {
                    testSeam_->queryPoolDestructions++;
                } else
#endif
                {
                    vkDestroyQueryPool(device_, queryPools_[i], nullptr);
                }
                queryPools_[i] = VK_NULL_HANDLE;
            }
        }
    } else {
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            acquireSemaphores_[i] = VK_NULL_HANDLE;
            releaseSemaphores_[i] = VK_NULL_HANDLE;
            inFlightFences_[i]    = VK_NULL_HANDLE;
            commandBuffers_[i]    = VK_NULL_HANDLE;
            queryPools_[i]        = VK_NULL_HANDLE;
        }
        commandPool_ = VK_NULL_HANDLE;
    }

    // 3. Destroy swapchain image views and the swapchain when a valid device exists.
    destroySwapchain();

    // 4. Destroy the logical device.
    if (device_ != VK_NULL_HANDLE) {
#if defined(FFXM_VK_CONTEXT_TESTING)
        if (testSeam_) {
            testSeam_->deviceDestructions++;
        } else
#endif
        {
            vkDestroyDevice(device_, nullptr);
        }
        device_ = VK_NULL_HANDLE;
    }
    physicalDevice_ = VK_NULL_HANDLE;
    queue_          = VK_NULL_HANDLE;
    queueFamily_    = 0;

    // 5. Destroy the surface when both surface and instance are valid.
    if (instance_ != VK_NULL_HANDLE && surface_ != VK_NULL_HANDLE) {
#if defined(FFXM_VK_CONTEXT_TESTING)
        if (testSeam_) {
            testSeam_->surfaceDestructions++;
        } else
#endif
        {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
        }
        surface_ = VK_NULL_HANDLE;
    } else {
        surface_ = VK_NULL_HANDLE;
    }

    // 6. Destroy the instance.
    if (instance_ != VK_NULL_HANDLE) {
#if defined(FFXM_VK_CONTEXT_TESTING)
        if (testSeam_) {
            testSeam_->instanceDestructions++;
        } else
#endif
        {
            vkDestroyInstance(instance_, nullptr);
        }
        instance_ = VK_NULL_HANDLE;
    }

    // 7. Reset capability and synchronization state.
    caps_ = DeviceCapabilities{};
    timestampValidBits_ = 0;
    timestampPeriod_    = 0.0f;
    deviceName_.clear();
    driverVersion_      = 0;
    apiVersion_         = 0;
    sync_.reset();
}

VkResult VkContext::getQueryPoolResults(uint32_t frameSlot, uint32_t firstQuery, uint32_t queryCount,
                                        size_t dataSize, void* data, VkDeviceSize stride,
                                        VkQueryResultFlags flags) const
{
    if (frameSlot >= kFramesInFlight || queryPools_[frameSlot] == VK_NULL_HANDLE || device_ == VK_NULL_HANDLE) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
#if defined(FFXM_VK_CONTEXT_TESTING)
    if (testSeam_) {
        return VK_SUCCESS;
    }
#endif
    return vkGetQueryPoolResults(device_, queryPools_[frameSlot], firstQuery, queryCount, dataSize, data, stride, flags);
}

void VkContext::cmdResetQueryPool(VkCommandBuffer cmd, uint32_t frameSlot, uint32_t firstQuery, uint32_t queryCount) const
{
    if (frameSlot < kFramesInFlight && queryPools_[frameSlot] != VK_NULL_HANDLE && cmd != VK_NULL_HANDLE) {
#if defined(FFXM_VK_CONTEXT_TESTING)
        if (testSeam_) {
            return;
        }
#endif
        vkCmdResetQueryPool(cmd, queryPools_[frameSlot], firstQuery, queryCount);
    }
}
