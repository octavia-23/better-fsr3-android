// Copyright © 2026 Arm Limited.
// SPDX-License-Identifier: MIT

#include "fsr3_upscaler.h"

#include <cstring>

using namespace arm;

namespace {

// FSR 3.1.4 takes resources as FfxmResource, which pairs the API handle with a
// description the backend uses to derive views and barriers.
FfxmResourceDescription describeTexture(const RenderTarget& rt, FfxmSurfaceFormat format,
                                        FfxmResourceUsage usage,
                                        FfxmResourceFlags flags = FFXM_RESOURCE_FLAGS_NONE)
{
    FfxmResourceDescription desc = {};
    desc.type     = FFXM_RESOURCE_TYPE_TEXTURE2D;
    desc.format   = format;
    desc.width    = rt.extent.width;
    desc.height   = rt.extent.height;
    desc.depth    = 1;
    desc.mipCount = 1;
    desc.flags    = flags;
    desc.usage    = usage;
    return desc;
}

FfxmResource wrap(const RenderTarget& rt, FfxmSurfaceFormat format, FfxmResourceUsage usage,
                  FfxmResourceStates state, wchar_t* name,
                  FfxmResourceFlags flags = FFXM_RESOURCE_FLAGS_NONE)
{
    return ffxmGetResourceVK(reinterpret_cast<void*>(rt.image),
                             describeTexture(rt, format, usage, flags), name, state);
}

void messageCallback(FfxmMsgType type, const wchar_t* message)
{
    char buffer[512];
    size_t i = 0;
    for (; message && message[i] != L'\0' && i < sizeof(buffer) - 1; ++i)
        buffer[i] = (message[i] > 0 && message[i] < 128) ? (char)message[i] : '?';
    buffer[i] = '\0';

    if (type == FFXM_MESSAGE_TYPE_ERROR)
        LOGE("FSR 3.1.4: %s", buffer);
    else
        LOGW("FSR 3.1.4: %s", buffer);
}

} // namespace

uint32_t Fsr3Upscaler::initializationFlags()
{
    // The scene renders motion vectors at render resolution, without jitter
    // baked in, and writes a conventional (non-inverted) depth buffer.
    // Auto-exposure handles luminance scaling; debug checking validates parameters.
    return FFXM_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE | FFXM_FSR3UPSCALER_ENABLE_DEBUG_CHECKING;
}

bool Fsr3Upscaler::createTarget(VkContext& ctx, RenderTarget& rt, VkFormat format, VkExtent2D extent,
                                VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    rt = {};
    rt.format = format;
    rt.extent = extent;

    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType     = VK_IMAGE_TYPE_2D;
    ci.format        = format;
    ci.extent        = {extent.width, extent.height, 1};
    ci.mipLevels     = 1;
    ci.arrayLayers   = 1;
    ci.samples       = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ci.usage         = usage;
    ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(ctx.device(), &ci, nullptr, &rt.image) != VK_SUCCESS) {
        rt = {};
        return false;
    }

    VkMemoryRequirements req = {};
    vkGetImageMemoryRequirements(ctx.device(), rt.image, &req);

    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize  = req.size;
    alloc.memoryTypeIndex = ctx.findMemoryType(req.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX) {
        LOGE("No device-local memory type for shared resource");
        vkDestroyImage(ctx.device(), rt.image, nullptr);
        rt = {};
        return false;
    }
    if (vkAllocateMemory(ctx.device(), &alloc, nullptr, &rt.memory) != VK_SUCCESS) {
        vkDestroyImage(ctx.device(), rt.image, nullptr);
        rt = {};
        return false;
    }
    if (vkBindImageMemory(ctx.device(), rt.image, rt.memory, 0) != VK_SUCCESS) {
        vkDestroyImage(ctx.device(), rt.image, nullptr);
        vkFreeMemory(ctx.device(), rt.memory, nullptr);
        rt = {};
        return false;
    }

    VkImageViewCreateInfo viewCi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewCi.image                       = rt.image;
    viewCi.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
    viewCi.format                      = format;
    viewCi.subresourceRange.aspectMask = aspect;
    viewCi.subresourceRange.levelCount = 1;
    viewCi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(ctx.device(), &viewCi, nullptr, &rt.view) != VK_SUCCESS) {
        vkDestroyImage(ctx.device(), rt.image, nullptr);
        vkFreeMemory(ctx.device(), rt.memory, nullptr);
        rt = {};
        return false;
    }
    return true;
}

bool Fsr3Upscaler::createSharedResources(VkContext& ctx)
{
    FfxmFsr3UpscalerSharedResourceDescriptions sharedDescs = {};
    FfxmErrorCode err = ffxmFsr3UpscalerGetSharedResourceDescriptions(&context_, &sharedDescs);
    if (err != FFXM_OK) {
        LOGE("ffxmFsr3UpscalerGetSharedResourceDescriptions failed: 0x%08X", (unsigned)err);
        return false;
    }

    const VkImageUsageFlags uavUsage = VK_IMAGE_USAGE_STORAGE_BIT |
                                       VK_IMAGE_USAGE_SAMPLED_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    // 1. Dilated depth: R32_FLOAT
    VkExtent2D dilatedDepthExtent = {
        sharedDescs.dilatedDepth.resourceDescription.width,
        sharedDescs.dilatedDepth.resourceDescription.height
    };
    if (!createTarget(ctx, dilatedDepth_, VK_FORMAT_R32_SFLOAT, dilatedDepthExtent,
                      uavUsage, VK_IMAGE_ASPECT_COLOR_BIT)) {
        LOGE("Failed to allocate dilated depth shared resource");
        destroySharedResources(ctx);
        return false;
    }

    // 2. Dilated motion vectors: R16G16_FLOAT
    VkExtent2D dilatedMvExtent = {
        sharedDescs.dilatedMotionVectors.resourceDescription.width,
        sharedDescs.dilatedMotionVectors.resourceDescription.height
    };
    if (!createTarget(ctx, dilatedMotionVectors_, VK_FORMAT_R16G16_SFLOAT, dilatedMvExtent,
                      uavUsage, VK_IMAGE_ASPECT_COLOR_BIT)) {
        LOGE("Failed to allocate dilated motion vectors shared resource");
        destroySharedResources(ctx);
        return false;
    }

    // 3. Reconstructed previous nearest depth: R32_UINT
    VkExtent2D reconDepthExtent = {
        sharedDescs.reconstructedPrevNearestDepth.resourceDescription.width,
        sharedDescs.reconstructedPrevNearestDepth.resourceDescription.height
    };
    if (!createTarget(ctx, reconstructedPrevNearestDepth_, VK_FORMAT_R32_UINT, reconDepthExtent,
                      uavUsage, VK_IMAGE_ASPECT_COLOR_BIT)) {
        LOGE("Failed to allocate reconstructed prev nearest depth shared resource");
        destroySharedResources(ctx);
        return false;
    }

    LOGI("FSR 3.1.4 shared resources allocated: dilatedDepth (%ux%u R32_SFLOAT), "
         "dilatedMV (%ux%u R16G16_SFLOAT), reconDepth (%ux%u R32_UINT)",
         dilatedDepthExtent.width, dilatedDepthExtent.height,
         dilatedMvExtent.width, dilatedMvExtent.height,
         reconDepthExtent.width, reconDepthExtent.height);
    sharedResourcesInitialized_ = false;
    return true;
}

void Fsr3Upscaler::destroySharedResources(VkContext& ctx)
{
    if (ctx.device() == VK_NULL_HANDLE) {
        dilatedDepth_ = {};
        dilatedMotionVectors_ = {};
        reconstructedPrevNearestDepth_ = {};
        sharedResourcesInitialized_ = false;
        return;
    }

    auto destroyTarget = [&](RenderTarget& rt) {
        if (rt.view != VK_NULL_HANDLE) {
            vkDestroyImageView(ctx.device(), rt.view, nullptr);
            rt.view = VK_NULL_HANDLE;
        }
        if (rt.image != VK_NULL_HANDLE) {
            vkDestroyImage(ctx.device(), rt.image, nullptr);
            rt.image = VK_NULL_HANDLE;
        }
        if (rt.memory != VK_NULL_HANDLE) {
            vkFreeMemory(ctx.device(), rt.memory, nullptr);
            rt.memory = VK_NULL_HANDLE;
        }
        rt = {};
    };
    destroyTarget(dilatedDepth_);
    destroyTarget(dilatedMotionVectors_);
    destroyTarget(reconstructedPrevNearestDepth_);
    sharedResourcesInitialized_ = false;
}

bool Fsr3Upscaler::init(VkContext& ctx, VkExtent2D renderExtent, VkExtent2D displayExtent,
                        FfxmFsr3UpscalerQualityMode quality)
{
    (void)quality;
    device_        = ctx.device();
    pVkCtx_        = &ctx;
    renderExtent_  = renderExtent;
    displayExtent_ = displayExtent;

    // 1. Scratch memory.
    const size_t maxContexts = FFXM_FSR3UPSCALER_CONTEXT_COUNT;
    const size_t scratchSize = ffxmGetScratchMemorySizeVK(ctx.physicalDevice(), maxContexts);
    scratchBuffer_.resize(scratchSize);
    LOGI("FSR 3.1.4 scratch buffer: %zu bytes", scratchSize);

    // 2. Backend device + interface.
    VkDeviceContext deviceContext = {};
    deviceContext.vkDevice         = ctx.device();
    deviceContext.vkPhysicalDevice = ctx.physicalDevice();
    deviceContext.vkDeviceProcAddr = vkGetDeviceProcAddr;

    FfxmDevice ffxmDevice = ffxmGetDeviceVK(&deviceContext);

    FfxmErrorCode err = ffxmGetInterfaceVK(&interface_, ffxmDevice, scratchBuffer_.data(),
                                           scratchBuffer_.size(), maxContexts);
    if (err != FFXM_OK) {
        LOGE("ffxmGetInterfaceVK failed: 0x%08X", (unsigned)err);
        return false;
    }

    // 3. FSR3 context.
    FfxmFsr3UpscalerContextDescription contextDesc = {};
    contextDesc.flags            = initializationFlags();
    contextDesc.maxRenderSize    = {renderExtent.width, renderExtent.height};
    contextDesc.maxUpscaleSize   = {displayExtent.width, displayExtent.height};
    contextDesc.backendInterface = interface_;
    contextDesc.fpMessage        = messageCallback;

    err = ffxmFsr3UpscalerContextCreate(&context_, &contextDesc);
    if (err != FFXM_OK) {
        LOGE("ffxmFsr3UpscalerContextCreate failed: 0x%08X", (unsigned)err);
        return false;
    }
    contextCreated_ = true;

    // 4. Allocate shared resources queried from public description API
    if (!createSharedResources(ctx)) {
        destroy();
        return false;
    }

    jitterPhaseCount_ = ffxmFsr3UpscalerGetJitterPhaseCount((int32_t)renderExtent.width,
                                                            (int32_t)displayExtent.width);
    LOGI("FSR 3.1.4 context created: %ux%u -> %ux%u, jitter phase count %d",
         renderExtent.width, renderExtent.height, displayExtent.width, displayExtent.height,
         jitterPhaseCount_);
    return true;
}

void Fsr3Upscaler::jitterForFrame(int32_t frameIndex, float* outX, float* outY) const
{
    *outX = 0.0f;
    *outY = 0.0f;
    if (jitterPhaseCount_ > 0)
        ffxmFsr3UpscalerGetJitterOffset(outX, outY, frameIndex % jitterPhaseCount_, jitterPhaseCount_);
}

bool Fsr3Upscaler::dispatch(VkCommandBuffer cmd, const Scene& scene, VkExtent2D renderExtent,
                            float frameTimeMs, int32_t frameIndex, bool reset)
{
    if (!contextCreated_)
        return false;

    float jitterX = 0.0f, jitterY = 0.0f;
    jitterForFrame(frameIndex, &jitterX, &jitterY);

    static wchar_t kColorName[]               = L"FSR3_Color";
    static wchar_t kDepthName[]               = L"FSR3_Depth";
    static wchar_t kMotionName[]              = L"FSR3_MotionVectors";
    static wchar_t kDilatedDepthName[]        = L"FSR3_DilatedDepth";
    static wchar_t kDilatedMotionVectorsName[] = L"FSR3_DilatedVelocity";
    static wchar_t kReconDepthName[]          = L"FSR3_ReconstructedPrevNearestDepth";
    static wchar_t kOutputName[]              = L"FSR3_Output";

    FfxmFsr3UpscalerDispatchDescription desc = {};
    desc.commandList = ffxmGetCommandListVK(cmd);

    desc.color = wrap(scene.color(), FFXM_SURFACE_FORMAT_R16G16B16A16_FLOAT,
                      FFXM_RESOURCE_USAGE_RENDERTARGET, FFXM_RESOURCE_STATE_COMPUTE_READ,
                      kColorName);

    desc.depth = wrap(scene.depth(), FFXM_SURFACE_FORMAT_R32_FLOAT,
                      FFXM_RESOURCE_USAGE_DEPTHTARGET, FFXM_RESOURCE_STATE_COMPUTE_READ,
                      kDepthName);

    desc.motionVectors = wrap(scene.motion(), FFXM_SURFACE_FORMAT_R16G16_FLOAT,
                              FFXM_RESOURCE_USAGE_RENDERTARGET, FFXM_RESOURCE_STATE_COMPUTE_READ,
                              kMotionName);

    const FfxmResourceFlags sharedFlags = !sharedResourcesInitialized_
        ? FFXM_RESOURCE_FLAGS_UNDEFINED
        : FFXM_RESOURCE_FLAGS_NONE;

    desc.dilatedDepth = wrap(dilatedDepth_, FFXM_SURFACE_FORMAT_R32_FLOAT,
                             static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
                             FFXM_RESOURCE_STATE_UNORDERED_ACCESS, kDilatedDepthName, sharedFlags);

    desc.dilatedMotionVectors = wrap(dilatedMotionVectors_, FFXM_SURFACE_FORMAT_R16G16_FLOAT,
                                     static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
                                     FFXM_RESOURCE_STATE_UNORDERED_ACCESS, kDilatedMotionVectorsName, sharedFlags);

    desc.reconstructedPrevNearestDepth = wrap(reconstructedPrevNearestDepth_, FFXM_SURFACE_FORMAT_R32_UINT,
                                              FFXM_RESOURCE_USAGE_UAV, FFXM_RESOURCE_STATE_UNORDERED_ACCESS,
                                              kReconDepthName, sharedFlags);

    desc.output = wrap(scene.output(), FFXM_SURFACE_FORMAT_R16G16B16A16_FLOAT,
                       FFXM_RESOURCE_USAGE_UAV, FFXM_RESOURCE_STATE_UNORDERED_ACCESS,
                       kOutputName);

    desc.jitterOffset               = {jitterX, jitterY};
    desc.motionVectorScale          = {1.0f, 1.0f};
    desc.renderSize                 = {renderExtent.width, renderExtent.height};
    desc.upscaleSize                = {displayExtent_.width, displayExtent_.height};
    desc.enableSharpening           = false; // Baseline compute passes; unsharpened Accumulate
    desc.sharpness                  = 0.0f;
    desc.frameTimeDelta             = frameTimeMs;
    desc.preExposure                = 1.0f;
    desc.reset                      = reset;
    desc.cameraNear                 = 0.1f;
    desc.cameraFar                  = 200.0f;
    desc.cameraFovAngleVertical     = 1.0471975512f;  // 60 degrees in radians
    desc.viewSpaceToMetersFactor    = 1.0f;
    desc.flags                      = 0;

    FfxmErrorCode err = ffxmFsr3UpscalerContextDispatch(&context_, &desc);
    if (err != FFXM_OK) {
        LOGE("ffxmFsr3UpscalerContextDispatch failed: 0x%08X", (unsigned)err);
        return false;
    }
    sharedResourcesInitialized_ = true;
    return true;
}

void Fsr3Upscaler::destroy()
{
    if (contextCreated_) {
        if (device_ != VK_NULL_HANDLE)
            vkDeviceWaitIdle(device_);
        if (pVkCtx_)
            destroySharedResources(*pVkCtx_);
        ffxmFsr3UpscalerContextDestroy(&context_);
        contextCreated_ = false;
    }
    scratchBuffer_.clear();
    pVkCtx_ = nullptr;
    device_ = VK_NULL_HANDLE;
}
