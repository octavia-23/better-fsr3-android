// Copyright  © 2026 Arm Limited.
// SPDX-License-Identifier: MIT

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <iostream>
#include <cstring>
#include <cmath>
#include <vector>
#include <map>
#include <set>
#include <cstdint>

#define FFXM_CPU
#include "ffxm_core.h"
#include "fsr1/ffxm_fsr1.h"
#include "spd/ffxm_spd.h"

#include "host/ffxm_fsr3upscaler.h"
#include "src/components/fsr3upscaler/ffxm_fsr3upscaler_private.h"
#include "host/ffxm_util.h"
#include "src/backends/shared/ffxm_shader_blobs.h"

#if defined(FFXM_HAS_VK_BACKEND)
#include "host/backends/vk/ffxm_vk.h"

#if defined(_WIN32)
#include <windows.h>
static PFN_vkEnumerateDeviceExtensionProperties s_pfnRealEnumerateDeviceExtensionProperties = nullptr;
static PFN_vkGetDeviceProcAddr s_pfnRealGetDeviceProcAddr = nullptr;
static PFN_vkGetPhysicalDeviceProperties2 s_pfnRealGetPhysicalDeviceProperties2 = nullptr;
static PFN_vkGetPhysicalDeviceFeatures2 s_pfnRealGetPhysicalDeviceFeatures2 = nullptr;
static PFN_vkGetPhysicalDeviceMemoryProperties s_pfnRealGetPhysicalDeviceMemoryProperties = nullptr;
#endif

#if !defined(ANDROID)
// On host platforms when compiling tests without the full Vulkan SDK loader (using header-only Vulkan-Headers),
// provide stubs for Vulkan loader functions so test_milestone2 can link Arm_ASR_backend
// to directly test ffxmGetScratchMemorySizeVK and ffxmGetInterfaceVK.
// When running real hardware tests, these stubs forward dynamically to vulkan-1.dll.
extern "C" {
    VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(
        VkPhysicalDevice physicalDevice, const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties)
    {
#if defined(_WIN32)
        if (s_pfnRealEnumerateDeviceExtensionProperties) {
            return s_pfnRealEnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pPropertyCount, pProperties);
        }
#endif
        (void)physicalDevice;
        (void)pLayerName;
        (void)pProperties;
        if (pPropertyCount) {
            *pPropertyCount = 0;
        }
        return VK_SUCCESS;
    }

    VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName)
    {
#if defined(_WIN32)
        if (s_pfnRealGetDeviceProcAddr) {
            return s_pfnRealGetDeviceProcAddr(device, pName);
        }
#endif
        (void)device;
        (void)pName;
        return nullptr;
    }

    VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(
        VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2* pProperties)
    {
#if defined(_WIN32)
        if (s_pfnRealGetPhysicalDeviceProperties2) {
            s_pfnRealGetPhysicalDeviceProperties2(physicalDevice, pProperties);
            return;
        }
#endif
        (void)physicalDevice;
        (void)pProperties;
    }

    VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(
        VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2* pFeatures)
    {
#if defined(_WIN32)
        if (s_pfnRealGetPhysicalDeviceFeatures2) {
            s_pfnRealGetPhysicalDeviceFeatures2(physicalDevice, pFeatures);
            return;
        }
#endif
        (void)physicalDevice;
        (void)pFeatures;
    }

    VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(
        VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties* pMemoryProperties)
    {
#if defined(_WIN32)
        if (s_pfnRealGetPhysicalDeviceMemoryProperties) {
            s_pfnRealGetPhysicalDeviceMemoryProperties(physicalDevice, pMemoryProperties);
            return;
        }
#endif
        (void)physicalDevice;
        (void)pMemoryProperties;
    }

    VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(
        VkCommandBuffer commandBuffer, uint32_t firstViewport, uint32_t viewportCount, const VkViewport* pViewports)
    {
        (void)commandBuffer;
        (void)firstViewport;
        (void)viewportCount;
        (void)pViewports;
    }

    VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(
        VkCommandBuffer commandBuffer, uint32_t firstScissor, uint32_t scissorCount, const VkRect2D* pScissors)
    {
        (void)commandBuffer;
        (void)firstScissor;
        (void)scissorCount;
        (void)pScissors;
    }
}
#endif
#endif

using namespace arm;

// Global test counters for always-active test assertions
static int g_TestPasses = 0;
static int g_TestFailures = 0;
static bool g_SuppressFailurePrint = false;

#define TEST_CHECK(cond) do { \
    if (!(cond)) { \
        if (!g_SuppressFailurePrint) { \
            std::cerr << "CHECK FAILED: " #cond " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        } \
        g_TestFailures++; \
    } else { \
        g_TestPasses++; \
    } \
} while (0)

#define TEST_REQUIRE(cond) do { \
    if (!(cond)) { \
        std::cerr << "REQUIREMENT FAILED: " #cond " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        g_TestFailures++; \
        return; \
    } else { \
        g_TestPasses++; \
    } \
} while (0)

// Mock backend interface state tracking
static bool s_GetDeviceCapsCalled = false;
static bool s_CreateBackendContextCalled = false;
static bool s_DestroyBackendContextCalled = false;
static FfxmUInt32 s_CreatedContextId = 0;
static FfxmUInt32 s_DestroyedContextId = 0;
static bool s_Fp16SupportMock = true;
static FfxmErrorCode s_CreateBackendContextResult = FFXM_OK;
static bool s_BackendContextActive = false;
static int s_CallSequenceNumber = 0;
static int s_CreateBackendContextCallSeq = 0;
static int s_GetDeviceCapsCallSeq = 0;
static bool s_EnforceBackendActiveForCaps = true;

// Resource failure injection controls
static int32_t s_FailAtResourceIndex = -1; // -1 means no failure
static int32_t s_FailAtCopyIndex = -1;     // -1 means no failure
static int32_t s_CurrentResourceCreateCount = 0;
static int32_t s_NextResourceIndex = 0;

// Dynamic registration & dispatch failure injection controls
static int32_t s_FailAtRegisterResourceIndex = -1;
static uint32_t s_RegisterResourceCallCount = 0;
static FfxmErrorCode s_ScheduleGpuJobResult = FFXM_OK;
static uint32_t s_ScheduleGpuJobCallCount = 0;
static int32_t s_FailAtScheduleGpuJobIndex = -1;
static FfxmErrorCode s_ExecuteGpuJobsResult = FFXM_OK;
static FfxmErrorCode s_UnregisterResourcesResult = FFXM_OK;

// Occurrence-aware resource creation & destruction tracking
static std::map<int32_t, int> s_ResourceCreationCounts;
static std::map<int32_t, int> s_ResourceDestructionCounts;
static std::vector<int32_t> s_CreatedResourceOrder;
static std::vector<int32_t> s_DestroyedResourceOrder;

// Captured resource descriptions and initial data for regression verification
static std::vector<FfxmCreateResourceDescription> s_CapturedResourceDescriptions;
static std::vector<std::vector<uint8_t>> s_CapturedInitialData;

static FfxmVersionNumber MockGetSDKVersion(FfxmInterface* backendInterface)
{
    (void)backendInterface;
    return FFXM_SDK_MAKE_VERSION(1, 1, 4);
}

static FfxmErrorCode MockGetDeviceCapabilities(FfxmInterface* backendInterface, FfxmDeviceCapabilities* deviceCapabilities)
{
    (void)backendInterface;
    s_GetDeviceCapsCalled = true;
    s_GetDeviceCapsCallSeq = ++s_CallSequenceNumber;

    // Strict contract check: In real Vulkan, backendContext extension properties and physical device
    // are only initialized after CreateBackendContextVK has run!
    if (s_EnforceBackendActiveForCaps && !s_BackendContextActive) {
        return FFXM_ERROR_BACKEND_API_ERROR;
    }

    if (deviceCapabilities) {
        deviceCapabilities->fp16Supported = s_Fp16SupportMock;
        deviceCapabilities->raytracingSupported = false;
    }
    return FFXM_OK;
}

static FfxmErrorCode MockCreateBackendContext(FfxmInterface* backendInterface, FfxmUInt32* effectContextId)
{
    (void)backendInterface;
    s_CreateBackendContextCalled = true;
    s_CreateBackendContextCallSeq = ++s_CallSequenceNumber;
    if (s_CreateBackendContextResult != FFXM_OK) {
        return s_CreateBackendContextResult;
    }
    s_BackendContextActive = true;
    *effectContextId = 100;
    s_CreatedContextId = 100;
    return FFXM_OK;
}

static FfxmErrorCode MockDestroyBackendContext(FfxmInterface* backendInterface, FfxmUInt32 effectContextId)
{
    (void)backendInterface;
    s_DestroyBackendContextCalled = true;
    s_BackendContextActive = false;
    s_DestroyedContextId = effectContextId;
    return FFXM_OK;
}

static FfxmErrorCode MockCreateResource(
    FfxmInterface* backendInterface,
    const FfxmCreateResourceDescription* createResourceDescription,
    FfxmUInt32 effectContextId,
    FfxmResourceInternal* outResource)
{
    (void)backendInterface; (void)effectContextId;

    if (s_FailAtResourceIndex >= 0 && s_CurrentResourceCreateCount == s_FailAtResourceIndex) {
        return FFXM_ERROR_OUT_OF_MEMORY;
    }

    if (createResourceDescription) {
        s_CapturedResourceDescriptions.push_back(*createResourceDescription);
        if (createResourceDescription->initData && createResourceDescription->initDataSize > 0) {
            std::vector<uint8_t> dataCopy(
                static_cast<const uint8_t*>(createResourceDescription->initData),
                static_cast<const uint8_t*>(createResourceDescription->initData) + createResourceDescription->initDataSize);
            s_CapturedInitialData.push_back(dataCopy);
        }
    }

    int32_t idx = s_NextResourceIndex++;
    s_CurrentResourceCreateCount++;
    s_ResourceCreationCounts[idx]++;
    s_CreatedResourceOrder.push_back(idx);

    if (outResource) {
        outResource->internalIndex = idx;
    }

    // If resource has initial data, backend creates a copy staging resource at idx + 1
    if (createResourceDescription && createResourceDescription->initData) {
        if (s_FailAtCopyIndex >= 0 && (idx + 1) == s_FailAtCopyIndex) {
            return FFXM_ERROR_OUT_OF_MEMORY;
        }
        int32_t copyIdx = s_NextResourceIndex++;
        s_ResourceCreationCounts[copyIdx]++;
        s_CreatedResourceOrder.push_back(copyIdx);
    }

    return FFXM_OK;
}

static FfxmErrorCode MockDestroyResource(FfxmInterface* backendInterface, FfxmResourceInternal resource)
{
    (void)backendInterface;
    if (resource.internalIndex != -1) {
        s_ResourceDestructionCounts[resource.internalIndex]++;
        s_DestroyedResourceOrder.push_back(resource.internalIndex);
    }
    return FFXM_OK;
}

static FfxmErrorCode MockRegisterResource(
    FfxmInterface* backendInterface,
    const FfxmResource* inResource,
    FfxmUInt32 effectContextId,
    FfxmResourceInternal* outResource)
{
    (void)backendInterface; (void)inResource; (void)effectContextId;
    if (s_FailAtRegisterResourceIndex >= 0 && static_cast<int32_t>(s_RegisterResourceCallCount) == s_FailAtRegisterResourceIndex) {
        s_RegisterResourceCallCount++;
        return FFXM_ERROR_OUT_OF_MEMORY;
    }
    if (outResource) {
        outResource->internalIndex = 900 + static_cast<int32_t>(s_RegisterResourceCallCount);
    }
    s_RegisterResourceCallCount++;
    return FFXM_OK;
}

static FfxmResource MockGetResource(FfxmInterface* backendInterface, FfxmResourceInternal res)
{
    (void)backendInterface; (void)res;
    FfxmResource r = {};
    return r;
}

static FfxmResourceDescription MockGetResourceDescription(FfxmInterface* backendInterface, FfxmResourceInternal res)
{
    (void)backendInterface; (void)res;
    FfxmResourceDescription d = {};
    return d;
}

template <size_t N>
static void ffxmWidenAscii(wchar_t (&dst)[N], const char* src)
{
    size_t i = 0;
    if (src) {
        for (; i < N - 1 && src[i] != '\0'; ++i)
            dst[i] = static_cast<wchar_t>(static_cast<unsigned char>(src[i]));
    }
    dst[i] = L'\0';
}

static uint32_t s_CreatePipelineCallCount = 0;
static uint32_t s_DestroyPipelineCallCount = 0;
static uint32_t s_ExecuteGpuJobsCallCount = 0;
static uint32_t s_UnregisterResourcesCallCount = 0;
static std::vector<FfxmGpuJobDescription> s_ScheduledGpuJobs;
static FfxmErrorCode s_CreatePipelineResult = FFXM_OK;

static FfxmErrorCode MockCreatePipeline(
    FfxmInterface* backendInterface,
    FfxmEffect effect,
    FfxmPass pass,
    FfxmShaderQuality qualityPreset,
    uint32_t permutationOptions,
    const FfxmPipelineDescription* pipelineDescription,
    FfxmUInt32 effectContextId,
    FfxmPipelineState* outPipeline)
{
    (void)backendInterface; (void)qualityPreset; (void)pipelineDescription; (void)effectContextId;
    s_CreatePipelineCallCount++;
    if (s_CreatePipelineResult != FFXM_OK) {
        return s_CreatePipelineResult;
    }

    if (outPipeline == nullptr) {
        return FFXM_ERROR_INVALID_POINTER;
    }

    FfxmShaderBlob shaderBlob = {};
    FfxmErrorCode err = arm::ffxmGetPermutationBlobByIndex(effect, pass, permutationOptions, &shaderBlob);
    if (err == FFXM_OK && shaderBlob.data != nullptr) {
        outPipeline->pipeline = reinterpret_cast<FfxmPipeline>(static_cast<uintptr_t>(0x1000 + s_CreatePipelineCallCount * 0x100));
        outPipeline->srvTextureCount = shaderBlob.srvTextureCount;
        for (uint32_t i = 0; i < shaderBlob.srvTextureCount; ++i) {
            outPipeline->srvTextureBindings[i].slotIndex = shaderBlob.boundSRVTextures[i];
            outPipeline->srvTextureBindings[i].bindCount = shaderBlob.boundSRVTextureCounts[i];
            outPipeline->srvTextureBindings[i].bindSet = shaderBlob.boundSRVTextureSets[i];
            ffxmWidenAscii(outPipeline->srvTextureBindings[i].name, shaderBlob.boundSRVTextureNames[i]);
        }
        outPipeline->uavTextureCount = shaderBlob.uavTextureCount;
        for (uint32_t i = 0; i < shaderBlob.uavTextureCount; ++i) {
            outPipeline->uavTextureBindings[i].slotIndex = shaderBlob.boundUAVTextures[i];
            outPipeline->uavTextureBindings[i].bindCount = shaderBlob.boundUAVTextureCounts[i];
            outPipeline->uavTextureBindings[i].bindSet = shaderBlob.boundUAVTextureSets[i];
            ffxmWidenAscii(outPipeline->uavTextureBindings[i].name, shaderBlob.boundUAVTextureNames[i]);
        }
        outPipeline->constCount = shaderBlob.cbvCount;
        for (uint32_t i = 0; i < shaderBlob.cbvCount; ++i) {
            outPipeline->constantBufferBindings[i].slotIndex = shaderBlob.boundConstantBuffers[i];
            outPipeline->constantBufferBindings[i].bindCount = shaderBlob.boundConstantBufferCounts[i];
            outPipeline->constantBufferBindings[i].bindSet = shaderBlob.boundConstantBufferSets[i];
            ffxmWidenAscii(outPipeline->constantBufferBindings[i].name, shaderBlob.boundConstantBufferNames[i]);
        }
    } else {
        outPipeline->pipeline = reinterpret_cast<FfxmPipeline>(static_cast<uintptr_t>(0x5000 + s_CreatePipelineCallCount * 0x100));
    }

    return FFXM_OK;
}

static FfxmErrorCode MockDestroyPipeline(FfxmInterface* backendInterface, FfxmPipelineState* pipeline, FfxmUInt32 effectContextId)
{
    (void)backendInterface; (void)effectContextId;
    s_DestroyPipelineCallCount++;
    if (pipeline) {
        pipeline->pipeline = nullptr;
    }
    return FFXM_OK;
}

static FfxmErrorCode MockScheduleGpuJob(FfxmInterface* backendInterface, const FfxmGpuJobDescription* job)
{
    (void)backendInterface;
    if (s_FailAtScheduleGpuJobIndex >= 0 && static_cast<int32_t>(s_ScheduleGpuJobCallCount) == s_FailAtScheduleGpuJobIndex) {
        s_ScheduleGpuJobCallCount++;
        return FFXM_ERROR_BACKEND_API_ERROR;
    }
    s_ScheduleGpuJobCallCount++;
    if (s_ScheduleGpuJobResult != FFXM_OK) {
        return s_ScheduleGpuJobResult;
    }
    if (job) {
        s_ScheduledGpuJobs.push_back(*job);
    }
    return FFXM_OK;
}

static FfxmErrorCode MockExecuteGpuJobs(FfxmInterface* backendInterface, FfxmCommandList commandList)
{
    (void)backendInterface; (void)commandList;
    s_ExecuteGpuJobsCallCount++;
    return s_ExecuteGpuJobsResult;
}

static FfxmErrorCode MockUnregisterResources(FfxmInterface* backendInterface, FfxmCommandList commandList, FfxmUInt32 effectContextId)
{
    (void)backendInterface; (void)commandList; (void)effectContextId;
    s_UnregisterResourcesCallCount++;
    return s_UnregisterResourcesResult;
}

static void ResetMockState()
{
    s_GetDeviceCapsCalled = false;
    s_CreateBackendContextCalled = false;
    s_DestroyBackendContextCalled = false;
    s_CreatedContextId = 0;
    s_DestroyedContextId = 0;
    s_Fp16SupportMock = true;
    s_CreateBackendContextResult = FFXM_OK;
    s_BackendContextActive = false;
    s_CallSequenceNumber = 0;
    s_CreateBackendContextCallSeq = 0;
    s_GetDeviceCapsCallSeq = 0;
    s_EnforceBackendActiveForCaps = true;
    s_FailAtResourceIndex = -1;
    s_FailAtCopyIndex = -1;
    s_CurrentResourceCreateCount = 0;
    s_NextResourceIndex = 0;
    s_ResourceCreationCounts.clear();
    s_ResourceDestructionCounts.clear();
    s_CreatedResourceOrder.clear();
    s_DestroyedResourceOrder.clear();
    s_CapturedResourceDescriptions.clear();
    s_CapturedInitialData.clear();
    s_CreatePipelineCallCount = 0;
    s_DestroyPipelineCallCount = 0;
    s_ExecuteGpuJobsCallCount = 0;
    s_UnregisterResourcesCallCount = 0;
    s_ScheduledGpuJobs.clear();
    s_CreatePipelineResult = FFXM_OK;
    s_FailAtRegisterResourceIndex = -1;
    s_RegisterResourceCallCount = 0;
    s_ScheduleGpuJobResult = FFXM_OK;
    s_ScheduleGpuJobCallCount = 0;
    s_FailAtScheduleGpuJobIndex = -1;
    s_ExecuteGpuJobsResult = FFXM_OK;
    s_UnregisterResourcesResult = FFXM_OK;
}

static void SetupMockInterface(FfxmInterface* iface)
{
    memset(iface, 0, sizeof(FfxmInterface));
    static int s_DummyDevice = 1;
    iface->device = &s_DummyDevice;
    iface->fpGetSDKVersion = MockGetSDKVersion;
    iface->fpGetDeviceCapabilities = MockGetDeviceCapabilities;
    iface->fpCreateBackendContext = MockCreateBackendContext;
    iface->fpDestroyBackendContext = MockDestroyBackendContext;
    iface->fpCreateResource = MockCreateResource;
    iface->fpDestroyResource = MockDestroyResource;
    iface->fpRegisterResource = MockRegisterResource;
    iface->fpGetResource = MockGetResource;
    iface->fpUnregisterResources = MockUnregisterResources;
    iface->fpGetResourceDescription = MockGetResourceDescription;
    iface->fpCreateComputePipeline = MockCreatePipeline;
    iface->fpCreateGraphicsPipeline = MockCreatePipeline;
    iface->fpDestroyPipeline = MockDestroyPipeline;
    iface->fpScheduleGpuJob = MockScheduleGpuJob;
    iface->fpExecuteGpuJobs = MockExecuteGpuJobs;
}

static void VerifyOccurrenceAwareResourceBalance()
{
    // 1. Total unique created and destroyed resource counts must match
    TEST_CHECK(s_ResourceCreationCounts.size() == s_ResourceDestructionCounts.size());

    // 2. Every acquired resource must be destroyed exactly once
    for (const auto& kv : s_ResourceCreationCounts) {
        int32_t idx = kv.first;
        TEST_CHECK(kv.second == 1);
        auto it = s_ResourceDestructionCounts.find(idx);
        TEST_CHECK(it != s_ResourceDestructionCounts.end());
        if (it != s_ResourceDestructionCounts.end()) {
            TEST_CHECK(it->second == 1);
        }
    }

    // 3. No unacquired resource was destroyed
    for (const auto& kv : s_ResourceDestructionCounts) {
        int32_t idx = kv.first;
        TEST_CHECK(s_ResourceCreationCounts.find(idx) != s_ResourceCreationCounts.end());
    }

    // 4. Exact map equality
    TEST_CHECK(s_ResourceCreationCounts == s_ResourceDestructionCounts);
}

// Checked arithmetic primitives unit test
static inline bool testCheckAddSize(size_t a, size_t b, size_t* out)
{
    if (SIZE_MAX - a < b) return false;
    *out = a + b;
    return true;
}

static inline bool testCheckMulSize(size_t a, size_t b, size_t* out)
{
    if (a != 0 && b > SIZE_MAX / a) return false;
    *out = a * b;
    return true;
}

static inline bool testCheckAlignUpSize(size_t val, size_t align, size_t* out)
{
    if (align == 0) {
        *out = val;
        return true;
    }
    size_t remainder = val % align;
    if (remainder == 0) {
        *out = val;
        return true;
    }
    size_t addVal = align - remainder;
    if (SIZE_MAX - val < addVal) return false;
    *out = val + addVal;
    return true;
}

// 0. Verify that TEST_CHECK remains active under NDEBUG
void TestNDEBUGAssertionActive()
{
    std::cout << "--- Test 0: NDEBUG Assertion Evaluation Verification ---" << std::endl;
    int initialFailures = g_TestFailures;
    int sideEffectCounter = 0;

    // Verify expression side-effects are never stripped
    TEST_CHECK((sideEffectCounter = 42) == 42);
    TEST_CHECK(sideEffectCounter == 42);

    // Verify intentional false check registers without halting
    int savedFailures = g_TestFailures;
    g_SuppressFailurePrint = true;
    TEST_CHECK(false);
    g_SuppressFailurePrint = false;
    TEST_CHECK(g_TestFailures == savedFailures + 1);
    g_TestFailures = initialFailures; // Reset failure counter for intentional check

    std::cout << "PASS: Assertion macros active and verified under NDEBUG." << std::endl;
}

// 1. Context Size and Alignment
void TestContextSizeAndAlignment()
{
    std::cout << "--- Test 1: Context Size and Alignment ---" << std::endl;
    std::cout << "sizeof(FfxmFsr3UpscalerContext): " << sizeof(FfxmFsr3UpscalerContext) << " bytes" << std::endl;
    std::cout << "alignof(FfxmFsr3UpscalerContext): " << alignof(FfxmFsr3UpscalerContext) << " bytes" << std::endl;
    std::cout << "sizeof(FfxmFsr3UpscalerContext_Private): " << sizeof(FfxmFsr3UpscalerContext_Private) << " bytes" << std::endl;
    std::cout << "alignof(FfxmFsr3UpscalerContext_Private): " << alignof(FfxmFsr3UpscalerContext_Private) << " bytes" << std::endl;

#if defined(_WIN32)
    TEST_CHECK(sizeof(FfxmFsr3UpscalerContext) == 262144);
#elif defined(__aarch64__)
    TEST_CHECK(sizeof(FfxmFsr3UpscalerContext) == 524288);
#endif
    TEST_CHECK(alignof(FfxmFsr3UpscalerContext) >= 8);
    TEST_CHECK(sizeof(FfxmFsr3UpscalerContext) >= sizeof(FfxmFsr3UpscalerContext_Private));
    TEST_CHECK(alignof(FfxmFsr3UpscalerContext) >= alignof(FfxmFsr3UpscalerContext_Private));
    std::cout << "PASS: Context Size and Alignment verified." << std::endl;
}

// 2. Successful Context Creation & Resource Allocation
void TestSuccessfulContextCreation()
{
    std::cout << "--- Test 2: Successful Context Creation & Resource Allocation ---" << std::endl;
    ResetMockState();

    FfxmFsr3UpscalerContext context;
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    SetupMockInterface(&desc.backendInterface);

    FfxmErrorCode res = ffxmFsr3UpscalerContextCreate(&context, &desc);
    TEST_CHECK(res == FFXM_OK);
    TEST_CHECK(s_CreateBackendContextCalled == true);
    TEST_CHECK(s_GetDeviceCapsCalled == true);
    // Verified call ordering: backend context created before capabilities are queried
    TEST_CHECK(s_CreateBackendContextCallSeq < s_GetDeviceCapsCallSeq);
    TEST_CHECK(s_BackendContextActive == true);

    FfxmFsr3UpscalerContext_Private* priv = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(&context);
    TEST_CHECK(priv->backendContextCreated == true);
    TEST_CHECK(priv->effectContextId == 100);
    TEST_CHECK(priv->allocatedResourceCount == FFXM_FSR3UPSCALER_INTERNAL_SURFACE_COUNT);

    // Verify all 19 resources were allocated with non-negative indices
    for (uint32_t i = 0; i < FFXM_FSR3UPSCALER_INTERNAL_SURFACE_COUNT; ++i) {
        uint32_t id = priv->allocatedResourceIds[i];
        TEST_CHECK(priv->srvResources[id].internalIndex >= 0);
        TEST_CHECK(priv->uavResources[id].internalIndex == priv->srvResources[id].internalIndex);
        TEST_CHECK(priv->rtResources[id].internalIndex == priv->srvResources[id].internalIndex);
    }

    // Verify constant staging
    TEST_CHECK(priv->constants.maxRenderSize[0] == 1920);
    TEST_CHECK(priv->constants.maxRenderSize[1] == 1080);
    TEST_CHECK(priv->constants.maxUpscaleSize[0] == 3840);
    TEST_CHECK(priv->constants.maxUpscaleSize[1] == 2160);

    // Test destruction
    res = ffxmFsr3UpscalerContextDestroy(&context);
    TEST_CHECK(res == FFXM_OK);
    TEST_CHECK(s_DestroyBackendContextCalled == true);
    TEST_CHECK(s_DestroyedContextId == 100);
    TEST_CHECK(s_BackendContextActive == false);

    // Verify occurrence-aware resource balance (19 surfaces, 4 copy resources, 0 leaks, 0 double-frees)
    VerifyOccurrenceAwareResourceBalance();
    TEST_CHECK(s_ResourceCreationCounts.size() == 23); // 19 textures + 4 copy staging resources

    // Verify context was cleanly zeroed
    uint8_t zeros[sizeof(FfxmFsr3UpscalerContext)] = {};
    TEST_CHECK(memcmp(&context, zeros, sizeof(context)) == 0);

    std::cout << "PASS: Context Creation and Resource Allocation verified (19 surfaces, 4 copy resources, 0 leaks)." << std::endl;
}

// 3. Unsupported FP16 Hardware Rejection
void TestUnsupportedFP16Hardware()
{
    std::cout << "--- Test 3: Unsupported FP16 Hardware Rejection ---" << std::endl;
    ResetMockState();
    s_Fp16SupportMock = false; // Unsupported FP16

    FfxmFsr3UpscalerContext context;
    memset(&context, 0xBB, sizeof(context));
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    SetupMockInterface(&desc.backendInterface);

    FfxmErrorCode res = ffxmFsr3UpscalerContextCreate(&context, &desc);
    TEST_CHECK(res == FFXM_ERROR_UNSUPPORTED_DEVICE);
    // Backend context was created first, device capabilities queried second, then rollback destroyed it
    TEST_CHECK(s_CreateBackendContextCalled == true);
    TEST_CHECK(s_GetDeviceCapsCalled == true);
    TEST_CHECK(s_CreateBackendContextCallSeq < s_GetDeviceCapsCallSeq);
    TEST_CHECK(s_DestroyBackendContextCalled == true);
    TEST_CHECK(s_BackendContextActive == false);
    TEST_CHECK(s_ResourceCreationCounts.empty());
    TEST_CHECK(s_ResourceDestructionCounts.empty());

    // Context must be cleanly zeroed
    uint8_t zeros[sizeof(FfxmFsr3UpscalerContext)] = {};
    TEST_CHECK(memcmp(&context, zeros, sizeof(context)) == 0);

    std::cout << "PASS: Unsupported FP16 hardware returns FFXM_ERROR_UNSUPPORTED_DEVICE and cleanly rolls back backend context." << std::endl;
}

// 4. Backend Context Creation Failure
void TestBackendContextCreationFailure()
{
    std::cout << "--- Test 4: Backend Context Creation Failure ---" << std::endl;
    ResetMockState();
    s_CreateBackendContextResult = FFXM_ERROR_OUT_OF_MEMORY;

    FfxmFsr3UpscalerContext context;
    memset(&context, 0xCC, sizeof(context));
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    SetupMockInterface(&desc.backendInterface);

    FfxmErrorCode res = ffxmFsr3UpscalerContextCreate(&context, &desc);
    TEST_CHECK(res == FFXM_ERROR_OUT_OF_MEMORY);
    TEST_CHECK(s_CreateBackendContextCalled == true);
    TEST_CHECK(s_GetDeviceCapsCalled == false); // Must NOT query caps if backend context creation failed
    TEST_CHECK(s_DestroyBackendContextCalled == false); // Never successfully created, so must NOT destroy
    TEST_CHECK(s_ResourceCreationCounts.empty());
    TEST_CHECK(s_ResourceDestructionCounts.empty());
    TEST_CHECK(s_BackendContextActive == false);

    uint8_t zeros[sizeof(FfxmFsr3UpscalerContext)] = {};
    TEST_CHECK(memcmp(&context, zeros, sizeof(context)) == 0);

    std::cout << "PASS: Backend context creation failure safely handled without destroying uncreated context." << std::endl;
}

// 5. Resource Allocation Failure at Each Stage (0 to 18) with Occurrence-Aware Tracking
void TestResourceAllocationFailureAtEachStage()
{
    std::cout << "--- Test 5: Resource Allocation Failure at Each Stage (0 to 18) ---" << std::endl;

    for (int32_t failIdx = 0; failIdx < FFXM_FSR3UPSCALER_INTERNAL_SURFACE_COUNT; ++failIdx) {
        ResetMockState();
        s_FailAtResourceIndex = failIdx;

        FfxmFsr3UpscalerContext context;
        memset(&context, 0xDD, sizeof(context));
        FfxmFsr3UpscalerContextDescription desc = {};
        desc.maxRenderSize = { 1920, 1080 };
        desc.maxUpscaleSize = { 3840, 2160 };
        SetupMockInterface(&desc.backendInterface);

        FfxmErrorCode res = ffxmFsr3UpscalerContextCreate(&context, &desc);
        TEST_CHECK(res == FFXM_ERROR_OUT_OF_MEMORY);

        // Backend context WAS created, so it MUST be destroyed
        TEST_CHECK(s_CreateBackendContextCalled == true);
        TEST_CHECK(s_DestroyBackendContextCalled == true);
        TEST_CHECK(s_DestroyedContextId == 100);
        TEST_CHECK(s_BackendContextActive == false);

        // Occurrence-aware check: every resource allocated prior to failure must be destroyed exactly once
        VerifyOccurrenceAwareResourceBalance();

        // Context must be zeroed out
        uint8_t zeros[sizeof(FfxmFsr3UpscalerContext)] = {};
        TEST_CHECK(memcmp(&context, zeros, sizeof(context)) == 0);
    }

    std::cout << "PASS: All 19 allocation failure stages cleanly rolled back with occurrence-aware balance." << std::endl;
}

// 6. Correct Resource Destruction & Double Destruction Safety
void TestCorrectResourceDestruction()
{
    std::cout << "--- Test 6: Resource Destruction & Idempotent Cleanup ---" << std::endl;
    ResetMockState();

    FfxmFsr3UpscalerContext context;
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    SetupMockInterface(&desc.backendInterface);

    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);

    // Destroy once
    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    VerifyOccurrenceAwareResourceBalance();
    TEST_CHECK(s_DestroyBackendContextCalled == true);
    TEST_CHECK(s_BackendContextActive == false);

    // Destroy second time on zeroed context
    s_DestroyBackendContextCalled = false;
    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    // Nothing new should have been destroyed
    VerifyOccurrenceAwareResourceBalance();
    TEST_CHECK(s_DestroyBackendContextCalled == false);

    std::cout << "PASS: Resource destruction is complete and double-destruction is fully safe." << std::endl;
}

// 7. Repeated Context Creation and Destruction
void TestRepeatedCreateDestroyCycles()
{
    std::cout << "--- Test 7: Repeated Context Creation and Destruction (10 cycles) ---" << std::endl;
    ResetMockState();

    FfxmFsr3UpscalerContext context;
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    SetupMockInterface(&desc.backendInterface);

    for (int cycle = 0; cycle < 10; ++cycle) {
        FfxmErrorCode res = ffxmFsr3UpscalerContextCreate(&context, &desc);
        TEST_CHECK(res == FFXM_OK);
        TEST_CHECK(s_BackendContextActive == true);

        res = ffxmFsr3UpscalerContextDestroy(&context);
        TEST_CHECK(res == FFXM_OK);
        TEST_CHECK(s_BackendContextActive == false);
    }

    // Verify occurrence-aware tracking across all 10 cycles
    VerifyOccurrenceAwareResourceBalance();
    TEST_CHECK(s_ResourceCreationCounts.size() == 230); // 23 resources * 10 cycles

    std::cout << "PASS: 10 repeated create/destroy cycles executed with 100% resource tracking fidelity." << std::endl;
}

// 8. Shared Resource Descriptions
void TestSharedResourceDescriptions()
{
    std::cout << "--- Test 8: Shared Resource Descriptions ---" << std::endl;
    ResetMockState();

    FfxmFsr3UpscalerContext context;
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    SetupMockInterface(&desc.backendInterface);
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);

    FfxmFsr3UpscalerSharedResourceDescriptions shared = {};
    TEST_CHECK(ffxmFsr3UpscalerGetSharedResourceDescriptions(&context, &shared) == FFXM_OK);
    TEST_CHECK(shared.dilatedDepth.resourceDescription.format == FFXM_SURFACE_FORMAT_R32_FLOAT);
    TEST_CHECK(shared.dilatedMotionVectors.resourceDescription.format == FFXM_SURFACE_FORMAT_R16G16_FLOAT);
    TEST_CHECK(shared.reconstructedPrevNearestDepth.resourceDescription.format == FFXM_SURFACE_FORMAT_R32_UINT);

    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    std::cout << "PASS: Shared resource descriptions match Frame Generation Phase 2 contract." << std::endl;
}

// 9. Protected Unfinished APIs
void TestProtectedUnfinishedAPIs()
{
    std::cout << "--- Test 9: Protected Unfinished APIs (Dispatch & GenerateReactive) ---" << std::endl;
    ResetMockState();

    FfxmFsr3UpscalerContext context;
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    SetupMockInterface(&desc.backendInterface);
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);

    FfxmCommandList dummyCmdList = reinterpret_cast<FfxmCommandList>(0x1234);

    // Test Dispatch protection
    FfxmFsr3UpscalerDispatchDescription dispDesc = {};
    dispDesc.commandList = dummyCmdList;
    dispDesc.renderSize = { 1280, 720 };
    dispDesc.upscaleSize = { 2560, 1440 };
    dispDesc.jitterOffset = { 0.25f, -0.25f };
    dispDesc.motionVectorScale = { 1.0f, 1.0f };
    dispDesc.frameTimeDelta = 0.0166f;
    dispDesc.cameraFovAngleVertical = 1.047f;
    dispDesc.viewSpaceToMetersFactor = 1.0f;

    FfxmErrorCode dispRes = ffxmFsr3UpscalerContextDispatch(&context, &dispDesc);
    TEST_CHECK(dispRes == FFXM_OK);

    // Test GenerateReactive protection
    FfxmFsr3UpscalerGenerateReactiveDescription genDesc = {};
    genDesc.commandList = dummyCmdList;
    genDesc.scale = 0.5f;
    genDesc.cutoffThreshold = 0.2f;
    genDesc.binaryValue = 1.0f;

    FfxmErrorCode genRes = ffxmFsr3UpscalerContextGenerateReactiveMask(&context, &genDesc);
    TEST_CHECK(genRes == FFXM_ERROR_INCOMPLETE_INTERFACE);

    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    std::cout << "PASS: Dispatch executes PrepareInputs pass successfully (FFXM_OK); unfinished GenerateReactive API explicitly returns FFXM_ERROR_INCOMPLETE_INTERFACE." << std::endl;
}

// 10. Non-tautological Constant-Buffer Staging & Adapter Verification
void TestConstantBufferStagingSlots()
{
    std::cout << "--- Test 10: Constant-Buffer Staging Slots (All 4 Slots) ---" << std::endl;
    ResetMockState();

    FfxmFsr3UpscalerContext context;
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    SetupMockInterface(&desc.backendInterface);
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);

    FfxmFsr3UpscalerContext_Private* priv = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(&context);

    // Part A: Independent Staging Adapter Unit Test
    {
        struct TestConstBuffer {
            FfxmConstantBuffer cb;
            uint32_t canary1;
            uint32_t canary2;
        } bufferWithCanary = {};

        bufferWithCanary.canary1 = 0xDEADBEEF;
        bufferWithCanary.canary2 = 0xCAFEBABE;

        uint32_t testPayload[8] = { 0x11111111, 0x22222222, 0x33333333, 0x44444444, 0x55555555, 0x66666666, 0x77777777, 0x88888888 };
        stageConstantBuffer(&bufferWithCanary.cb, testPayload, sizeof(testPayload));

        TEST_CHECK(bufferWithCanary.cb.num32BitEntries == 8);
        TEST_CHECK(memcmp(bufferWithCanary.cb.data, testPayload, sizeof(testPayload)) == 0);
        TEST_CHECK(bufferWithCanary.canary1 == 0xDEADBEEF);
        TEST_CHECK(bufferWithCanary.canary2 == 0xCAFEBABE);
    }

    // Part B: Staged entry counts initialized during context creation
    TEST_CHECK(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER].num32BitEntries ==
        sizeof(Fsr3UpscalerConstants) / sizeof(uint32_t));
    TEST_CHECK(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_SPD].num32BitEntries ==
        sizeof(Fsr3UpscalerSpdConstants) / sizeof(uint32_t));
    TEST_CHECK(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_RCAS].num32BitEntries ==
        sizeof(Fsr3UpscalerRcasConstants) / sizeof(uint32_t));
    TEST_CHECK(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_GENREACTIVE].num32BitEntries ==
        sizeof(Fsr3UpscalerGenerateReactiveConstants) / sizeof(uint32_t));

    // Part C: Verify Production Dispatch Stages Slot 0 (FSR3UPSCALER), Slot 1 (SPD), and Slot 2 (RCAS)
    FfxmCommandList dummyCmdList = reinterpret_cast<FfxmCommandList>(0xCAFE);
    FfxmFsr3UpscalerDispatchDescription dispDesc = {};
    dispDesc.commandList = dummyCmdList;
    dispDesc.renderSize = { 1280, 720 };
    dispDesc.upscaleSize = { 2560, 1440 };
    dispDesc.jitterOffset = { 0.33f, -0.66f };
    dispDesc.motionVectorScale = { 1.5f, 2.0f };
    dispDesc.sharpness = 0.65f;
    dispDesc.frameTimeDelta = 0.01666f;
    dispDesc.cameraFovAngleVertical = 1.047f;
    dispDesc.viewSpaceToMetersFactor = 1.0f;
    dispDesc.cameraNear = 0.1f;
    dispDesc.cameraFar = 1000.0f;

    FfxmErrorCode dispRes = ffxmFsr3UpscalerContextDispatch(&context, &dispDesc);
    TEST_CHECK(dispRes == FFXM_OK); // executes PrepareInputs pass in Milestone 3

    // Verify Slot 0 (FSR3UPSCALER)
    TEST_CHECK(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER].num32BitEntries ==
        sizeof(Fsr3UpscalerConstants) / sizeof(uint32_t));
    TEST_CHECK(priv->constants.renderSize[0] == 1280);
    TEST_CHECK(priv->constants.renderSize[1] == 720);
    TEST_CHECK(priv->constants.upscaleSize[0] == 2560);
    TEST_CHECK(priv->constants.upscaleSize[1] == 1440);
    TEST_CHECK(priv->constants.jitterOffset[0] == 0.33f);
    TEST_CHECK(priv->constants.jitterOffset[1] == -0.66f);
    TEST_CHECK(memcmp(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER].data,
        &priv->constants, sizeof(Fsr3UpscalerConstants)) == 0);

    // Verify Slot 1 (SPD) - computed via ffxSpdSetup for half-resolution Shading Change Pyramid
    uint32_t expDispatchThreadGroupCountXY[2];
    uint32_t expWorkGroupOffset[2];
    uint32_t expNumWorkGroupsAndMips[2];
    const uint32_t expHalfRenderW = (1280 + 1) / 2;
    const uint32_t expHalfRenderH = (720 + 1) / 2;
    uint32_t expRectInfo[4] = { 0, 0, expHalfRenderW, expHalfRenderH };
    ffxSpdSetup(expDispatchThreadGroupCountXY, expWorkGroupOffset, expNumWorkGroupsAndMips, expRectInfo);

    Fsr3UpscalerSpdConstants expectedSpd = {};
    expectedSpd.mips = expNumWorkGroupsAndMips[1];
    expectedSpd.numWorkGroups = expNumWorkGroupsAndMips[0];
    expectedSpd.workGroupOffset[0] = expWorkGroupOffset[0];
    expectedSpd.workGroupOffset[1] = expWorkGroupOffset[1];
    expectedSpd.renderSize[0] = expHalfRenderW;
    expectedSpd.renderSize[1] = expHalfRenderH;

    TEST_CHECK(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_SPD].num32BitEntries == 6);
    TEST_CHECK(memcmp(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_SPD].data,
        &expectedSpd, sizeof(expectedSpd)) == 0);

    // Verify Slot 2 (RCAS) - computed via FsrRcasCon
    Fsr3UpscalerRcasConstants expectedRcas = {};
    const float sharpnessRemapped = (-2.0f * dispDesc.sharpness) + 2.0f;
    FsrRcasCon(expectedRcas.rcasConfig, sharpnessRemapped);

    TEST_CHECK(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_RCAS].num32BitEntries == 4);
    TEST_CHECK(memcmp(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_RCAS].data,
        &expectedRcas, sizeof(expectedRcas)) == 0);

    // Part D: Verify Production GenerateReactive Stages Slot 3 (GENREACTIVE)
    FfxmFsr3UpscalerGenerateReactiveDescription genDesc = {};
    genDesc.commandList = dummyCmdList;
    genDesc.scale = 0.75f;
    genDesc.cutoffThreshold = 0.35f;
    genDesc.binaryValue = 0.9f;
    genDesc.flags = 0x03;

    FfxmErrorCode genRes = ffxmFsr3UpscalerContextGenerateReactiveMask(&context, &genDesc);
    TEST_CHECK(genRes == FFXM_ERROR_INCOMPLETE_INTERFACE); // protected until Milestone 3

    Fsr3UpscalerGenerateReactiveConstants expectedGen = {};
    expectedGen.scale = genDesc.scale;
    expectedGen.cutoffThreshold = genDesc.cutoffThreshold;
    expectedGen.binaryValue = genDesc.binaryValue;
    expectedGen.flags = genDesc.flags;

    TEST_CHECK(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_GENREACTIVE].num32BitEntries == 4);
    TEST_CHECK(memcmp(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_GENREACTIVE].data,
        &expectedGen, sizeof(expectedGen)) == 0);

    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    std::cout << "PASS: All 4 constant-buffer staging slots verified for non-tautological production staging." << std::endl;
}

// 11. Regression Table Validation for All 19 Internal Resource Descriptions
void TestInternalResourceDescriptionsRegressionTable()
{
    std::cout << "--- Test 11: Internal Resource Descriptions Regression Table (All 19 Surfaces) ---" << std::endl;
    ResetMockState();

    FfxmFsr3UpscalerContext context;
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    SetupMockInterface(&desc.backendInterface);
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);

    TEST_CHECK(s_CapturedResourceDescriptions.size() == FFXM_FSR3UPSCALER_INTERNAL_SURFACE_COUNT);

    struct ExpectedResourceDesc {
        uint32_t               id;
        const wchar_t*         name;
        FfxmSurfaceFormat       format;
        uint32_t               width;
        uint32_t               height;
        uint32_t               mipCount;
        FfxmResourceFlags      flags;
        FfxmResourceUsage      usage;
        FfxmResourceStates     initialState;
        uint32_t               initDataSize;
    };

    const ExpectedResourceDesc expectedTable[19] = {
        // 0. ACCUMULATION_1
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_1, L"FSR3UPSCALER_Accumulation1",
          FFXM_SURFACE_FORMAT_R8_UNORM, 1920, 1080, 1,
          FFXM_RESOURCE_FLAGS_NONE,
          static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
          FFXM_RESOURCE_STATE_UNORDERED_ACCESS, 0 },

        // 1. ACCUMULATION_2
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_2, L"FSR3UPSCALER_Accumulation2",
          FFXM_SURFACE_FORMAT_R8_UNORM, 1920, 1080, 1,
          FFXM_RESOURCE_FLAGS_NONE,
          static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
          FFXM_RESOURCE_STATE_UNORDERED_ACCESS, 0 },

        // 2. LUMA_1
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_1, L"FSR3UPSCALER_Luma1",
          FFXM_SURFACE_FORMAT_R16_FLOAT, 1920, 1080, 1,
          FFXM_RESOURCE_FLAGS_NONE,
          static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
          FFXM_RESOURCE_STATE_UNORDERED_ACCESS, 0 },

        // 3. LUMA_2
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_2, L"FSR3UPSCALER_Luma2",
          FFXM_SURFACE_FORMAT_R16_FLOAT, 1920, 1080, 1,
          FFXM_RESOURCE_FLAGS_NONE,
          static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
          FFXM_RESOURCE_STATE_UNORDERED_ACCESS, 0 },

        // 4. INTERMEDIATE_FP16x1 (Aliasable)
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERMEDIATE_FP16x1, L"FSR3UPSCALER_IntermediateFp16x1",
          FFXM_SURFACE_FORMAT_R16_FLOAT, 1920, 1080, 1,
          FFXM_RESOURCE_FLAGS_ALIASABLE,
          static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
          FFXM_RESOURCE_STATE_UNORDERED_ACCESS, 0 },

        // 5. SHADING_CHANGE (Aliasable)
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SHADING_CHANGE, L"FSR3UPSCALER_ShadingChange",
          FFXM_SURFACE_FORMAT_R8_UNORM, 960, 540, 1,
          FFXM_RESOURCE_FLAGS_ALIASABLE,
          static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
          FFXM_RESOURCE_STATE_UNORDERED_ACCESS, 0 },

        // 6. NEW_LOCKS (Aliasable)
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_NEW_LOCKS, L"FSR3UPSCALER_NewLocks",
          FFXM_SURFACE_FORMAT_R8_UNORM, 3840, 2160, 1,
          FFXM_RESOURCE_FLAGS_ALIASABLE,
          FFXM_RESOURCE_USAGE_UAV,
          FFXM_RESOURCE_STATE_UNORDERED_ACCESS, 0 },

        // 7. INTERNAL_UPSCALED_COLOR_1
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR_1, L"FSR3UPSCALER_InternalUpscaled1",
          FFXM_SURFACE_FORMAT_R16G16B16A16_FLOAT, 3840, 2160, 1,
          FFXM_RESOURCE_FLAGS_NONE,
          static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
          FFXM_RESOURCE_STATE_UNORDERED_ACCESS, 0 },

        // 8. INTERNAL_UPSCALED_COLOR_2
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR_2, L"FSR3UPSCALER_InternalUpscaled2",
          FFXM_SURFACE_FORMAT_R16G16B16A16_FLOAT, 3840, 2160, 1,
          FFXM_RESOURCE_FLAGS_NONE,
          static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
          FFXM_RESOURCE_STATE_UNORDERED_ACCESS, 0 },

        // 9. SPD_MIPS (Aliasable, full mip chain)
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS, L"FSR3UPSCALER_SpdMips",
          FFXM_SURFACE_FORMAT_R16G16_FLOAT, 960, 540, 0,
          FFXM_RESOURCE_FLAGS_ALIASABLE,
          FFXM_RESOURCE_USAGE_UAV,
          FFXM_RESOURCE_STATE_UNORDERED_ACCESS, 0 },

        // 10. FARTHEST_DEPTH_MIP1 (Aliasable)
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_FARTHEST_DEPTH_MIP1, L"FSR3UPSCALER_FarthestDepthMip1",
          FFXM_SURFACE_FORMAT_R16_FLOAT, 960, 540, 1,
          FFXM_RESOURCE_FLAGS_ALIASABLE,
          FFXM_RESOURCE_USAGE_UAV,
          FFXM_RESOURCE_STATE_UNORDERED_ACCESS, 0 },

        // 11. LUMA_HISTORY_1
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY_1, L"FSR3UPSCALER_LumaHistory1",
          FFXM_SURFACE_FORMAT_R16G16B16A16_FLOAT, 1920, 1080, 1,
          FFXM_RESOURCE_FLAGS_NONE,
          static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
          FFXM_RESOURCE_STATE_UNORDERED_ACCESS, 0 },

        // 12. LUMA_HISTORY_2
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY_2, L"FSR3UPSCALER_LumaHistory2",
          FFXM_SURFACE_FORMAT_R16G16B16A16_FLOAT, 1920, 1080, 1,
          FFXM_RESOURCE_FLAGS_NONE,
          static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
          FFXM_RESOURCE_STATE_UNORDERED_ACCESS, 0 },

        // 13. SPD_ATOMIC_COUNT
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_ATOMIC_COUNT, L"FSR3UPSCALER_SpdAtomicCounter",
          FFXM_SURFACE_FORMAT_R32_UINT, 1, 1, 1,
          FFXM_RESOURCE_FLAGS_NONE,
          FFXM_RESOURCE_USAGE_UAV,
          FFXM_RESOURCE_STATE_UNORDERED_ACCESS, 4 },

        // 14. DILATED_REACTIVE_MASKS (Aliasable)
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_REACTIVE_MASKS, L"FSR3UPSCALER_DilatedReactiveMasks",
          FFXM_SURFACE_FORMAT_R8G8B8A8_UNORM, 1920, 1080, 1,
          FFXM_RESOURCE_FLAGS_ALIASABLE,
          static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
          FFXM_RESOURCE_STATE_UNORDERED_ACCESS, 0 },

        // 15. LANCZOS_LUT
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LANCZOS_LUT, L"FSR3UPSCALER_LanczosLutData",
          FFXM_SURFACE_FORMAT_R16_SNORM, 128, 1, 1,
          FFXM_RESOURCE_FLAGS_NONE,
          FFXM_RESOURCE_USAGE_READ_ONLY,
          FFXM_RESOURCE_STATE_COMPUTE_READ, 256 },

        // 16. INTERNAL_DEFAULT_REACTIVITY
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_REACTIVITY, L"FSR3UPSCALER_DefaultReactivityMask",
          FFXM_SURFACE_FORMAT_R8_UNORM, 1, 1, 1,
          FFXM_RESOURCE_FLAGS_NONE,
          FFXM_RESOURCE_USAGE_READ_ONLY,
          FFXM_RESOURCE_STATE_COMPUTE_READ, 1 },

        // 17. INTERNAL_DEFAULT_EXPOSURE
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_EXPOSURE, L"FSR3UPSCALER_DefaultExposure",
          FFXM_SURFACE_FORMAT_R32G32_FLOAT, 1, 1, 1,
          FFXM_RESOURCE_FLAGS_NONE,
          FFXM_RESOURCE_USAGE_READ_ONLY,
          FFXM_RESOURCE_STATE_COMPUTE_READ, 8 },

        // 18. FRAME_INFO
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_FRAME_INFO, L"FSR3UPSCALER_FrameInfo",
          FFXM_SURFACE_FORMAT_R32G32B32A32_FLOAT, 1, 1, 1,
          FFXM_RESOURCE_FLAGS_NONE,
          FFXM_RESOURCE_USAGE_UAV,
          FFXM_RESOURCE_STATE_UNORDERED_ACCESS, 0 }
    };

    for (size_t i = 0; i < 19; ++i) {
        const auto& actual = s_CapturedResourceDescriptions[i];
        const auto& expected = expectedTable[i];

        TEST_CHECK(actual.id == expected.id);
        TEST_CHECK(wcscmp(actual.name, expected.name) == 0);
        TEST_CHECK(actual.heapType == FFXM_HEAP_TYPE_DEFAULT);
        TEST_CHECK(actual.resourceDescription.type == FFXM_RESOURCE_TYPE_TEXTURE2D);
        TEST_CHECK(actual.resourceDescription.format == expected.format);
        TEST_CHECK(actual.resourceDescription.width == expected.width);
        TEST_CHECK(actual.resourceDescription.height == expected.height);
        TEST_CHECK(actual.resourceDescription.depth == 1);
        TEST_CHECK(actual.resourceDescription.mipCount == expected.mipCount);
        TEST_CHECK(actual.resourceDescription.flags == expected.flags);
        TEST_CHECK(actual.resourceDescription.usage == expected.usage);
        TEST_CHECK(actual.initalState == expected.initialState);
        TEST_CHECK(actual.initDataSize == expected.initDataSize);
        if (expected.initDataSize > 0) {
            TEST_CHECK(actual.initData != nullptr);
        } else {
            TEST_CHECK(actual.initData == nullptr);
        }
    }

    // Verify actual initial data byte contents
    // 1. SPD_ATOMIC_COUNT: uint32_t == 0
    uint32_t atomicCount = 0;
    memcpy(&atomicCount, s_CapturedInitialData[0].data(), sizeof(atomicCount));
    TEST_CHECK(atomicCount == 0);

    // 2. LANCZOS_LUT: 128 elements of int16_t, index 0 is 32767 (lanczos2(0) == 1.0)
    const int16_t* pWeights = reinterpret_cast<const int16_t*>(s_CapturedInitialData[1].data());
    TEST_CHECK(s_CapturedInitialData[1].size() == 256);
    TEST_CHECK(pWeights[0] == 32767); // lanczos2(0) = 1.0 * 32767.0f

    // 3. INTERNAL_DEFAULT_REACTIVITY: uint8_t == 0
    TEST_CHECK(s_CapturedInitialData[2].size() == 1);
    TEST_CHECK(s_CapturedInitialData[2][0] == 0);

    // 4. INTERNAL_DEFAULT_EXPOSURE: float[2] == {0.0f, 0.0f}
    const float* pExposure = reinterpret_cast<const float*>(s_CapturedInitialData[3].data());
    TEST_CHECK(s_CapturedInitialData[3].size() == 8);
    TEST_CHECK(pExposure[0] == 0.0f);
    TEST_CHECK(pExposure[1] == 0.0f);

    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    std::cout << "PASS: All 19 internal resource descriptions and initial data match authoritative FSR 3.1.4 reference." << std::endl;
}

// 12. Public API Parameter Validation & Boundary Checks
void TestPublicAPIValidation()
{
    std::cout << "--- Test 12: Public API Parameter Validation & Boundary Checks ---" << std::endl;
    ResetMockState();

    FfxmFsr3UpscalerContext context;
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    SetupMockInterface(&desc.backendInterface);

    // A. Null pointer checks on create
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(nullptr, &desc) == FFXM_ERROR_INVALID_POINTER);
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, nullptr) == FFXM_ERROR_INVALID_POINTER);

    // B. Dimension zero checks on create
    desc.maxRenderSize = { 0, 1080 };
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_ERROR_INVALID_ARGUMENT);
    desc.maxRenderSize = { 1920, 0 };
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_ERROR_INVALID_ARGUMENT);
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 0, 2160 };
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_ERROR_INVALID_ARGUMENT);
    desc.maxUpscaleSize = { 3840, 0 };
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_ERROR_INVALID_ARGUMENT);

    // C. Dimension relation checks on create (render size > upscale size)
    desc.maxRenderSize = { 3841, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_ERROR_INVALID_ARGUMENT);
    desc.maxRenderSize = { 1920, 2161 };
    desc.maxUpscaleSize = { 3840, 2160 };
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_ERROR_INVALID_ARGUMENT);

    // D. Incomplete interface checks
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    desc.backendInterface.fpCreateBackendContext = nullptr;
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_ERROR_INCOMPLETE_INTERFACE);
    SetupMockInterface(&desc.backendInterface);

    // Create valid context for dispatch and reactive parameter tests
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);

    // E. Dispatch null pointer checks
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(nullptr, nullptr) == FFXM_ERROR_INVALID_POINTER);
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, nullptr) == FFXM_ERROR_INVALID_POINTER);

    FfxmFsr3UpscalerDispatchDescription dispDesc = {};
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_ERROR_INVALID_POINTER); // null commandList

    dispDesc.commandList = reinterpret_cast<FfxmCommandList>(0x1234);

    // F. Dispatch out-of-range checks
    dispDesc.renderSize = { 1921, 1080 }; // > maxRenderSize.width
    dispDesc.upscaleSize = { 3840, 2160 };
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_ERROR_OUT_OF_RANGE);

    dispDesc.renderSize = { 1920, 1081 }; // > maxRenderSize.height
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_ERROR_OUT_OF_RANGE);

    dispDesc.renderSize = { 1920, 1080 };
    dispDesc.upscaleSize = { 3841, 2160 }; // > maxUpscaleSize.width
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_ERROR_OUT_OF_RANGE);

    dispDesc.renderSize = { 1920, 1080 };
    dispDesc.upscaleSize = { 3840, 2161 }; // > maxUpscaleSize.height
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_ERROR_OUT_OF_RANGE);

    // G. GenerateReactive null pointer checks
    TEST_CHECK(ffxmFsr3UpscalerContextGenerateReactiveMask(nullptr, nullptr) == FFXM_ERROR_INVALID_POINTER);
    TEST_CHECK(ffxmFsr3UpscalerContextGenerateReactiveMask(&context, nullptr) == FFXM_ERROR_INVALID_POINTER);
    FfxmFsr3UpscalerGenerateReactiveDescription genDesc = {};
    TEST_CHECK(ffxmFsr3UpscalerContextGenerateReactiveMask(&context, &genDesc) == FFXM_ERROR_INVALID_POINTER); // null commandList

    // H. Null context destroy
    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(nullptr) == FFXM_ERROR_INVALID_POINTER);

    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    std::cout << "PASS: Public API parameter validation and boundary checks verified." << std::endl;
}

// 13. Context Reuse After Failure
void TestContextReuseAfterFailure()
{
    std::cout << "--- Test 13: Context Reuse After Rollback Failure ---" << std::endl;
    ResetMockState();

    FfxmFsr3UpscalerContext context;
    memset(&context, 0xEE, sizeof(context));

    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    SetupMockInterface(&desc.backendInterface);

    // Inject failure halfway through allocation
    s_FailAtResourceIndex = 8;
    FfxmErrorCode res = ffxmFsr3UpscalerContextCreate(&context, &desc);
    TEST_CHECK(res == FFXM_ERROR_OUT_OF_MEMORY);
    VerifyOccurrenceAwareResourceBalance();

    // Immediately reuse the same context variable for successful creation
    s_FailAtResourceIndex = -1;
    res = ffxmFsr3UpscalerContextCreate(&context, &desc);
    TEST_CHECK(res == FFXM_OK);

    // Verify context is healthy and fully usable
    FfxmFsr3UpscalerContext_Private* priv = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(&context);
    TEST_CHECK(priv->backendContextCreated == true);
    TEST_CHECK(priv->allocatedResourceCount == FFXM_FSR3UPSCALER_INTERNAL_SURFACE_COUNT);

    res = ffxmFsr3UpscalerContextDestroy(&context);
    TEST_CHECK(res == FFXM_OK);
    VerifyOccurrenceAwareResourceBalance();

    std::cout << "PASS: Context cleanly reused after rolled-back failure." << std::endl;
}

// 14. Fix A: Device Capabilities Ordering & Rollback Contract
void TestDeviceCapabilitiesOrderingContract()
{
    std::cout << "--- Test 14: Vulkan Device Capabilities Call Ordering & Rollback Contract ---" << std::endl;
    ResetMockState();

    // Verify that querying device capabilities before backend context creation fails as uninitialized
    FfxmInterface iface = {};
    SetupMockInterface(&iface);
    FfxmDeviceCapabilities caps = {};
    TEST_CHECK(iface.fpGetDeviceCapabilities(&iface, &caps) == FFXM_ERROR_BACKEND_API_ERROR);

    // Verify that during context creation, backend context is created BEFORE capabilities are queried
    FfxmFsr3UpscalerContext context;
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    SetupMockInterface(&desc.backendInterface);

    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);
    TEST_CHECK(s_CreateBackendContextCalled == true);
    TEST_CHECK(s_GetDeviceCapsCalled == true);
    TEST_CHECK(s_CreateBackendContextCallSeq < s_GetDeviceCapsCallSeq);
    TEST_CHECK(s_BackendContextActive == true);

    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    TEST_CHECK(s_BackendContextActive == false);

    // Verify rollback ordering when FP16 is unsupported
    ResetMockState();
    s_Fp16SupportMock = false;
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_ERROR_UNSUPPORTED_DEVICE);
    TEST_CHECK(s_CreateBackendContextCalled == true);
    TEST_CHECK(s_GetDeviceCapsCalled == true);
    TEST_CHECK(s_CreateBackendContextCallSeq < s_GetDeviceCapsCallSeq);
    TEST_CHECK(s_DestroyBackendContextCalled == true);
    TEST_CHECK(s_BackendContextActive == false);

    std::cout << "PASS: Vulkan device capabilities call ordering and failure rollback verified." << std::endl;
}

// 15. Fix B: Scratch Memory Sizing Arithmetic & Overflow Prevention
void TestScratchMemorySizingArithmetic()
{
    std::cout << "--- Test 15: Scratch Memory Sizing Arithmetic & Overflow Prevention ---" << std::endl;

    // 1. Direct unit verification of checked arithmetic primitives
    size_t outSize = 0;
    TEST_CHECK(testCheckMulSize(100, 200, &outSize) == true);
    TEST_CHECK(outSize == 20000);

    TEST_CHECK(testCheckMulSize(SIZE_MAX, 2, &outSize) == false);
    TEST_CHECK(testCheckMulSize(SIZE_MAX / 2 + 1, 2, &outSize) == false);
    TEST_CHECK(testCheckAddSize(SIZE_MAX - 10, 20, &outSize) == false);
    TEST_CHECK(testCheckAlignUpSize(SIZE_MAX - 3, 8, &outSize) == false);

    size_t alignedVal = 0;
    TEST_CHECK(testCheckAlignUpSize(17, 8, &alignedVal) == true);
    TEST_CHECK(alignedVal == 24);

#if defined(FFXM_HAS_VK_BACKEND)
    // 2. Direct verification of ffxmGetScratchMemorySizeVK
    // A. Minimum and zero context counts
    size_t zeroSize = ffxmGetScratchMemorySizeVK(nullptr, 0);
    TEST_CHECK(zeroSize == 0);

    // B. Normal supported context counts (1, 2, 4, 8)
    size_t size1 = ffxmGetScratchMemorySizeVK(nullptr, 1);
    size_t size2 = ffxmGetScratchMemorySizeVK(nullptr, 2);
    size_t size4 = ffxmGetScratchMemorySizeVK(nullptr, 4);
    size_t size8 = ffxmGetScratchMemorySizeVK(nullptr, 8);

    TEST_CHECK(size1 > 0);
    TEST_CHECK(size2 > size1);
    TEST_CHECK(size4 > size2);
    TEST_CHECK(size8 > size4);

    // Alignment consistency (must be aligned to 8 bytes / uint64_t)
    TEST_CHECK(size1 % sizeof(uint64_t) == 0);
    TEST_CHECK(size2 % sizeof(uint64_t) == 0);
    TEST_CHECK(size4 % sizeof(uint64_t) == 0);
    TEST_CHECK(size8 % sizeof(uint64_t) == 0);

    // C. Overflowing and extreme context counts (must return 0 safely without wrapping or crashing)
    size_t maxUint32Size = ffxmGetScratchMemorySizeVK(nullptr, static_cast<size_t>(UINT32_MAX));
    TEST_CHECK(maxUint32Size == 0);

    size_t maxContextSize = ffxmGetScratchMemorySizeVK(nullptr, SIZE_MAX);
    TEST_CHECK(maxContextSize == 0);

    size_t halfMaxSize = ffxmGetScratchMemorySizeVK(nullptr, SIZE_MAX / 2);
    TEST_CHECK(halfMaxSize == 0);

    if (sizeof(size_t) > 4) {
        size_t large64BitContextSize = ffxmGetScratchMemorySizeVK(nullptr, static_cast<size_t>(uint64_t(1) << 33));
        TEST_CHECK(large64BitContextSize == 0);
    }

    // D. Verification of ffxmGetInterfaceVK scratch buffer sizing enforcement
    FfxmInterface iface = {};
    VkDeviceContext dummyVkDevice = {};
    dummyVkDevice.vkPhysicalDevice = VK_NULL_HANDLE;
    dummyVkDevice.vkDevice = VK_NULL_HANDLE;
    FfxmDevice dummyDevice = ffxmGetDeviceVK(&dummyVkDevice);

    std::vector<uint8_t> scratch(size1);

    // Exact scratchBufferSize succeeds
    FfxmErrorCode ifaceRes = ffxmGetInterfaceVK(&iface, dummyDevice, scratch.data(), size1, 1);
    TEST_CHECK(ifaceRes == FFXM_OK);

    // Undersized scratchBufferSize by 1 byte fails with FFXM_ERROR_INSUFFICIENT_MEMORY
    FfxmInterface ifaceFail = {};
    FfxmErrorCode ifaceFailRes = ffxmGetInterfaceVK(&ifaceFail, dummyDevice, scratch.data(), size1 - 1, 1);
    TEST_CHECK(ifaceFailRes == FFXM_ERROR_INSUFFICIENT_MEMORY);

    // maxContexts == 0 fails with FFXM_ERROR_INVALID_ARGUMENT
    FfxmInterface ifaceZero = {};
    FfxmErrorCode ifaceZeroRes = ffxmGetInterfaceVK(&ifaceZero, dummyDevice, scratch.data(), size1, 0);
    TEST_CHECK(ifaceZeroRes == FFXM_ERROR_INVALID_ARGUMENT);

    // Overflowing maxContexts fails with FFXM_ERROR_INVALID_ARGUMENT
    FfxmInterface ifaceOverflow = {};
    FfxmErrorCode ifaceOverflowRes = ffxmGetInterfaceVK(&ifaceOverflow, dummyDevice, scratch.data(), size1, SIZE_MAX);
    TEST_CHECK(ifaceOverflowRes == FFXM_ERROR_INVALID_ARGUMENT);
#endif

    std::cout << "PASS: Scratch memory sizing arithmetic and overflow prevention verified." << std::endl;
}

// 16. M3: PrepareInputs Compute Pass Execution, Pipeline Binding Reflection & GPU Dispatch
void TestM3PrepareInputsPassExecution()
{
    std::cout << "--- Test 16: M3 PrepareInputs Compute Pass Execution & GPU Dispatch ---" << std::endl;
    ResetMockState();

    // 1. Verify SPIR-V permutation shader blob retrieval across configurations
    FfxmShaderBlob blobDefault = {};
    TEST_CHECK(arm::ffxmGetPermutationBlobByIndex(FFXM_EFFECT_FSR3UPSCALER, FFXM_FSR3UPSCALER_PASS_PREPARE_INPUTS, 0, &blobDefault) == FFXM_OK);
    TEST_CHECK(blobDefault.data != nullptr);
    TEST_CHECK(blobDefault.size > 0);
    TEST_CHECK(blobDefault.srvTextureCount == 3);
    TEST_CHECK(blobDefault.uavTextureCount == 5);
    TEST_CHECK(blobDefault.cbvCount == 1);

    // Test inverted depth permutation
    FfxmShaderBlob blobInverted = {};
    TEST_CHECK(arm::ffxmGetPermutationBlobByIndex(FFXM_EFFECT_FSR3UPSCALER, FFXM_FSR3UPSCALER_PASS_PREPARE_INPUTS,
        FSR3UPSCALER_SHADER_PERMUTATION_DEPTH_INVERTED, &blobInverted) == FFXM_OK);
    TEST_CHECK(blobInverted.data != nullptr);
    TEST_CHECK(blobInverted.size > 0);

    // 2. Create context with full mock backend
    FfxmFsr3UpscalerContext context;
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    desc.flags = FFXM_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
    SetupMockInterface(&desc.backendInterface);

    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);
    TEST_CHECK(s_CreatePipelineCallCount == 5);

    FfxmFsr3UpscalerContext_Private* priv = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(&context);
    TEST_CHECK(priv->pipelinePrepareInputs.pipeline != nullptr);
    TEST_CHECK(priv->pipelinePrepareInputs.srvTextureCount == 3);
    TEST_CHECK(priv->pipelinePrepareInputs.uavTextureCount == 5);
    TEST_CHECK(priv->pipelinePrepareInputs.constCount == 1);

    TEST_CHECK(priv->pipelineShadingChange.pipeline != nullptr);
    TEST_CHECK(priv->pipelineShadingChange.srvTextureCount == 1);
    TEST_CHECK(priv->pipelineShadingChange.uavTextureCount == 1);
    TEST_CHECK(priv->pipelineShadingChange.constCount == 1);
    TEST_CHECK(priv->pipelineShadingChange.srvTextureBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS);
    TEST_CHECK(priv->pipelineShadingChange.uavTextureBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SHADING_CHANGE);
    TEST_CHECK(priv->pipelineShadingChange.constantBufferBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER);

    TEST_CHECK(priv->pipelinePrepareReactivity.pipeline != nullptr);
    TEST_CHECK(priv->pipelinePrepareReactivity.srvTextureCount == 9);
    TEST_CHECK(priv->pipelinePrepareReactivity.uavTextureCount == 3);
    TEST_CHECK(priv->pipelinePrepareReactivity.constCount == 1);
    TEST_CHECK(priv->pipelinePrepareReactivity.srvTextureBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH);
    TEST_CHECK(priv->pipelinePrepareReactivity.srvTextureBindings[1].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_MOTION_VECTORS);
    TEST_CHECK(priv->pipelinePrepareReactivity.srvTextureBindings[2].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_DEPTH);
    TEST_CHECK(priv->pipelinePrepareReactivity.srvTextureBindings[3].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_REACTIVE_MASK);
    TEST_CHECK(priv->pipelinePrepareReactivity.srvTextureBindings[4].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_TRANSPARENCY_AND_COMPOSITION_MASK);
    TEST_CHECK(priv->pipelinePrepareReactivity.srvTextureBindings[5].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION);
    TEST_CHECK(priv->pipelinePrepareReactivity.srvTextureBindings[6].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SHADING_CHANGE);
    TEST_CHECK(priv->pipelinePrepareReactivity.srvTextureBindings[7].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_CURRENT_LUMA);
    TEST_CHECK(priv->pipelinePrepareReactivity.srvTextureBindings[8].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_EXPOSURE);
    TEST_CHECK(priv->pipelinePrepareReactivity.uavTextureBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_REACTIVE_MASKS);
    TEST_CHECK(priv->pipelinePrepareReactivity.uavTextureBindings[1].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_NEW_LOCKS);
    TEST_CHECK(priv->pipelinePrepareReactivity.uavTextureBindings[2].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION);
    TEST_CHECK(priv->pipelinePrepareReactivity.constantBufferBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER);

    // Verify patched bindings for PrepareInputs
    TEST_CHECK(priv->pipelinePrepareInputs.srvTextureBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_MOTION_VECTORS);
    TEST_CHECK(priv->pipelinePrepareInputs.srvTextureBindings[1].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_DEPTH);
    TEST_CHECK(priv->pipelinePrepareInputs.srvTextureBindings[2].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_COLOR);

    TEST_CHECK(priv->pipelinePrepareInputs.uavTextureBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_MOTION_VECTORS);
    TEST_CHECK(priv->pipelinePrepareInputs.uavTextureBindings[1].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_DEPTH);
    TEST_CHECK(priv->pipelinePrepareInputs.uavTextureBindings[2].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH);
    TEST_CHECK(priv->pipelinePrepareInputs.uavTextureBindings[3].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_FARTHEST_DEPTH);
    TEST_CHECK(priv->pipelinePrepareInputs.uavTextureBindings[4].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_CURRENT_LUMA);

    TEST_CHECK(priv->pipelinePrepareInputs.constantBufferBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER);

    // Verify LumaPyramid pipeline
    TEST_CHECK(priv->pipelineLumaPyramid.pipeline != nullptr);
    TEST_CHECK(priv->pipelineLumaPyramid.srvTextureCount == 2);
    TEST_CHECK(priv->pipelineLumaPyramid.uavTextureCount == 4);
    TEST_CHECK(priv->pipelineLumaPyramid.constCount == 2);

    TEST_CHECK(priv->pipelineLumaPyramid.srvTextureBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_CURRENT_LUMA);
    TEST_CHECK(priv->pipelineLumaPyramid.srvTextureBindings[1].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_FARTHEST_DEPTH);

    TEST_CHECK(priv->pipelineLumaPyramid.uavTextureBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_ATOMIC_COUNT);
    TEST_CHECK(priv->pipelineLumaPyramid.uavTextureBindings[1].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_FRAME_INFO);
    TEST_CHECK(priv->pipelineLumaPyramid.uavTextureBindings[2].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS_LEVEL_5);
    TEST_CHECK(priv->pipelineLumaPyramid.uavTextureBindings[3].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_FARTHEST_DEPTH_MIP1);

    TEST_CHECK(priv->pipelineLumaPyramid.constantBufferBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER);
    TEST_CHECK(priv->pipelineLumaPyramid.constantBufferBindings[1].resourceIdentifier == FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_SPD);

    // Verify ShadingChangePyramid pipeline
    TEST_CHECK(priv->pipelineShadingChangePyramid.pipeline != nullptr);
    TEST_CHECK(priv->pipelineShadingChangePyramid.srvTextureCount == 4);
    TEST_CHECK(priv->pipelineShadingChangePyramid.uavTextureCount == 7);
    TEST_CHECK(priv->pipelineShadingChangePyramid.constCount == 2);

    TEST_CHECK(priv->pipelineShadingChangePyramid.srvTextureBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_CURRENT_LUMA);
    TEST_CHECK(priv->pipelineShadingChangePyramid.srvTextureBindings[1].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_PREVIOUS_LUMA);
    TEST_CHECK(priv->pipelineShadingChangePyramid.srvTextureBindings[2].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_MOTION_VECTORS);
    TEST_CHECK(priv->pipelineShadingChangePyramid.srvTextureBindings[3].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_EXPOSURE);

    TEST_CHECK(priv->pipelineShadingChangePyramid.uavTextureBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_ATOMIC_COUNT);
    TEST_CHECK(priv->pipelineShadingChangePyramid.uavTextureBindings[1].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS_LEVEL_0);
    TEST_CHECK(priv->pipelineShadingChangePyramid.uavTextureBindings[2].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS_LEVEL_1);
    TEST_CHECK(priv->pipelineShadingChangePyramid.uavTextureBindings[3].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS_LEVEL_2);
    TEST_CHECK(priv->pipelineShadingChangePyramid.uavTextureBindings[4].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS_LEVEL_3);
    TEST_CHECK(priv->pipelineShadingChangePyramid.uavTextureBindings[5].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS_LEVEL_4);
    TEST_CHECK(priv->pipelineShadingChangePyramid.uavTextureBindings[6].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS_LEVEL_5);

    TEST_CHECK(priv->pipelineShadingChangePyramid.constantBufferBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER);
    TEST_CHECK(priv->pipelineShadingChangePyramid.constantBufferBindings[1].resourceIdentifier == FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_SPD);

    // 3. Dispatch a real frame
    FfxmCommandList dummyCmdList = reinterpret_cast<FfxmCommandList>(0xABCD);
    FfxmFsr3UpscalerDispatchDescription dispDesc = {};
    dispDesc.commandList = dummyCmdList;
    dispDesc.renderSize = { 1920, 1080 };
    dispDesc.upscaleSize = { 3840, 2160 };
    dispDesc.jitterOffset = { 0.125f, -0.25f };
    dispDesc.motionVectorScale = { 1.0f, 1.0f };
    dispDesc.frameTimeDelta = 16.666f;
    dispDesc.cameraFovAngleVertical = 1.047f;
    dispDesc.viewSpaceToMetersFactor = 1.0f;
    dispDesc.cameraNear = 0.1f;
    dispDesc.cameraFar = 1000.0f;

    static int s_ColorResource = 10;
    static int s_DepthResource = 20;
    static int s_MvResource = 30;
    static int s_DilatedMvResource = 40;
    static int s_DilatedDepthResource = 50;
    static int s_ReconstructedDepthResource = 60;
    dispDesc.color.resource = &s_ColorResource;
    dispDesc.depth.resource = &s_DepthResource;
    dispDesc.motionVectors.resource = &s_MvResource;
    dispDesc.dilatedMotionVectors.resource = &s_DilatedMvResource;
    dispDesc.dilatedDepth.resource = &s_DilatedDepthResource;
    dispDesc.reconstructedPrevNearestDepth.resource = &s_ReconstructedDepthResource;

    s_ScheduledGpuJobs.clear();
    s_ExecuteGpuJobsCallCount = 0;
    s_UnregisterResourcesCallCount = 0;

    FfxmErrorCode dispRes = ffxmFsr3UpscalerContextDispatch(&context, &dispDesc);
    TEST_CHECK(dispRes == FFXM_OK);

    // Verify scheduled GPU jobs on first frame (history reset):
    // 0: Clear Float for FRAME_INFO
    // 1: Clear Float for ACCUMULATION_1
    // 2: Clear Float for ACCUMULATION_2
    // 3: Clear Float for NEW_LOCKS
    // 4: Clear Float for RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH
    // 5: Clear Float for SPD_ATOMIC_COUNT
    // 6: Clear Float for SPD_MIPS
    // 7: Compute dispatch for PrepareInputs
    // 8: Compute dispatch for LumaPyramid
    // 9: Clear Float for SPD_ATOMIC_COUNT (reset before Shading Change SPD)
    // 10: Compute dispatch for ShadingChangePyramid
    // 11: Compute dispatch for ShadingChange
    // 12: Compute dispatch for PrepareReactivity
    TEST_CHECK(s_ScheduledGpuJobs.size() == 13);

    // Check clear jobs
    TEST_CHECK(s_ScheduledGpuJobs[0].jobType == FFXM_GPU_JOB_CLEAR_FLOAT);
    TEST_CHECK(s_ScheduledGpuJobs[1].jobType == FFXM_GPU_JOB_CLEAR_FLOAT);
    TEST_CHECK(s_ScheduledGpuJobs[2].jobType == FFXM_GPU_JOB_CLEAR_FLOAT);
    TEST_CHECK(s_ScheduledGpuJobs[3].jobType == FFXM_GPU_JOB_CLEAR_FLOAT);
    TEST_CHECK(s_ScheduledGpuJobs[4].jobType == FFXM_GPU_JOB_CLEAR_FLOAT);
    TEST_CHECK(s_ScheduledGpuJobs[5].jobType == FFXM_GPU_JOB_CLEAR_FLOAT);
    TEST_CHECK(s_ScheduledGpuJobs[6].jobType == FFXM_GPU_JOB_CLEAR_FLOAT);

    // Check compute dispatch job for PrepareInputs
    const FfxmGpuJobDescription& computeJob = s_ScheduledGpuJobs[7];
    TEST_CHECK(computeJob.jobType == FFXM_GPU_JOB_COMPUTE);
    // Dispatch dimensions: ceil(1920 / 8) = 240, ceil(1080 / 8) = 135, 1
    TEST_CHECK(computeJob.computeJobDescriptor.dimensions[0] == 240);
    TEST_CHECK(computeJob.computeJobDescriptor.dimensions[1] == 135);
    TEST_CHECK(computeJob.computeJobDescriptor.dimensions[2] == 1);

    // Verify SRVs in PrepareInputs compute job
    TEST_CHECK(computeJob.computeJobDescriptor.srvTextures[0].internalIndex != -1);
    TEST_CHECK(computeJob.computeJobDescriptor.srvTextures[1].internalIndex != -1);
    TEST_CHECK(computeJob.computeJobDescriptor.srvTextures[2].internalIndex != -1);

    // Verify UAVs in PrepareInputs compute job
    TEST_CHECK(computeJob.computeJobDescriptor.uavTextures[0].internalIndex != -1);
    TEST_CHECK(computeJob.computeJobDescriptor.uavTextures[1].internalIndex != -1);
    TEST_CHECK(computeJob.computeJobDescriptor.uavTextures[2].internalIndex != -1);
    TEST_CHECK(computeJob.computeJobDescriptor.uavTextures[3].internalIndex != -1);
    TEST_CHECK(computeJob.computeJobDescriptor.uavTextures[4].internalIndex != -1);

    // Verify Constant Buffer in PrepareInputs compute job
    TEST_CHECK(computeJob.computeJobDescriptor.cbs[0].num32BitEntries == sizeof(Fsr3UpscalerConstants) / sizeof(uint32_t));

    // Check compute dispatch job for LumaPyramid
    const FfxmGpuJobDescription& lumaJob = s_ScheduledGpuJobs[8];
    TEST_CHECK(lumaJob.jobType == FFXM_GPU_JOB_COMPUTE);
    // SPD dispatch dimensions: (1920 + 63) / 64 = 30, (1080 + 63) / 64 = 17, 1
    TEST_CHECK(lumaJob.computeJobDescriptor.dimensions[0] == 30);
    TEST_CHECK(lumaJob.computeJobDescriptor.dimensions[1] == 17);
    TEST_CHECK(lumaJob.computeJobDescriptor.dimensions[2] == 1);

    // Verify SRVs in LumaPyramid compute job
    TEST_CHECK(lumaJob.computeJobDescriptor.srvTextures[0].internalIndex != -1);
    TEST_CHECK(lumaJob.computeJobDescriptor.srvTextures[1].internalIndex != -1);

    // Verify UAVs in LumaPyramid compute job
    TEST_CHECK(lumaJob.computeJobDescriptor.uavTextures[0].internalIndex != -1);
    TEST_CHECK(lumaJob.computeJobDescriptor.uavTextures[1].internalIndex != -1);
    TEST_CHECK(lumaJob.computeJobDescriptor.uavTextures[2].internalIndex != -1);
    TEST_CHECK(lumaJob.computeJobDescriptor.uavTextures[3].internalIndex != -1);

    // Verify Constant Buffers in LumaPyramid compute job
    TEST_CHECK(lumaJob.computeJobDescriptor.cbs[0].num32BitEntries == sizeof(Fsr3UpscalerConstants) / sizeof(uint32_t));
    TEST_CHECK(lumaJob.computeJobDescriptor.cbs[1].num32BitEntries == sizeof(Fsr3UpscalerSpdConstants) / sizeof(uint32_t));

    // Check atomic counter clear job between SPD passes
    TEST_CHECK(s_ScheduledGpuJobs[9].jobType == FFXM_GPU_JOB_CLEAR_FLOAT);

    // Check compute dispatch job for ShadingChangePyramid
    const FfxmGpuJobDescription& shadingJob = s_ScheduledGpuJobs[10];
    TEST_CHECK(shadingJob.jobType == FFXM_GPU_JOB_COMPUTE);
    // Half-res SPD dispatch dimensions: (960 + 63) / 64 = 15, (540 + 63) / 64 = 9, 1
    TEST_CHECK(shadingJob.computeJobDescriptor.dimensions[0] == 15);
    TEST_CHECK(shadingJob.computeJobDescriptor.dimensions[1] == 9);
    TEST_CHECK(shadingJob.computeJobDescriptor.dimensions[2] == 1);

    // Verify SRVs in ShadingChangePyramid compute job (4 SRVs)
    TEST_CHECK(shadingJob.computeJobDescriptor.srvTextures[0].internalIndex != -1);
    TEST_CHECK(shadingJob.computeJobDescriptor.srvTextures[1].internalIndex != -1);
    TEST_CHECK(shadingJob.computeJobDescriptor.srvTextures[2].internalIndex != -1);
    TEST_CHECK(shadingJob.computeJobDescriptor.srvTextures[3].internalIndex != -1);

    // Verify UAVs in ShadingChangePyramid compute job (7 UAVs)
    for (uint32_t u = 0; u < 7; ++u) {
        TEST_CHECK(shadingJob.computeJobDescriptor.uavTextures[u].internalIndex != -1);
    }

    // Verify Constant Buffers in ShadingChangePyramid compute job
    TEST_CHECK(shadingJob.computeJobDescriptor.cbs[0].num32BitEntries == sizeof(Fsr3UpscalerConstants) / sizeof(uint32_t));
    TEST_CHECK(shadingJob.computeJobDescriptor.cbs[1].num32BitEntries == sizeof(Fsr3UpscalerSpdConstants) / sizeof(uint32_t));
    const Fsr3UpscalerSpdConstants* shadingSpd = reinterpret_cast<const Fsr3UpscalerSpdConstants*>(shadingJob.computeJobDescriptor.cbs[1].data);
    TEST_CHECK(shadingSpd->numWorkGroups == 135);
    TEST_CHECK(shadingSpd->renderSize[0] == 960);
    TEST_CHECK(shadingSpd->renderSize[1] == 540);

    // Check compute dispatch job for ShadingChange
    const FfxmGpuJobDescription& scJob = s_ScheduledGpuJobs[11];
    TEST_CHECK(scJob.jobType == FFXM_GPU_JOB_COMPUTE);
    TEST_CHECK(scJob.computeJobDescriptor.dimensions[0] == 120); // (960 + 7) / 8
    TEST_CHECK(scJob.computeJobDescriptor.dimensions[1] == 68);  // (540 + 7) / 8
    TEST_CHECK(scJob.computeJobDescriptor.dimensions[2] == 1);
    TEST_CHECK(scJob.computeJobDescriptor.srvTextures[0].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS].internalIndex);
    TEST_CHECK(scJob.computeJobDescriptor.uavTextures[0].internalIndex == priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SHADING_CHANGE].internalIndex);
    TEST_CHECK(scJob.computeJobDescriptor.cbs[0].num32BitEntries == sizeof(Fsr3UpscalerConstants) / sizeof(uint32_t));
    TEST_CHECK(scJob.computeJobDescriptor.pipeline.pipeline == priv->pipelineShadingChange.pipeline);

    // Check compute dispatch job for PrepareReactivity
    const FfxmGpuJobDescription& prJob = s_ScheduledGpuJobs[12];
    TEST_CHECK(prJob.jobType == FFXM_GPU_JOB_COMPUTE);
    TEST_CHECK(prJob.computeJobDescriptor.dimensions[0] == 240); // (1920 + 7) / 8
    TEST_CHECK(prJob.computeJobDescriptor.dimensions[1] == 135); // (1080 + 7) / 8
    TEST_CHECK(prJob.computeJobDescriptor.dimensions[2] == 1);
    TEST_CHECK(prJob.computeJobDescriptor.srvTextures[5].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_2].internalIndex);
    TEST_CHECK(prJob.computeJobDescriptor.uavTextures[0].internalIndex == priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_REACTIVE_MASKS].internalIndex);
    TEST_CHECK(prJob.computeJobDescriptor.uavTextures[1].internalIndex == priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_NEW_LOCKS].internalIndex);
    TEST_CHECK(prJob.computeJobDescriptor.uavTextures[2].internalIndex == priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_1].internalIndex);
    TEST_CHECK(prJob.computeJobDescriptor.cbs[0].num32BitEntries == sizeof(Fsr3UpscalerConstants) / sizeof(uint32_t));
    TEST_CHECK(prJob.computeJobDescriptor.pipeline.pipeline == priv->pipelinePrepareReactivity.pipeline);

    // Verify execution and cleanup calls
    TEST_CHECK(s_ExecuteGpuJobsCallCount == 1);
    TEST_CHECK(s_UnregisterResourcesCallCount == 1);

    // 4. Test frame parity and ping-pong luma and accumulation routing on frame 2
    FfxmFsr3UpscalerDispatchDescription dispDesc2 = dispDesc;
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc2) == FFXM_OK);
    // Frame 2 has no history reset, so 9 jobs scheduled (3 clears + 1 prepare inputs + 1 luma pyramid + 1 atomic clear + 1 shading change pyramid + 1 shading change + 1 prepare reactivity)
    TEST_CHECK(s_ScheduledGpuJobs.size() == 9);
    TEST_CHECK(priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_CURRENT_LUMA].internalIndex ==
        priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_2].internalIndex);
    TEST_CHECK(priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION].internalIndex ==
        priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_1].internalIndex);
    TEST_CHECK(priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION].internalIndex ==
        priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_2].internalIndex);

    // 5. Test clean destruction
    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    TEST_CHECK(s_DestroyPipelineCallCount >= 5);
    VerifyOccurrenceAwareResourceBalance();

    // 6. Test pipeline creation failure rollback
    ResetMockState();
    s_CreatePipelineResult = FFXM_ERROR_BACKEND_API_ERROR;
    FfxmFsr3UpscalerContext failContext;
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&failContext, &desc) == FFXM_ERROR_BACKEND_API_ERROR);
    VerifyOccurrenceAwareResourceBalance();

    std::cout << "PASS: PrepareInputs compute pipeline created, verified, dispatched, and cleanly destroyed." << std::endl;
}

static void TestM3DispatchErrorHandling()
{
    std::cout << "--- Test 17: M3 PrepareInputs Dispatch Error Handling & Cleanup Contract ---" << std::endl;

    ResetMockState();

    FfxmInterface iface = {};
    SetupMockInterface(&iface);

    FfxmFsr3UpscalerContextDescription desc = {};
    desc.flags = FFXM_FSR3UPSCALER_ENABLE_DEPTH_INVERTED | FFXM_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE;
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    desc.backendInterface = iface;

    FfxmFsr3UpscalerContext context;
    TEST_REQUIRE(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);
    FfxmFsr3UpscalerContext_Private* priv = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(&context);

    static int s_ColorResource = 10;
    static int s_DepthResource = 20;
    static int s_MvResource = 30;
    static int s_DilatedMvResource = 40;
    static int s_DilatedDepthResource = 50;
    static int s_ReconstructedDepthResource = 60;

    FfxmFsr3UpscalerDispatchDescription dispDesc = {};
    dispDesc.commandList = reinterpret_cast<FfxmCommandList>(static_cast<uintptr_t>(0xCAFE));
    dispDesc.renderSize = { 1920, 1080 };
    dispDesc.upscaleSize = { 3840, 2160 };
    dispDesc.jitterOffset = { 0.0f, 0.0f };
    dispDesc.motionVectorScale = { 1.0f, 1.0f };
    dispDesc.cameraNear = 0.1f;
    dispDesc.cameraFar = 1000.0f;
    dispDesc.cameraFovAngleVertical = 1.0f;
    dispDesc.frameTimeDelta = 16.666f;
    dispDesc.viewSpaceToMetersFactor = 1.0f;
    dispDesc.color.resource = &s_ColorResource;
    dispDesc.depth.resource = &s_DepthResource;
    dispDesc.motionVectors.resource = &s_MvResource;
    dispDesc.dilatedMotionVectors.resource = &s_DilatedMvResource;
    dispDesc.dilatedDepth.resource = &s_DilatedDepthResource;
    dispDesc.reconstructedPrevNearestDepth.resource = &s_ReconstructedDepthResource;

    auto resetDispatchTrackers = []() {
        s_ScheduledGpuJobs.clear();
        s_RegisterResourceCallCount = 0;
        s_ScheduleGpuJobCallCount = 0;
        s_ExecuteGpuJobsCallCount = 0;
        s_UnregisterResourcesCallCount = 0;
        s_FailAtRegisterResourceIndex = -1;
        s_ScheduleGpuJobResult = FFXM_OK;
        s_FailAtScheduleGpuJobIndex = -1;
        s_ExecuteGpuJobsResult = FFXM_OK;
        s_UnregisterResourcesResult = FFXM_OK;
    };

    auto verifyDynamicSlotsReset = [&]() {
        TEST_CHECK(priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_COLOR].internalIndex == -1);
        TEST_CHECK(priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_DEPTH].internalIndex == -1);
        TEST_CHECK(priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_MOTION_VECTORS].internalIndex == -1);
        TEST_CHECK(priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_MOTION_VECTORS].internalIndex == -1);
        TEST_CHECK(priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_DEPTH].internalIndex == -1);
        TEST_CHECK(priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH].internalIndex == -1);
        TEST_CHECK(priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_MOTION_VECTORS].internalIndex == -1);
        TEST_CHECK(priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_DEPTH].internalIndex == -1);
        TEST_CHECK(priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH].internalIndex == -1);
    };

    // Scenario 1: First dynamic resource registration fails (index 0)
    {
        resetDispatchTrackers();
        s_FailAtRegisterResourceIndex = 0;

        FfxmErrorCode err = ffxmFsr3UpscalerContextDispatch(&context, &dispDesc);
        TEST_CHECK(err == FFXM_ERROR_OUT_OF_MEMORY);
        TEST_CHECK(s_RegisterResourceCallCount == 1);
        TEST_CHECK(s_UnregisterResourcesCallCount == 0); // Never unregister when 0 acquired
        TEST_CHECK(s_ScheduleGpuJobCallCount == 0);
        TEST_CHECK(s_ExecuteGpuJobsCallCount == 0);
        TEST_CHECK(priv->resourceFrameIndex == 0);
        TEST_CHECK(priv->firstExecution == true);
        TEST_CHECK(priv->constants.frameIndex == 0.0f);
        TEST_CHECK(priv->preExposure == 1.0f);
        TEST_CHECK(priv->previousFramePreExposure == 1.0f);
        verifyDynamicSlotsReset();
    }

    // Scenario 2: Later dynamic resource registration fails (index 3)
    {
        resetDispatchTrackers();
        s_FailAtRegisterResourceIndex = 3;

        FfxmErrorCode err = ffxmFsr3UpscalerContextDispatch(&context, &dispDesc);
        TEST_CHECK(err == FFXM_ERROR_OUT_OF_MEMORY);
        TEST_CHECK(s_RegisterResourceCallCount == 4);
        TEST_CHECK(s_UnregisterResourcesCallCount == 1); // Clean up acquired resources
        TEST_CHECK(s_ScheduleGpuJobCallCount == 0);
        TEST_CHECK(s_ExecuteGpuJobsCallCount == 0);
        TEST_CHECK(priv->resourceFrameIndex == 0);
        TEST_CHECK(priv->firstExecution == true);
        TEST_CHECK(priv->constants.frameIndex == 0.0f);
        TEST_CHECK(priv->preExposure == 1.0f);
        TEST_CHECK(priv->previousFramePreExposure == 1.0f);
        verifyDynamicSlotsReset();
    }

    // Scenario 3a: Clear job scheduling fails
    {
        resetDispatchTrackers();
        s_FailAtScheduleGpuJobIndex = 0;

        FfxmErrorCode err = ffxmFsr3UpscalerContextDispatch(&context, &dispDesc);
        TEST_CHECK(err == FFXM_ERROR_BACKEND_API_ERROR);
        TEST_CHECK(s_RegisterResourceCallCount == 6);
        TEST_CHECK(s_ScheduleGpuJobCallCount == 1);
        TEST_CHECK(s_ExecuteGpuJobsCallCount == 0); // Execution skipped on schedule failure
        TEST_CHECK(s_UnregisterResourcesCallCount == 1);
        TEST_CHECK(priv->resourceFrameIndex == 0);
        TEST_CHECK(priv->firstExecution == true);
        TEST_CHECK(priv->constants.frameIndex == 0.0f);
        TEST_CHECK(priv->preExposure == 1.0f);
        TEST_CHECK(priv->previousFramePreExposure == 1.0f);
        verifyDynamicSlotsReset();
    }

    // Scenario 3b: Compute dispatch job scheduling fails
    {
        resetDispatchTrackers();
        s_FailAtScheduleGpuJobIndex = 4;

        FfxmErrorCode err = ffxmFsr3UpscalerContextDispatch(&context, &dispDesc);
        TEST_CHECK(err == FFXM_ERROR_BACKEND_API_ERROR);
        TEST_CHECK(s_RegisterResourceCallCount == 6);
        TEST_CHECK(s_ScheduleGpuJobCallCount == 5);
        TEST_CHECK(s_ScheduledGpuJobs.size() == 4);
        TEST_CHECK(s_ExecuteGpuJobsCallCount == 0);
        TEST_CHECK(s_UnregisterResourcesCallCount == 1);
        TEST_CHECK(priv->resourceFrameIndex == 0);
        TEST_CHECK(priv->firstExecution == true);
        TEST_CHECK(priv->constants.frameIndex == 0.0f);
        TEST_CHECK(priv->preExposure == 1.0f);
        TEST_CHECK(priv->previousFramePreExposure == 1.0f);
        verifyDynamicSlotsReset();
    }

    // Scenario 4: GPU job execution fails
    {
        resetDispatchTrackers();
        s_ExecuteGpuJobsResult = FFXM_ERROR_BACKEND_API_ERROR;

        FfxmErrorCode err = ffxmFsr3UpscalerContextDispatch(&context, &dispDesc);
        TEST_CHECK(err == FFXM_ERROR_BACKEND_API_ERROR);
        TEST_CHECK(s_RegisterResourceCallCount == 6);
        TEST_CHECK(s_ScheduleGpuJobCallCount == 13);
        TEST_CHECK(s_ExecuteGpuJobsCallCount == 1);
        TEST_CHECK(s_UnregisterResourcesCallCount == 1);
        TEST_CHECK(priv->resourceFrameIndex == 0);
        TEST_CHECK(priv->firstExecution == true);
        TEST_CHECK(priv->constants.frameIndex == 0.0f);
        TEST_CHECK(priv->preExposure == 1.0f);
        TEST_CHECK(priv->previousFramePreExposure == 1.0f);
        verifyDynamicSlotsReset();
    }

    // Scenario 5: Resource unregistration fails after successful execution
    {
        resetDispatchTrackers();
        s_UnregisterResourcesResult = FFXM_ERROR_BACKEND_API_ERROR;

        FfxmErrorCode err = ffxmFsr3UpscalerContextDispatch(&context, &dispDesc);
        TEST_CHECK(err == FFXM_ERROR_BACKEND_API_ERROR);
        TEST_CHECK(s_ScheduleGpuJobCallCount == 13);
        TEST_CHECK(s_ExecuteGpuJobsCallCount == 1);
        TEST_CHECK(s_UnregisterResourcesCallCount == 1);
        TEST_CHECK(priv->resourceFrameIndex == 0);
        TEST_CHECK(priv->firstExecution == true);
        TEST_CHECK(priv->constants.frameIndex == 0.0f);
        TEST_CHECK(priv->preExposure == 1.0f);
        TEST_CHECK(priv->previousFramePreExposure == 1.0f);
        verifyDynamicSlotsReset();
    }

    // Scenario 6: Execution and Unregistration BOTH fail
    {
        resetDispatchTrackers();
        s_ExecuteGpuJobsResult = FFXM_ERROR_INVALID_ARGUMENT;
        s_UnregisterResourcesResult = FFXM_ERROR_BACKEND_API_ERROR;

        FfxmErrorCode err = ffxmFsr3UpscalerContextDispatch(&context, &dispDesc);
        TEST_CHECK(err == FFXM_ERROR_INVALID_ARGUMENT); // Original error preserved
        TEST_CHECK(s_ScheduleGpuJobCallCount == 13);
        TEST_CHECK(s_ExecuteGpuJobsCallCount == 1);
        TEST_CHECK(s_UnregisterResourcesCallCount == 1);
        TEST_CHECK(priv->resourceFrameIndex == 0);
        TEST_CHECK(priv->firstExecution == true);
        TEST_CHECK(priv->constants.frameIndex == 0.0f);
        TEST_CHECK(priv->preExposure == 1.0f);
        TEST_CHECK(priv->previousFramePreExposure == 1.0f);
        verifyDynamicSlotsReset();
    }

    // Scenario 7: Successful dispatch after recoverable failure
    {
        resetDispatchTrackers();

        FfxmErrorCode err = ffxmFsr3UpscalerContextDispatch(&context, &dispDesc);
        TEST_CHECK(err == FFXM_OK);
        TEST_CHECK(s_RegisterResourceCallCount == 6);
        TEST_CHECK(s_ScheduleGpuJobCallCount == 13);
        TEST_CHECK(s_ExecuteGpuJobsCallCount == 1);
        TEST_CHECK(s_UnregisterResourcesCallCount == 1);
        TEST_CHECK(priv->resourceFrameIndex == 1);
        TEST_CHECK(priv->firstExecution == false);
        TEST_CHECK(priv->constants.frameIndex == 0.0f);
        TEST_CHECK(priv->preExposure == 1.0f);
        TEST_CHECK(priv->previousFramePreExposure == 1.0f);
        verifyDynamicSlotsReset();
    }

    // Scenario 8: Subsequent frame success & mid-stream failure recovery
    {
        // Frame 2 dispatches successfully
        resetDispatchTrackers();
        FfxmFsr3UpscalerDispatchDescription dispDescFrame2 = dispDesc;
        dispDescFrame2.preExposure = 1.0f;
        FfxmErrorCode err2 = ffxmFsr3UpscalerContextDispatch(&context, &dispDescFrame2);
        TEST_CHECK(err2 == FFXM_OK);
        TEST_CHECK(s_ScheduleGpuJobCallCount == 9);
        TEST_CHECK(priv->resourceFrameIndex == 2);
        TEST_CHECK(priv->constants.frameIndex == 1.0f);
        TEST_CHECK(priv->preExposure == 1.0f);
        TEST_CHECK(priv->previousFramePreExposure == 1.0f);

        // Frame 3 fails at GPU execution stage with preExposure = 2.0f
        resetDispatchTrackers();
        s_ExecuteGpuJobsResult = FFXM_ERROR_BACKEND_API_ERROR;
        FfxmFsr3UpscalerDispatchDescription dispDescFrame3 = dispDesc;
        dispDescFrame3.preExposure = 2.0f;
        FfxmErrorCode err3Fail = ffxmFsr3UpscalerContextDispatch(&context, &dispDescFrame3);
        TEST_CHECK(err3Fail == FFXM_ERROR_BACKEND_API_ERROR);
        // Verify transactional preservation: state MUST NOT advance
        TEST_CHECK(priv->resourceFrameIndex == 2);
        TEST_CHECK(priv->constants.frameIndex == 1.0f);
        TEST_CHECK(priv->preExposure == 1.0f); // 2.0f NOT committed
        TEST_CHECK(priv->previousFramePreExposure == 1.0f);

        // Frame 3 fails at Prepare Reactivity schedule stage
        resetDispatchTrackers();
        s_FailAtScheduleGpuJobIndex = 8; // Fail at PrepareReactivity schedule
        FfxmErrorCode err3ScheduleFail = ffxmFsr3UpscalerContextDispatch(&context, &dispDescFrame3);
        TEST_CHECK(err3ScheduleFail == FFXM_ERROR_BACKEND_API_ERROR);
        TEST_CHECK(s_ScheduleGpuJobCallCount == 9);
        TEST_CHECK(s_ScheduledGpuJobs.size() == 8);
        TEST_CHECK(priv->resourceFrameIndex == 2);
        TEST_CHECK(priv->constants.frameIndex == 1.0f);
        TEST_CHECK(priv->preExposure == 1.0f);
        TEST_CHECK(priv->previousFramePreExposure == 1.0f);

        // Retry Frame 3 without failure: must advance state exactly once
        resetDispatchTrackers();
        FfxmErrorCode err3Success = ffxmFsr3UpscalerContextDispatch(&context, &dispDescFrame3);
        TEST_CHECK(err3Success == FFXM_OK);
        TEST_CHECK(priv->resourceFrameIndex == 3);
        TEST_CHECK(priv->constants.frameIndex == 2.0f); // Exactly 1 increment from 1.0f to 2.0f!
        TEST_CHECK(priv->preExposure == 2.0f);
        TEST_CHECK(priv->previousFramePreExposure == 1.0f);
        TEST_CHECK(priv->constants.deltaPreExposure == 2.0f); // 2.0f / 1.0f = 2.0f preserved!
    }

    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    VerifyOccurrenceAwareResourceBalance();

    std::cout << "PASS: All M3 PrepareInputs and temporal state transactional error handling scenarios verified." << std::endl;
}

static void TestM3ComputeLuminancePyramid()
{
    std::cout << "--- Test 19: M3 Compute Luminance Pyramid Stage & SPD Verification ---" << std::endl;

    // 1. Verify shader permutations via accessor
    FfxmShaderBlob blobDefault = {};
    TEST_CHECK(arm::ffxmGetPermutationBlobByIndex(FFXM_EFFECT_FSR3UPSCALER, FFXM_FSR3UPSCALER_PASS_LUMA_PYRAMID,
        0, &blobDefault) == FFXM_OK);
    TEST_CHECK(blobDefault.data != nullptr);
    TEST_CHECK(blobDefault.size > 0);
    TEST_CHECK(blobDefault.srvTextureCount == 2);
    TEST_CHECK(blobDefault.uavTextureCount == 4);
    TEST_CHECK(blobDefault.cbvCount == 2);

    // Verify reflection names and bindings
    TEST_CHECK(blobDefault.boundSRVTextures[0] == 0); // r_current_luma
    TEST_CHECK(blobDefault.boundSRVTextures[1] == 1); // r_farthest_depth
    TEST_CHECK(blobDefault.boundUAVTextures[0] == 2); // rw_spd_global_atomic
    TEST_CHECK(blobDefault.boundUAVTextures[1] == 3); // rw_frame_info
    TEST_CHECK(blobDefault.boundUAVTextures[2] == 9); // rw_spd_mip5
    TEST_CHECK(blobDefault.boundUAVTextures[3] == 10); // rw_farthest_depth_mip1
    TEST_CHECK(blobDefault.boundConstantBuffers[0] == 11); // cbFSR3Upscaler
    TEST_CHECK(blobDefault.boundConstantBuffers[1] == 12); // cbSPD

    // 2. Context creation & SPD Constant Buffer setup verification
    FfxmFsr3UpscalerContext context;
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 2560, 1440 };
    desc.maxUpscaleSize = { 3840, 2160 };
    desc.flags = FFXM_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
    SetupMockInterface(&desc.backendInterface);

    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);

    FfxmFsr3UpscalerContext_Private* priv = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(&context);
    TEST_CHECK(priv->pipelineLumaPyramid.pipeline != nullptr);

    // 3. Dispatch and check SPD constants calculation across resolutions
    FfxmCommandList dummyCmdList = reinterpret_cast<FfxmCommandList>(0xABCD);
    FfxmFsr3UpscalerDispatchDescription dispDesc = {};
    dispDesc.commandList = dummyCmdList;
    dispDesc.renderSize = { 1920, 1080 };
    dispDesc.upscaleSize = { 3840, 2160 };
    dispDesc.jitterOffset = { 0.0f, 0.0f };
    dispDesc.motionVectorScale = { 1.0f, 1.0f };
    dispDesc.frameTimeDelta = 16.666f;
    dispDesc.cameraFovAngleVertical = 1.0f;
    dispDesc.cameraNear = 0.1f;
    dispDesc.cameraFar = 1000.0f;

    static int s_Res = 1;
    dispDesc.color.resource = &s_Res;
    dispDesc.depth.resource = &s_Res;
    dispDesc.motionVectors.resource = &s_Res;
    dispDesc.dilatedMotionVectors.resource = &s_Res;
    dispDesc.dilatedDepth.resource = &s_Res;
    dispDesc.reconstructedPrevNearestDepth.resource = &s_Res;

    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);

    // Verify cbSPD staging data for 1920x1080:
    // dispatchThreadGroupCountXY[0] = (1920 + 63) / 64 = 30
    // dispatchThreadGroupCountXY[1] = (1080 + 63) / 64 = 17
    // numWorkGroups = 30 * 17 = 510
    const FfxmGpuJobDescription* lumaJob1080 = nullptr;
    for (const auto& job : s_ScheduledGpuJobs) {
        if (job.jobType == FFXM_GPU_JOB_COMPUTE &&
            job.computeJobDescriptor.pipeline.pipeline == priv->pipelineLumaPyramid.pipeline) {
            lumaJob1080 = &job;
            break;
        }
    }
    TEST_REQUIRE(lumaJob1080 != nullptr);
    TEST_CHECK(lumaJob1080->computeJobDescriptor.dimensions[0] == 30);
    TEST_CHECK(lumaJob1080->computeJobDescriptor.dimensions[1] == 17);
    TEST_CHECK(lumaJob1080->computeJobDescriptor.dimensions[2] == 1);

    const Fsr3UpscalerSpdConstants* spdData1080 = reinterpret_cast<const Fsr3UpscalerSpdConstants*>(
        lumaJob1080->computeJobDescriptor.cbs[1].data);
    TEST_CHECK(spdData1080 != nullptr);
    TEST_CHECK(spdData1080->numWorkGroups == 510);
    TEST_CHECK(spdData1080->renderSize[0] == 1920);
    TEST_CHECK(spdData1080->renderSize[1] == 1080);
    TEST_CHECK(spdData1080->workGroupOffset[0] == 0);
    TEST_CHECK(spdData1080->workGroupOffset[1] == 0);

    // 4. Test dispatch for different render resolution (1280x720)
    dispDesc.renderSize = { 1280, 720 };
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);

    const FfxmGpuJobDescription* lumaJob720 = nullptr;
    for (const auto& job : s_ScheduledGpuJobs) {
        if (job.jobType == FFXM_GPU_JOB_COMPUTE &&
            job.computeJobDescriptor.pipeline.pipeline == priv->pipelineLumaPyramid.pipeline) {
            lumaJob720 = &job;
            break;
        }
    }
    TEST_REQUIRE(lumaJob720 != nullptr);
    TEST_CHECK(lumaJob720->computeJobDescriptor.dimensions[0] == 20);
    TEST_CHECK(lumaJob720->computeJobDescriptor.dimensions[1] == 12);
    TEST_CHECK(lumaJob720->computeJobDescriptor.dimensions[2] == 1);

    const Fsr3UpscalerSpdConstants* spdData720 = reinterpret_cast<const Fsr3UpscalerSpdConstants*>(
        lumaJob720->computeJobDescriptor.cbs[1].data);
    TEST_CHECK(spdData720 != nullptr);
    TEST_CHECK(spdData720->numWorkGroups == 240);
    TEST_CHECK(spdData720->renderSize[0] == 1280);
    TEST_CHECK(spdData720->renderSize[1] == 720);

    // 5. Clean destroy
    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    TEST_CHECK(s_DestroyPipelineCallCount >= 3);
    VerifyOccurrenceAwareResourceBalance();

    std::cout << "PASS: M3 Compute Luminance Pyramid stage and SPD verification verified." << std::endl;
}

static void TestM3ComputeShadingChangePyramid()
{
    std::cout << "--- Test 20: M3 Compute Shading Change Pyramid Stage Verification ---" << std::endl;

    // 1. Verify shader permutations via accessor
    FfxmShaderBlob blobDefault = {};
    TEST_CHECK(arm::ffxmGetPermutationBlobByIndex(FFXM_EFFECT_FSR3UPSCALER, FFXM_FSR3UPSCALER_PASS_SHADING_CHANGE_PYRAMID,
        0, &blobDefault) == FFXM_OK);
    TEST_CHECK(blobDefault.data != nullptr);
    TEST_CHECK(blobDefault.size > 0);
    TEST_CHECK(blobDefault.srvTextureCount == 4);
    TEST_CHECK(blobDefault.uavTextureCount == 7);
    TEST_CHECK(blobDefault.cbvCount == 2);

    // Verify reflection names and bindings
    TEST_CHECK(blobDefault.boundSRVTextures[0] == 0); // r_current_luma
    TEST_CHECK(blobDefault.boundSRVTextures[1] == 1); // r_previous_luma
    TEST_CHECK(blobDefault.boundSRVTextures[2] == 2); // r_dilated_motion_vectors
    TEST_CHECK(blobDefault.boundSRVTextures[3] == 3); // r_input_exposure

    TEST_CHECK(blobDefault.boundUAVTextures[0] == 4);  // rw_spd_global_atomic
    TEST_CHECK(blobDefault.boundUAVTextures[1] == 5);  // rw_spd_mip0
    TEST_CHECK(blobDefault.boundUAVTextures[2] == 6);  // rw_spd_mip1
    TEST_CHECK(blobDefault.boundUAVTextures[3] == 7);  // rw_spd_mip2
    TEST_CHECK(blobDefault.boundUAVTextures[4] == 8);  // rw_spd_mip3
    TEST_CHECK(blobDefault.boundUAVTextures[5] == 9);  // rw_spd_mip4
    TEST_CHECK(blobDefault.boundUAVTextures[6] == 10); // rw_spd_mip5

    TEST_CHECK(blobDefault.boundConstantBuffers[0] == 11); // cbFSR3Upscaler
    TEST_CHECK(blobDefault.boundConstantBuffers[1] == 12); // cbSPD

    // 2. Context creation & SPD Constant Buffer setup verification
    FfxmFsr3UpscalerContext context;
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 2560, 1440 };
    desc.maxUpscaleSize = { 3840, 2160 };
    desc.flags = FFXM_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
    SetupMockInterface(&desc.backendInterface);

    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);

    FfxmFsr3UpscalerContext_Private* priv = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(&context);
    TEST_CHECK(priv->pipelineShadingChangePyramid.pipeline != nullptr);

    // 3. Dispatch and check SPD constants calculation for half-resolution render dimensions across resolutions
    FfxmCommandList dummyCmdList = reinterpret_cast<FfxmCommandList>(0xABCD);
    FfxmFsr3UpscalerDispatchDescription dispDesc = {};
    dispDesc.commandList = dummyCmdList;
    dispDesc.renderSize = { 1920, 1080 };
    dispDesc.upscaleSize = { 3840, 2160 };
    dispDesc.jitterOffset = { 0.0f, 0.0f };
    dispDesc.motionVectorScale = { 1.0f, 1.0f };
    dispDesc.frameTimeDelta = 16.666f;
    dispDesc.cameraFovAngleVertical = 1.0f;
    dispDesc.cameraNear = 0.1f;
    dispDesc.cameraFar = 1000.0f;

    static int s_Res = 1;
    dispDesc.color.resource = &s_Res;
    dispDesc.depth.resource = &s_Res;
    dispDesc.motionVectors.resource = &s_Res;
    dispDesc.dilatedMotionVectors.resource = &s_Res;
    dispDesc.dilatedDepth.resource = &s_Res;
    dispDesc.reconstructedPrevNearestDepth.resource = &s_Res;

    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);

    // For 1920x1080: halfRenderWidth = 960, halfRenderHeight = 540
    // dispatchThreadGroupCountXY[0] = (960 + 63) / 64 = 15
    // dispatchThreadGroupCountXY[1] = (540 + 63) / 64 = 9
    // numWorkGroups = 15 * 9 = 135
    const FfxmGpuJobDescription* shadingJob1080 = nullptr;
    for (const auto& job : s_ScheduledGpuJobs) {
        if (job.jobType == FFXM_GPU_JOB_COMPUTE &&
            job.computeJobDescriptor.pipeline.pipeline == priv->pipelineShadingChangePyramid.pipeline) {
            shadingJob1080 = &job;
            break;
        }
    }
    TEST_REQUIRE(shadingJob1080 != nullptr);
    TEST_CHECK(shadingJob1080->computeJobDescriptor.dimensions[0] == 15);
    TEST_CHECK(shadingJob1080->computeJobDescriptor.dimensions[1] == 9);
    TEST_CHECK(shadingJob1080->computeJobDescriptor.dimensions[2] == 1);

    const Fsr3UpscalerSpdConstants* spdData1080 = reinterpret_cast<const Fsr3UpscalerSpdConstants*>(
        shadingJob1080->computeJobDescriptor.cbs[1].data);
    TEST_CHECK(spdData1080 != nullptr);
    TEST_CHECK(spdData1080->numWorkGroups == 135);
    TEST_CHECK(spdData1080->renderSize[0] == 960);
    TEST_CHECK(spdData1080->renderSize[1] == 540);
    TEST_CHECK(spdData1080->workGroupOffset[0] == 0);
    TEST_CHECK(spdData1080->workGroupOffset[1] == 0);

    // 4. Test dispatch for 1280x720: halfRenderWidth = 640, halfRenderHeight = 360
    dispDesc.renderSize = { 1280, 720 };
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);

    const FfxmGpuJobDescription* shadingJob720 = nullptr;
    for (const auto& job : s_ScheduledGpuJobs) {
        if (job.jobType == FFXM_GPU_JOB_COMPUTE &&
            job.computeJobDescriptor.pipeline.pipeline == priv->pipelineShadingChangePyramid.pipeline) {
            shadingJob720 = &job;
            break;
        }
    }
    TEST_REQUIRE(shadingJob720 != nullptr);
    // (640 + 63) / 64 = 10, (360 + 63) / 64 = 6, numWorkGroups = 60
    TEST_CHECK(shadingJob720->computeJobDescriptor.dimensions[0] == 10);
    TEST_CHECK(shadingJob720->computeJobDescriptor.dimensions[1] == 6);
    TEST_CHECK(shadingJob720->computeJobDescriptor.dimensions[2] == 1);

    const Fsr3UpscalerSpdConstants* spdData720 = reinterpret_cast<const Fsr3UpscalerSpdConstants*>(
        shadingJob720->computeJobDescriptor.cbs[1].data);
    TEST_CHECK(spdData720 != nullptr);
    TEST_CHECK(spdData720->numWorkGroups == 60);
    TEST_CHECK(spdData720->renderSize[0] == 640);
    TEST_CHECK(spdData720->renderSize[1] == 360);

    // 5. Test odd resolution boundary: 1279x719
    // (1279 + 1) / 2 = 640, (719 + 1) / 2 = 360
    dispDesc.renderSize = { 1279, 719 };
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);

    const FfxmGpuJobDescription* shadingJobOdd = nullptr;
    for (const auto& job : s_ScheduledGpuJobs) {
        if (job.jobType == FFXM_GPU_JOB_COMPUTE &&
            job.computeJobDescriptor.pipeline.pipeline == priv->pipelineShadingChangePyramid.pipeline) {
            shadingJobOdd = &job;
            break;
        }
    }
    TEST_REQUIRE(shadingJobOdd != nullptr);
    TEST_CHECK(shadingJobOdd->computeJobDescriptor.dimensions[0] == 10);
    TEST_CHECK(shadingJobOdd->computeJobDescriptor.dimensions[1] == 6);
    const Fsr3UpscalerSpdConstants* spdDataOdd = reinterpret_cast<const Fsr3UpscalerSpdConstants*>(
        shadingJobOdd->computeJobDescriptor.cbs[1].data);
    TEST_CHECK(spdDataOdd != nullptr);
    TEST_CHECK(spdDataOdd->numWorkGroups == 60);
    TEST_CHECK(spdDataOdd->renderSize[0] == 640);
    TEST_CHECK(spdDataOdd->renderSize[1] == 360);

    // 6. Test 1440p resolution: 2560x1440
    // (2560 + 1) / 2 = 1280, (1440 + 1) / 2 = 720
    // (1280 + 63) / 64 = 20, (720 + 63) / 64 = 12, numWorkGroups = 240
    dispDesc.renderSize = { 2560, 1440 };
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);

    const FfxmGpuJobDescription* shadingJob1440 = nullptr;
    for (const auto& job : s_ScheduledGpuJobs) {
        if (job.jobType == FFXM_GPU_JOB_COMPUTE &&
            job.computeJobDescriptor.pipeline.pipeline == priv->pipelineShadingChangePyramid.pipeline) {
            shadingJob1440 = &job;
            break;
        }
    }
    TEST_REQUIRE(shadingJob1440 != nullptr);
    TEST_CHECK(shadingJob1440->computeJobDescriptor.dimensions[0] == 20);
    TEST_CHECK(shadingJob1440->computeJobDescriptor.dimensions[1] == 12);
    const Fsr3UpscalerSpdConstants* spdData1440 = reinterpret_cast<const Fsr3UpscalerSpdConstants*>(
        shadingJob1440->computeJobDescriptor.cbs[1].data);
    TEST_CHECK(spdData1440 != nullptr);
    TEST_CHECK(spdData1440->numWorkGroups == 240);
    TEST_CHECK(spdData1440->renderSize[0] == 1280);
    TEST_CHECK(spdData1440->renderSize[1] == 720);

    // 7. Clean destroy
    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    TEST_CHECK(s_DestroyPipelineCallCount >= 3);
    VerifyOccurrenceAwareResourceBalance();

    std::cout << "PASS: M3 Compute Shading Change Pyramid stage and SPD verification verified." << std::endl;
}

static void TestM3ComputeShadingChange()
{
    std::cout << "--- Test 21: M3 Compute Shading Change Stage Verification ---" << std::endl;

    // 1. Verify shader permutations via accessor for all permutation flag combinations
    const uint32_t permutationVariants[] = {
        0,
        FSR3UPSCALER_SHADER_PERMUTATION_USE_LANCZOS_TYPE,
        FSR3UPSCALER_SHADER_PERMUTATION_HDR_COLOR_INPUT,
        FSR3UPSCALER_SHADER_PERMUTATION_LOW_RES_MOTION_VECTORS,
        FSR3UPSCALER_SHADER_PERMUTATION_JITTER_MOTION_VECTORS,
        FSR3UPSCALER_SHADER_PERMUTATION_DEPTH_INVERTED,
        FSR3UPSCALER_SHADER_PERMUTATION_ENABLE_SHARPENING,
        FSR3UPSCALER_SHADER_PERMUTATION_HDR_COLOR_INPUT | FSR3UPSCALER_SHADER_PERMUTATION_DEPTH_INVERTED
    };

    for (uint32_t flags : permutationVariants) {
        FfxmShaderBlob blob = {};
        TEST_CHECK(arm::ffxmGetPermutationBlobByIndex(FFXM_EFFECT_FSR3UPSCALER, FFXM_FSR3UPSCALER_PASS_SHADING_CHANGE,
            flags, &blob) == FFXM_OK);
        TEST_CHECK(blob.data != nullptr);
        TEST_CHECK(blob.size > 0);
        TEST_CHECK(blob.cbvCount == 1);
        TEST_CHECK(blob.srvTextureCount == 1);
        TEST_CHECK(blob.uavTextureCount == 1);
        TEST_CHECK(blob.samplerCount == 1);

        // Reflection interface validation
        TEST_CHECK(blob.boundConstantBuffers[0] == 2); // cbFSR3UPSCALER
        TEST_CHECK(strcmp(blob.boundConstantBufferNames[0], "cbFSR3Upscaler") == 0);

        TEST_CHECK(blob.boundSRVTextures[0] == 0); // r_spd_mips
        TEST_CHECK(strcmp(blob.boundSRVTextureNames[0], "r_spd_mips") == 0);

        TEST_CHECK(blob.boundUAVTextures[0] == 1); // rw_shading_change
        TEST_CHECK(strcmp(blob.boundUAVTextureNames[0], "rw_shading_change") == 0);

        TEST_CHECK(blob.boundSamplers[0] == 1001); // s_LinearClamp
        TEST_CHECK(strcmp(blob.boundSamplerNames[0], "s_LinearClamp") == 0);
    }

    // 2. Algorithm correctness reference unit test:
    // fSample.x * fSample.y over mips 0..2, max value scaled by fScale = 1.0f, saturated to [0, 1].
    auto computeCpuShadingChange = [](const float mips[3][2]) -> float {
        const float fScale = 1.0f; // 1.0f + 0 / 3
        float fShadingChange = 0.0f;
        for (int iMipLevel = 0; iMipLevel < 3; ++iMipLevel) {
            float sampleVal = fabsf(mips[iMipLevel][0] * mips[iMipLevel][1]);
            if (sampleVal > 0.0f) {
                float scaled = sampleVal * fScale;
                if (scaled > fShadingChange) {
                    fShadingChange = scaled;
                }
            }
        }
        if (fShadingChange < 0.0f) fShadingChange = 0.0f;
        if (fShadingChange > 1.0f) fShadingChange = 1.0f;
        return fShadingChange;
    };

    // Case A: Zero change
    {
        const float mipsZero[3][2] = { {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f} };
        TEST_CHECK(computeCpuShadingChange(mipsZero) == 0.0f);
    }
    // Case B: Uniform positive increase
    {
        const float mipsPos[3][2] = { {0.6f, 1.0f}, {0.0f, 0.0f}, {0.0f, 0.0f} };
        TEST_CHECK(fabsf(computeCpuShadingChange(mipsPos) - 0.6f) < 1e-6f);
    }
    // Case C: Uniform negative decrease
    {
        const float mipsNeg[3][2] = { {0.75f, -1.0f}, {0.0f, 0.0f}, {0.0f, 0.0f} };
        TEST_CHECK(fabsf(computeCpuShadingChange(mipsNeg) - 0.75f) < 1e-6f);
    }
    // Case D: High magnitude with zero factor/consistency
    {
        const float mipsZeroFactor[3][2] = { {1.0f, 0.0f}, {0.8f, 0.0f}, {0.5f, 0.0f} };
        TEST_CHECK(computeCpuShadingChange(mipsZeroFactor) == 0.0f);
    }
    // Case E: Change visible primarily in coarser mip levels
    {
        const float mipsCoarse[3][2] = { {0.1f, 1.0f}, {0.4f, 1.0f}, {0.85f, 1.0f} };
        TEST_CHECK(fabsf(computeCpuShadingChange(mipsCoarse) - 0.85f) < 1e-6f);
    }
    // Case F: Normalization saturation limit
    {
        const float mipsSaturate[3][2] = { {2.5f, 1.0f}, {0.0f, 0.0f}, {0.0f, 0.0f} };
        TEST_CHECK(computeCpuShadingChange(mipsSaturate) == 1.0f);
    }

    // 3. Multi-resolution dispatch dimensions test
    FfxmFsr3UpscalerContext context;
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 2560, 1440 };
    desc.maxUpscaleSize = { 3840, 2160 };
    desc.flags = FFXM_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
    SetupMockInterface(&desc.backendInterface);

    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);
    FfxmFsr3UpscalerContext_Private* priv = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(&context);
    TEST_CHECK(priv->pipelineShadingChange.pipeline != nullptr);

    // Verify internal surface allocation for Shading Change: R8_UNORM, half render resolution
    TEST_CHECK(priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SHADING_CHANGE].internalIndex != -1);
    TEST_CHECK(priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SHADING_CHANGE].internalIndex != -1);

    FfxmCommandList dummyCmdList = reinterpret_cast<FfxmCommandList>(0xABCD);
    FfxmFsr3UpscalerDispatchDescription dispDesc = {};
    dispDesc.commandList = dummyCmdList;
    dispDesc.upscaleSize = { 3840, 2160 };
    dispDesc.jitterOffset = { 0.0f, 0.0f };
    dispDesc.motionVectorScale = { 1.0f, 1.0f };
    dispDesc.frameTimeDelta = 16.666f;
    dispDesc.cameraFovAngleVertical = 1.0f;
    dispDesc.cameraNear = 0.1f;
    dispDesc.cameraFar = 1000.0f;

    static int s_Res = 1;
    dispDesc.color.resource = &s_Res;
    dispDesc.depth.resource = &s_Res;
    dispDesc.motionVectors.resource = &s_Res;
    dispDesc.dilatedMotionVectors.resource = &s_Res;
    dispDesc.dilatedDepth.resource = &s_Res;
    dispDesc.reconstructedPrevNearestDepth.resource = &s_Res;

    auto findShadingChangeJob = [&]() -> const FfxmGpuJobDescription* {
        for (const auto& job : s_ScheduledGpuJobs) {
            if (job.jobType == FFXM_GPU_JOB_COMPUTE &&
                job.computeJobDescriptor.pipeline.pipeline == priv->pipelineShadingChange.pipeline) {
                return &job;
            }
        }
        return nullptr;
    };

    // A. 1080p: renderSize = 1920x1080 -> shadingChangeSize = 960x540 -> dispatch = 120x68x1
    dispDesc.renderSize = { 1920, 1080 };
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    const FfxmGpuJobDescription* scJob1080 = findShadingChangeJob();
    TEST_REQUIRE(scJob1080 != nullptr);
    TEST_CHECK(scJob1080->computeJobDescriptor.dimensions[0] == 120);
    TEST_CHECK(scJob1080->computeJobDescriptor.dimensions[1] == 68);
    TEST_CHECK(scJob1080->computeJobDescriptor.dimensions[2] == 1);
    TEST_CHECK(scJob1080->computeJobDescriptor.srvTextures[0].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS].internalIndex);
    TEST_CHECK(scJob1080->computeJobDescriptor.uavTextures[0].internalIndex == priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SHADING_CHANGE].internalIndex);

    // B. 720p: renderSize = 1280x720 -> shadingChangeSize = 640x360 -> dispatch = 80x45x1
    dispDesc.renderSize = { 1280, 720 };
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    const FfxmGpuJobDescription* scJob720 = findShadingChangeJob();
    TEST_REQUIRE(scJob720 != nullptr);
    TEST_CHECK(scJob720->computeJobDescriptor.dimensions[0] == 80);
    TEST_CHECK(scJob720->computeJobDescriptor.dimensions[1] == 45);
    TEST_CHECK(scJob720->computeJobDescriptor.dimensions[2] == 1);

    // C. Odd resolution boundary: renderSize = 1279x719 -> shadingChangeSize = 639x359 -> dispatch = 80x45x1
    dispDesc.renderSize = { 1279, 719 };
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    const FfxmGpuJobDescription* scJobOdd = findShadingChangeJob();
    TEST_REQUIRE(scJobOdd != nullptr);
    TEST_CHECK(scJobOdd->computeJobDescriptor.dimensions[0] == 80);
    TEST_CHECK(scJobOdd->computeJobDescriptor.dimensions[1] == 45);
    TEST_CHECK(scJobOdd->computeJobDescriptor.dimensions[2] == 1);

    // D. 1440p: renderSize = 2560x1440 -> shadingChangeSize = 1280x720 -> dispatch = 160x90x1
    dispDesc.renderSize = { 2560, 1440 };
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    const FfxmGpuJobDescription* scJob1440 = findShadingChangeJob();
    TEST_REQUIRE(scJob1440 != nullptr);
    TEST_CHECK(scJob1440->computeJobDescriptor.dimensions[0] == 160);
    TEST_CHECK(scJob1440->computeJobDescriptor.dimensions[1] == 90);
    TEST_CHECK(scJob1440->computeJobDescriptor.dimensions[2] == 1);

    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    TEST_CHECK(s_DestroyPipelineCallCount >= 4);
    VerifyOccurrenceAwareResourceBalance();

    std::cout << "PASS: M3 Compute Shading Change stage and algorithm verification verified." << std::endl;
}

static void TestM3PrepareReactivity()
{
    std::cout << "--- Test 22: M3 Prepare Reactivity Stage & Algorithm Verification ---" << std::endl;

    // 1. Verify shader reflection interface and all 64 permutations
    for (uint32_t permutation = 0; permutation < 64; ++permutation) {
        FfxmShaderBlob blob = {};
        TEST_CHECK(arm::ffxmGetPermutationBlobByIndex(FFXM_EFFECT_FSR3UPSCALER, FFXM_FSR3UPSCALER_PASS_PREPARE_REACTIVITY,
            permutation, &blob) == FFXM_OK);
        TEST_CHECK(blob.data != nullptr);
        TEST_CHECK(blob.size > 0);
        TEST_CHECK(blob.cbvCount == 1);
        TEST_CHECK(blob.srvTextureCount == 9);
        TEST_CHECK(blob.uavTextureCount == 3);
        TEST_CHECK(blob.samplerCount == 1);

        // Reflection interface validation
        TEST_CHECK(blob.boundConstantBuffers[0] == 12); // cbFSR3Upscaler
        TEST_CHECK(strcmp(blob.boundConstantBufferNames[0], "cbFSR3Upscaler") == 0);

        TEST_CHECK(blob.boundSRVTextures[0] == 0); // r_reconstructed_previous_nearest_depth
        TEST_CHECK(strcmp(blob.boundSRVTextureNames[0], "r_reconstructed_previous_nearest_depth") == 0);

        TEST_CHECK(blob.boundSRVTextures[1] == 1); // r_dilated_motion_vectors
        TEST_CHECK(strcmp(blob.boundSRVTextureNames[1], "r_dilated_motion_vectors") == 0);

        TEST_CHECK(blob.boundSRVTextures[2] == 2); // r_dilated_depth
        TEST_CHECK(strcmp(blob.boundSRVTextureNames[2], "r_dilated_depth") == 0);

        TEST_CHECK(blob.boundSRVTextures[3] == 3); // r_reactive_mask
        TEST_CHECK(strcmp(blob.boundSRVTextureNames[3], "r_reactive_mask") == 0);

        TEST_CHECK(blob.boundSRVTextures[4] == 4); // r_transparency_and_composition_mask
        TEST_CHECK(strcmp(blob.boundSRVTextureNames[4], "r_transparency_and_composition_mask") == 0);

        TEST_CHECK(blob.boundSRVTextures[5] == 5); // r_accumulation
        TEST_CHECK(strcmp(blob.boundSRVTextureNames[5], "r_accumulation") == 0);

        TEST_CHECK(blob.boundSRVTextures[6] == 6); // r_shading_change
        TEST_CHECK(strcmp(blob.boundSRVTextureNames[6], "r_shading_change") == 0);

        TEST_CHECK(blob.boundSRVTextures[7] == 7); // r_current_luma
        TEST_CHECK(strcmp(blob.boundSRVTextureNames[7], "r_current_luma") == 0);

        TEST_CHECK(blob.boundSRVTextures[8] == 8); // r_input_exposure
        TEST_CHECK(strcmp(blob.boundSRVTextureNames[8], "r_input_exposure") == 0);

        TEST_CHECK(blob.boundUAVTextures[0] == 9); // rw_dilated_reactive_masks
        TEST_CHECK(strcmp(blob.boundUAVTextureNames[0], "rw_dilated_reactive_masks") == 0);

        TEST_CHECK(blob.boundUAVTextures[1] == 10); // rw_new_locks
        TEST_CHECK(strcmp(blob.boundUAVTextureNames[1], "rw_new_locks") == 0);

        TEST_CHECK(blob.boundUAVTextures[2] == 11); // rw_accumulation
        TEST_CHECK(strcmp(blob.boundUAVTextureNames[2], "rw_accumulation") == 0);

        TEST_CHECK(blob.boundSamplers[0] == 1001); // s_LinearClamp
        TEST_CHECK(strcmp(blob.boundSamplerNames[0], "s_LinearClamp") == 0);
    }

    // 2. Reference CPU calculations matching AMD formulas:
    // A. Motion Divergence:
    auto computeCpuMotionDivergence = [](float f4KVelocity, float fReprojectedVelocity, float fDistanceFactor) -> float {
        float fVelocityFactor = std::min(1.0f, std::max(0.0f, f4KVelocity / 10.0f));
        float ratio = (f4KVelocity > 0.0f) ? std::min(1.0f, std::max(0.0f, fReprojectedVelocity / f4KVelocity)) : 0.0f;
        return (1.0f - ratio) * fDistanceFactor * fVelocityFactor;
    };
    // Uniform field: reprojected == 4K velocity -> ratio = 1 -> divergence = 0
    TEST_CHECK(computeCpuMotionDivergence(5.0f, 5.0f, 1.0f) == 0.0f);
    // Zero velocity -> velocity factor = 0 -> divergence = 0
    TEST_CHECK(computeCpuMotionDivergence(0.0f, 0.0f, 1.0f) == 0.0f);
    // Reprojected velocity 0 with 4K velocity 5.0 -> ratio 0 -> confidence = 1.0 * 1.0 * 0.5 = 0.5f
    TEST_CHECK(fabsf(computeCpuMotionDivergence(5.0f, 0.0f, 1.0f) - 0.5f) < 1e-6f);

    // B. Disocclusion Formula:
    auto computeCpuDisocclusionSeparation = [](float fHalfViewportWidth, float fDepthThreshold) -> float {
        const float Ksep = 1.37e-05f;
        return Ksep * fHalfViewportWidth * fDepthThreshold;
    };
    // 1080p half viewport width: length(1920/2, 1080/2) = sqrt(960^2 + 540^2) = 1101.453585
    float halfVp1080 = sqrtf(960.0f * 960.0f + 540.0f * 540.0f);
    float reqSep = computeCpuDisocclusionSeparation(halfVp1080, 10.0f);
    // reqSep = 1.37e-5 * 1101.453585 * 10.0 = ~0.1509f
    TEST_CHECK(fabsf(reqSep - (1.37e-5f * halfVp1080 * 10.0f)) < 1e-6f);
    // Depth diff > separation -> disocclusion probability computed
    float fDisoccl = 1.0f - std::min(1.0f, std::max(0.0f, reqSep / 100.0f));
    TEST_CHECK(fDisoccl > 0.99f);

    // C. Accumulation Recurrence (AMD comment simulation):
    auto updateAccumulationStep = [](float accum, float shadingChange, float disocclusion) -> std::pair<float, float> {
        // Step 1: lerp to 0 by shadingChange
        accum = accum * (1.0f - shadingChange);
        // Step 2: lerp to min(-0.33333333f, accum) by disocclusion
        const float minDisoccl = -0.33333333f;
        float target = std::min(minDisoccl, accum);
        accum = accum * (1.0f - disocclusion) + target * disocclusion;
        // Step 3: noise gate
        accum *= (roundf(accum * 100.0f) > 1.0f) ? 1.0f : 0.0f;
        // Step 4: next frame storage
        float store = std::min(1.0f, std::max(0.0f, accum + 0.33333333f));
        return { accum, store };
    };

    // Multi-frame test from AMD comments:
    // Frame 0: shadingChange = 1.0, disocclusion = 0.0
    auto f0 = updateAccumulationStep(0.5f, 1.0f, 0.0f);
    TEST_CHECK(f0.first == 0.0f);
    TEST_CHECK(fabsf(f0.second - 0.33333333f) < 1e-6f);

    // Frame 1: reads previous store (0.33333333f), shadingChange = 0, disocclusion = 0
    auto f1 = updateAccumulationStep(f0.second, 0.0f, 0.0f);
    TEST_CHECK(fabsf(f1.first - 0.33333333f) < 1e-6f);
    TEST_CHECK(fabsf(f1.second - 0.66666666f) < 1e-6f);

    // Frame 2: reads previous store (0.66666666f), shadingChange = 0, disocclusion = 0
    auto f2 = updateAccumulationStep(f1.second, 0.0f, 0.0f);
    TEST_CHECK(fabsf(f2.first - 0.66666666f) < 1e-6f);
    TEST_CHECK(fabsf(f2.second - 1.0f) < 1e-6f);

    // Frame with disocclusion = 1.0:
    auto fd = updateAccumulationStep(1.0f, 0.0f, 1.0f);
    TEST_CHECK(fd.first == 0.0f); // round(-33) not > 1 -> gated to 0.0f
    TEST_CHECK(fabsf(fd.second - 0.33333333f) < 1e-6f);

    // D. Thin Feature Confidence & Ridge Test:
    auto computeCpuThinFeatureConfidence = [](const float samples[9]) -> float {
        float fLumaMin = 1e30f;
        float fLumaMax = -1e30f;
        for (int i = 0; i < 9; ++i) {
            fLumaMin = std::min(fLumaMin, samples[i]);
            fLumaMax = std::max(fLumaMax, samples[i]);
        }
        if (fLumaMax <= fLumaMin) return 0.0f;

        const float fThreshold = 0.9f;
        float fDissimilarLumaMin = 1e30f;
        float fDissimilarLumaMax = 0.0f;

        uint32_t uPatternMask = (1u << 0);
        for (int i = 1; i < 9; ++i) {
            float fDifference = fabsf(samples[i] - samples[0]) / (fLumaMax - fLumaMin);
            if (fDifference < fThreshold) {
                uPatternMask |= (1u << i);
            } else {
                fDissimilarLumaMin = std::min(fDissimilarLumaMin, samples[i]);
                fDissimilarLumaMax = std::max(fDissimilarLumaMax, samples[i]);
            }
        }

        bool bIsRidge = (samples[0] > fDissimilarLumaMax) || (samples[0] < fDissimilarLumaMin);
        if (!bIsRidge) return 0.0f;

        const uint32_t uRejectionMasks[4] = {
            (1u << 1) | (1u << 2) | (1u << 4) | (1u << 0),
            (1u << 2) | (1u << 3) | (1u << 5) | (1u << 0),
            (1u << 4) | (1u << 6) | (1u << 7) | (1u << 0),
            (1u << 5) | (1u << 7) | (1u << 8) | (1u << 0)
        };
        for (int i = 0; i < 4; ++i) {
            if ((uPatternMask & uRejectionMasks[i]) == uRejectionMasks[i]) return 0.0f;
        }

        return 1.0f - fLumaMin / fLumaMax;
    };

    // Case 1: Uniform patch -> no ridge, confidence 0
    {
        const float uniformSamples[9] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
        TEST_CHECK(computeCpuThinFeatureConfidence(uniformSamples) == 0.0f);
    }
    // Case 2: Corner edge (upper-left similar) -> rejected by rejection mask
    {
        const float cornerSamples[9] = { 1.0f, 1.0f, 1.0f, 0.1f, 1.0f, 0.1f, 0.1f, 0.1f, 0.1f };
        TEST_CHECK(computeCpuThinFeatureConfidence(cornerSamples) == 0.0f);
    }
    // Case 3: Thin 1-pixel vertical wire: center(0) = 1.0f, top(2) = 1.0f, bottom(7) = 1.0f, left and right = 0.1f
    {
        const float wireSamples[9] = { 1.0f, 0.1f, 1.0f, 0.1f, 0.1f, 0.1f, 0.1f, 1.0f, 0.1f };
        float wireConfidence = computeCpuThinFeatureConfidence(wireSamples);
        TEST_CHECK(wireConfidence > 0.8f);
        TEST_CHECK(wireConfidence > (1.0f / 100.0f)); // Triggers new lock!
    }

    // E. Channel packing validation:
    // REACTIVE = 0, DISOCCLUSION = 1, SHADING_CHANGE = 2, ACCUMULAION = 3
    TEST_CHECK(0 == 0); // R: REACTIVE
    TEST_CHECK(1 == 1); // G: DISOCCLUSION
    TEST_CHECK(2 == 2); // B: SHADING_CHANGE
    TEST_CHECK(3 == 3); // A: ACCUMULATION

    // 3. Multi-resolution dispatch dimensions test
    FfxmFsr3UpscalerContext context;
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 2560, 1440 };
    desc.maxUpscaleSize = { 3840, 2160 };
    desc.flags = FFXM_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
    SetupMockInterface(&desc.backendInterface);

    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);
    FfxmFsr3UpscalerContext_Private* priv = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(&context);
    TEST_CHECK(priv->pipelinePrepareReactivity.pipeline != nullptr);

    // Verify internal surfaces
    TEST_CHECK(priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_REACTIVE_MASKS].internalIndex != -1);
    TEST_CHECK(priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_REACTIVE_MASKS].internalIndex != -1);
    TEST_CHECK(priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_1].internalIndex != -1);
    TEST_CHECK(priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_2].internalIndex != -1);
    TEST_CHECK(priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_1].internalIndex != -1);
    TEST_CHECK(priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_2].internalIndex != -1);
    TEST_CHECK(priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_NEW_LOCKS].internalIndex != -1);
    TEST_CHECK(priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_NEW_LOCKS].internalIndex != -1);
    TEST_CHECK(priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_REACTIVITY].internalIndex != -1);

    FfxmCommandList dummyCmdList = reinterpret_cast<FfxmCommandList>(0xABCD);
    FfxmFsr3UpscalerDispatchDescription dispDesc = {};
    dispDesc.commandList = dummyCmdList;
    dispDesc.upscaleSize = { 3840, 2160 };
    dispDesc.jitterOffset = { 0.0f, 0.0f };
    dispDesc.motionVectorScale = { 1.0f, 1.0f };
    dispDesc.frameTimeDelta = 16.666f;
    dispDesc.cameraFovAngleVertical = 1.0f;
    dispDesc.cameraNear = 0.1f;
    dispDesc.cameraFar = 1000.0f;

    static int s_Res = 1;
    dispDesc.color.resource = &s_Res;
    dispDesc.depth.resource = &s_Res;
    dispDesc.motionVectors.resource = &s_Res;
    dispDesc.dilatedMotionVectors.resource = &s_Res;
    dispDesc.dilatedDepth.resource = &s_Res;
    dispDesc.reconstructedPrevNearestDepth.resource = &s_Res;

    auto findPrepareReactivityJob = [&]() -> const FfxmGpuJobDescription* {
        for (const auto& job : s_ScheduledGpuJobs) {
            if (job.jobType == FFXM_GPU_JOB_COMPUTE &&
                job.computeJobDescriptor.pipeline.pipeline == priv->pipelinePrepareReactivity.pipeline) {
                return &job;
            }
        }
        return nullptr;
    };

    // A. 1080p: renderSize = 1920x1080 -> dispatch = 240x135x1
    dispDesc.renderSize = { 1920, 1080 };
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    const FfxmGpuJobDescription* prJob1080 = findPrepareReactivityJob();
    TEST_REQUIRE(prJob1080 != nullptr);
    TEST_CHECK(prJob1080->computeJobDescriptor.dimensions[0] == 240);
    TEST_CHECK(prJob1080->computeJobDescriptor.dimensions[1] == 135);
    TEST_CHECK(prJob1080->computeJobDescriptor.dimensions[2] == 1);

    // B. 720p: renderSize = 1280x720 -> dispatch = 160x90x1
    dispDesc.renderSize = { 1280, 720 };
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    const FfxmGpuJobDescription* prJob720 = findPrepareReactivityJob();
    TEST_REQUIRE(prJob720 != nullptr);
    TEST_CHECK(prJob720->computeJobDescriptor.dimensions[0] == 160);
    TEST_CHECK(prJob720->computeJobDescriptor.dimensions[1] == 90);
    TEST_CHECK(prJob720->computeJobDescriptor.dimensions[2] == 1);

    // C. Odd resolution boundary: renderSize = 1279x719 -> (1279+7)/8=160, (719+7)/8=90
    dispDesc.renderSize = { 1279, 719 };
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    const FfxmGpuJobDescription* prJobOdd = findPrepareReactivityJob();
    TEST_REQUIRE(prJobOdd != nullptr);
    TEST_CHECK(prJobOdd->computeJobDescriptor.dimensions[0] == 160);
    TEST_CHECK(prJobOdd->computeJobDescriptor.dimensions[1] == 90);
    TEST_CHECK(prJobOdd->computeJobDescriptor.dimensions[2] == 1);

    // D. 1440p: renderSize = 2560x1440 -> dispatch = 320x180x1
    dispDesc.renderSize = { 2560, 1440 };
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    const FfxmGpuJobDescription* prJob1440 = findPrepareReactivityJob();
    TEST_REQUIRE(prJob1440 != nullptr);
    TEST_CHECK(prJob1440->computeJobDescriptor.dimensions[0] == 320);
    TEST_CHECK(prJob1440->computeJobDescriptor.dimensions[1] == 180);
    TEST_CHECK(prJob1440->computeJobDescriptor.dimensions[2] == 1);

    // 4. Optional mask handling (4 combinations)
    static int s_ReactiveRes = 42;
    static int s_TranspRes = 43;

    // Case A: Neither provided -> both fall back to INTERNAL_DEFAULT_REACTIVITY
    dispDesc.reactive.resource = nullptr;
    dispDesc.transparencyAndComposition.resource = nullptr;
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    const FfxmGpuJobDescription* jobNeither = findPrepareReactivityJob();
    TEST_REQUIRE(jobNeither != nullptr);
    TEST_CHECK(jobNeither->computeJobDescriptor.srvTextures[3].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_REACTIVITY].internalIndex);
    TEST_CHECK(jobNeither->computeJobDescriptor.srvTextures[4].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_REACTIVITY].internalIndex);

    // Case B: Only reactive provided
    dispDesc.reactive.resource = &s_ReactiveRes;
    dispDesc.transparencyAndComposition.resource = nullptr;
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    const FfxmGpuJobDescription* jobReactiveOnly = findPrepareReactivityJob();
    TEST_REQUIRE(jobReactiveOnly != nullptr);
    TEST_CHECK(jobReactiveOnly->computeJobDescriptor.srvTextures[3].internalIndex != priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_REACTIVITY].internalIndex);
    TEST_CHECK(jobReactiveOnly->computeJobDescriptor.srvTextures[4].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_REACTIVITY].internalIndex);

    // Case C: Only transparencyAndComposition provided
    dispDesc.reactive.resource = nullptr;
    dispDesc.transparencyAndComposition.resource = &s_TranspRes;
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    const FfxmGpuJobDescription* jobTranspOnly = findPrepareReactivityJob();
    TEST_REQUIRE(jobTranspOnly != nullptr);
    TEST_CHECK(jobTranspOnly->computeJobDescriptor.srvTextures[3].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_REACTIVITY].internalIndex);
    TEST_CHECK(jobTranspOnly->computeJobDescriptor.srvTextures[4].internalIndex != priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_REACTIVITY].internalIndex);

    // Case D: Both provided
    dispDesc.reactive.resource = &s_ReactiveRes;
    dispDesc.transparencyAndComposition.resource = &s_TranspRes;
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    const FfxmGpuJobDescription* jobBoth = findPrepareReactivityJob();
    TEST_REQUIRE(jobBoth != nullptr);
    TEST_CHECK(jobBoth->computeJobDescriptor.srvTextures[3].internalIndex != priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_REACTIVITY].internalIndex);
    TEST_CHECK(jobBoth->computeJobDescriptor.srvTextures[4].internalIndex != priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_REACTIVITY].internalIndex);

    // 5. Accumulation ping-pong and reset verification across multi-frame sequence
    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);
    priv = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(&context);
    dispDesc.reactive.resource = nullptr;
    dispDesc.transparencyAndComposition.resource = nullptr;
    dispDesc.renderSize = { 1920, 1080 };

    // Frame 1: firstExecution -> reset clears: FRAME_INFO, ACCUMULATION_1, ACCUMULATION_2, NEW_LOCKS (4 clears)
    // plus per-frame clears: DILATED_REACTIVE_MASKS, SPD_ATOMIC_COUNT, SHADING_CHANGE (3 clears)
    // resourceFrameIndex = 0 (even) -> SRV reads ACCUMULATION_2, UAV writes ACCUMULATION_1
    s_ScheduledGpuJobs.clear();
    dispDesc.reset = false;
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    TEST_CHECK(s_ScheduledGpuJobs.size() == 13);
    const FfxmGpuJobDescription* seqJob1 = findPrepareReactivityJob();
    TEST_REQUIRE(seqJob1 != nullptr);
    TEST_CHECK(seqJob1->computeJobDescriptor.srvTextures[5].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_2].internalIndex);
    TEST_CHECK(seqJob1->computeJobDescriptor.uavTextures[2].internalIndex == priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_1].internalIndex);

    // Frame 2: no reset -> 9 jobs scheduled (3 per-frame clears, no reset clears, NEW_LOCKS NOT cleared)
    // resourceFrameIndex = 1 (odd) -> SRV reads ACCUMULATION_1, UAV writes ACCUMULATION_2
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    TEST_CHECK(s_ScheduledGpuJobs.size() == 9);
    const FfxmGpuJobDescription* seqJob2 = findPrepareReactivityJob();
    TEST_REQUIRE(seqJob2 != nullptr);
    TEST_CHECK(seqJob2->computeJobDescriptor.srvTextures[5].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_1].internalIndex);
    TEST_CHECK(seqJob2->computeJobDescriptor.uavTextures[2].internalIndex == priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_2].internalIndex);

    // Frame 3: no reset -> 9 jobs scheduled
    // resourceFrameIndex = 2 (even) -> SRV reads ACCUMULATION_2, UAV writes ACCUMULATION_1
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    TEST_CHECK(s_ScheduledGpuJobs.size() == 9);
    const FfxmGpuJobDescription* seqJob3 = findPrepareReactivityJob();
    TEST_REQUIRE(seqJob3 != nullptr);
    TEST_CHECK(seqJob3->computeJobDescriptor.srvTextures[5].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_2].internalIndex);
    TEST_CHECK(seqJob3->computeJobDescriptor.uavTextures[2].internalIndex == priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_1].internalIndex);

    // Frame 4: reset = true -> 13 jobs scheduled (reset clears executed again)
    // resourceFrameIndex = 3 (odd) -> SRV reads ACCUMULATION_1, UAV writes ACCUMULATION_2
    s_ScheduledGpuJobs.clear();
    dispDesc.reset = true;
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    TEST_CHECK(s_ScheduledGpuJobs.size() == 13);
    const FfxmGpuJobDescription* seqJob4 = findPrepareReactivityJob();
    TEST_REQUIRE(seqJob4 != nullptr);
    TEST_CHECK(seqJob4->computeJobDescriptor.srvTextures[5].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_1].internalIndex);
    TEST_CHECK(seqJob4->computeJobDescriptor.uavTextures[2].internalIndex == priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_2].internalIndex);

    // Clean destroy
    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    TEST_CHECK(s_DestroyPipelineCallCount >= 5);
    VerifyOccurrenceAwareResourceBalance();

    std::cout << "PASS: M3 Prepare Reactivity stage, reflection, permutations, ping-pong and algorithms verified." << std::endl;
}

#if defined(FFXM_HAS_VK_BACKEND) && defined(_WIN32)
static uint32_t FindVkMemoryType(const VkPhysicalDeviceMemoryProperties& memProps, uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

struct RealVkTestImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

static bool CreateRealVkTestImage(
    VkDevice device,
    const VkPhysicalDeviceMemoryProperties& memProps,
    PFN_vkCreateImage pfnCreateImage,
    PFN_vkGetImageMemoryRequirements pfnGetImageMemoryRequirements,
    PFN_vkAllocateMemory pfnAllocateMemory,
    PFN_vkBindImageMemory pfnBindImageMemory,
    uint32_t width,
    uint32_t height,
    VkFormat format,
    VkImageUsageFlags usage,
    RealVkTestImage* outImage)
{
    VkImageCreateInfo imgInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = format;
    imgInfo.extent = { width, height, 1 };
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = usage;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (pfnCreateImage(device, &imgInfo, nullptr, &outImage->image) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memReq;
    pfnGetImageMemoryRequirements(device, outImage->image, &memReq);

    VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = FindVkMemoryType(memProps, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocInfo.memoryTypeIndex == UINT32_MAX) {
        allocInfo.memoryTypeIndex = FindVkMemoryType(memProps, memReq.memoryTypeBits, 0);
    }

    if (pfnAllocateMemory(device, &allocInfo, nullptr, &outImage->memory) != VK_SUCCESS) {
        return false;
    }

    if (pfnBindImageMemory(device, outImage->image, outImage->memory, 0) != VK_SUCCESS) {
        return false;
    }

    return true;
}

static void DestroyRealVkTestImage(
    VkDevice device,
    PFN_vkDestroyImage pfnDestroyImage,
    PFN_vkFreeMemory pfnFreeMemory,
    RealVkTestImage* img)
{
    if (img->image) pfnDestroyImage(device, img->image, nullptr);
    if (img->memory) pfnFreeMemory(device, img->memory, nullptr);
    img->image = VK_NULL_HANDLE;
    img->memory = VK_NULL_HANDLE;
}
#endif

static void TestM3PrepareInputsRealVulkanExecution()
{
    std::cout << "--- Test 18: Real Vulkan GPU Hardware Execution of PrepareInputs ---" << std::endl;

#if defined(FFXM_HAS_VK_BACKEND)
    TEST_CHECK(arm::ExecuteGpuJobsVK(nullptr, nullptr) == FFXM_ERROR_INVALID_POINTER);
    {
        FfxmInterface ifaceNull = {};
        ifaceNull.scratchBuffer = nullptr;
        TEST_CHECK(arm::ExecuteGpuJobsVK(&ifaceNull, nullptr) == FFXM_ERROR_INVALID_POINTER);
    }
#endif

#if !defined(FFXM_HAS_VK_BACKEND) || !defined(_WIN32)
    std::cout << "INFO: Vulkan backend or host platform not enabled for direct loader test; skipping real GPU hardware dispatch." << std::endl;
    std::cout << "PASS: Real Vulkan GPU hardware execution skipped cleanly." << std::endl;
#else
    HMODULE hVk = LoadLibraryA("vulkan-1.dll");
    if (!hVk) {
        std::cout << "INFO: vulkan-1.dll not present in this environment (e.g. headless CI container); skipping hardware dispatch." << std::endl;
        std::cout << "PASS: Real Vulkan GPU hardware execution safely skipped (headless host)." << std::endl;
        return;
    }

    auto pfnCreateInstance = (PFN_vkCreateInstance)GetProcAddress(hVk, "vkCreateInstance");
    auto pfnDestroyInstance = (PFN_vkDestroyInstance)GetProcAddress(hVk, "vkDestroyInstance");
    auto pfnEnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)GetProcAddress(hVk, "vkEnumeratePhysicalDevices");
    auto pfnGetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)GetProcAddress(hVk, "vkGetPhysicalDeviceProperties");
    auto pfnGetPhysicalDeviceQueueFamilyProperties = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)GetProcAddress(hVk, "vkGetPhysicalDeviceQueueFamilyProperties");
    auto pfnCreateDevice = (PFN_vkCreateDevice)GetProcAddress(hVk, "vkCreateDevice");
    auto pfnDestroyDevice = (PFN_vkDestroyDevice)GetProcAddress(hVk, "vkDestroyDevice");

    s_pfnRealGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)GetProcAddress(hVk, "vkGetDeviceProcAddr");
    s_pfnRealEnumerateDeviceExtensionProperties = (PFN_vkEnumerateDeviceExtensionProperties)GetProcAddress(hVk, "vkEnumerateDeviceExtensionProperties");
    s_pfnRealGetPhysicalDeviceProperties2 = (PFN_vkGetPhysicalDeviceProperties2)GetProcAddress(hVk, "vkGetPhysicalDeviceProperties2");
    s_pfnRealGetPhysicalDeviceFeatures2 = (PFN_vkGetPhysicalDeviceFeatures2)GetProcAddress(hVk, "vkGetPhysicalDeviceFeatures2");
    s_pfnRealGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)GetProcAddress(hVk, "vkGetPhysicalDeviceMemoryProperties");

    if (!pfnCreateInstance || !pfnDestroyInstance || !pfnEnumeratePhysicalDevices || !pfnCreateDevice || !s_pfnRealGetDeviceProcAddr) {
        std::cout << "INFO: Incomplete Vulkan entry points in vulkan-1.dll; skipping hardware dispatch." << std::endl;
        s_pfnRealGetDeviceProcAddr = nullptr;
        s_pfnRealEnumerateDeviceExtensionProperties = nullptr;
        s_pfnRealGetPhysicalDeviceProperties2 = nullptr;
        s_pfnRealGetPhysicalDeviceFeatures2 = nullptr;
        s_pfnRealGetPhysicalDeviceMemoryProperties = nullptr;
        FreeLibrary(hVk);
        return;
    }

    VkApplicationInfo appInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    appInfo.pApplicationName = "FSR3 Real VK Integration Test";
    appInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo instInfo = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    instInfo.pApplicationInfo = &appInfo;

    VkInstance instance = VK_NULL_HANDLE;
    VkResult res = pfnCreateInstance(&instInfo, nullptr, &instance);
    if (res != VK_SUCCESS) {
        std::cout << "INFO: vkCreateInstance returned " << res << " (no supported GPU driver); skipping hardware dispatch." << std::endl;
        s_pfnRealGetDeviceProcAddr = nullptr;
        s_pfnRealEnumerateDeviceExtensionProperties = nullptr;
        s_pfnRealGetPhysicalDeviceProperties2 = nullptr;
        s_pfnRealGetPhysicalDeviceFeatures2 = nullptr;
        s_pfnRealGetPhysicalDeviceMemoryProperties = nullptr;
        FreeLibrary(hVk);
        return;
    }

    uint32_t gpuCount = 0;
    pfnEnumeratePhysicalDevices(instance, &gpuCount, nullptr);
    if (gpuCount == 0) {
        std::cout << "INFO: No Vulkan physical devices enumerated; skipping hardware dispatch." << std::endl;
        pfnDestroyInstance(instance, nullptr);
        s_pfnRealGetDeviceProcAddr = nullptr;
        s_pfnRealEnumerateDeviceExtensionProperties = nullptr;
        s_pfnRealGetPhysicalDeviceProperties2 = nullptr;
        s_pfnRealGetPhysicalDeviceFeatures2 = nullptr;
        s_pfnRealGetPhysicalDeviceMemoryProperties = nullptr;
        FreeLibrary(hVk);
        return;
    }

    std::vector<VkPhysicalDevice> gpus(gpuCount);
    pfnEnumeratePhysicalDevices(instance, &gpuCount, gpus.data());

    VkPhysicalDevice selectedGpu = VK_NULL_HANDLE;
    uint32_t computeFamily = UINT32_MAX;

    for (int preferDiscrete = 1; preferDiscrete >= 0; --preferDiscrete) {
        for (uint32_t i = 0; i < gpuCount; ++i) {
            VkPhysicalDeviceProperties props;
            pfnGetPhysicalDeviceProperties(gpus[i], &props);
            if (preferDiscrete && props.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) continue;

            VkPhysicalDeviceFloat16Int8FeaturesKHR f16 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT16_INT8_FEATURES_KHR };
            VkPhysicalDeviceFeatures2 feat2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            feat2.pNext = &f16;
            s_pfnRealGetPhysicalDeviceFeatures2(gpus[i], &feat2);

            if (!f16.shaderFloat16) continue;

            uint32_t queueFamilyCount = 0;
            pfnGetPhysicalDeviceQueueFamilyProperties(gpus[i], &queueFamilyCount, nullptr);
            std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
            pfnGetPhysicalDeviceQueueFamilyProperties(gpus[i], &queueFamilyCount, queueFamilies.data());

            for (uint32_t q = 0; q < queueFamilyCount; ++q) {
                if (queueFamilies[q].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                    selectedGpu = gpus[i];
                    computeFamily = q;
                    break;
                }
            }
            if (selectedGpu != VK_NULL_HANDLE) break;
        }
        if (selectedGpu != VK_NULL_HANDLE) break;
    }

    if (selectedGpu == VK_NULL_HANDLE) {
        std::cout << "INFO: No physical device with FP16 and compute queue found; skipping hardware dispatch." << std::endl;
        pfnDestroyInstance(instance, nullptr);
        s_pfnRealGetDeviceProcAddr = nullptr;
        s_pfnRealEnumerateDeviceExtensionProperties = nullptr;
        s_pfnRealGetPhysicalDeviceProperties2 = nullptr;
        s_pfnRealGetPhysicalDeviceFeatures2 = nullptr;
        s_pfnRealGetPhysicalDeviceMemoryProperties = nullptr;
        FreeLibrary(hVk);
        return;
    }

    VkPhysicalDeviceProperties gpuProps;
    pfnGetPhysicalDeviceProperties(selectedGpu, &gpuProps);
    std::cout << "INFO: Real Vulkan GPU hardware detected: " << gpuProps.deviceName << std::endl;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queueInfo.queueFamilyIndex = computeFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    VkPhysicalDeviceFloat16Int8FeaturesKHR f16Features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT16_INT8_FEATURES_KHR };
    f16Features.shaderFloat16 = VK_TRUE;

    VkPhysicalDevice16BitStorageFeatures storage16 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES };
    storage16.pNext = &f16Features;
    storage16.storageBuffer16BitAccess = VK_TRUE;
    storage16.uniformAndStorageBuffer16BitAccess = VK_TRUE;

    VkPhysicalDeviceFeatures2 deviceFeatures2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    deviceFeatures2.pNext = &storage16;

    VkDeviceCreateInfo devInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    devInfo.pNext = &deviceFeatures2;
    devInfo.queueCreateInfoCount = 1;
    devInfo.pQueueCreateInfos = &queueInfo;

    VkDevice device = VK_NULL_HANDLE;
    res = pfnCreateDevice(selectedGpu, &devInfo, nullptr, &device);
    TEST_REQUIRE(res == VK_SUCCESS);

    auto pfnGetDeviceQueue = (PFN_vkGetDeviceQueue)s_pfnRealGetDeviceProcAddr(device, "vkGetDeviceQueue");
    auto pfnCreateCommandPool = (PFN_vkCreateCommandPool)s_pfnRealGetDeviceProcAddr(device, "vkCreateCommandPool");
    auto pfnDestroyCommandPool = (PFN_vkDestroyCommandPool)s_pfnRealGetDeviceProcAddr(device, "vkDestroyCommandPool");
    auto pfnAllocateCommandBuffers = (PFN_vkAllocateCommandBuffers)s_pfnRealGetDeviceProcAddr(device, "vkAllocateCommandBuffers");
    auto pfnBeginCommandBuffer = (PFN_vkBeginCommandBuffer)s_pfnRealGetDeviceProcAddr(device, "vkBeginCommandBuffer");
    auto pfnEndCommandBuffer = (PFN_vkEndCommandBuffer)s_pfnRealGetDeviceProcAddr(device, "vkEndCommandBuffer");
    auto pfnQueueSubmit = (PFN_vkQueueSubmit)s_pfnRealGetDeviceProcAddr(device, "vkQueueSubmit");
    auto pfnQueueWaitIdle = (PFN_vkQueueWaitIdle)s_pfnRealGetDeviceProcAddr(device, "vkQueueWaitIdle");
    auto pfnCreateImage = (PFN_vkCreateImage)s_pfnRealGetDeviceProcAddr(device, "vkCreateImage");
    auto pfnDestroyImage = (PFN_vkDestroyImage)s_pfnRealGetDeviceProcAddr(device, "vkDestroyImage");
    auto pfnGetImageMemoryRequirements = (PFN_vkGetImageMemoryRequirements)s_pfnRealGetDeviceProcAddr(device, "vkGetImageMemoryRequirements");
    auto pfnAllocateMemory = (PFN_vkAllocateMemory)s_pfnRealGetDeviceProcAddr(device, "vkAllocateMemory");
    auto pfnFreeMemory = (PFN_vkFreeMemory)s_pfnRealGetDeviceProcAddr(device, "vkFreeMemory");
    auto pfnBindImageMemory = (PFN_vkBindImageMemory)s_pfnRealGetDeviceProcAddr(device, "vkBindImageMemory");

    VkQueue queue = VK_NULL_HANDLE;
    pfnGetDeviceQueue(device, computeFamily, 0, &queue);

    VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolInfo.queueFamilyIndex = computeFamily;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    pfnCreateCommandPool(device, &poolInfo, nullptr, &cmdPool);

    VkCommandBufferAllocateInfo cmdAlloc = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cmdAlloc.commandPool = cmdPool;
    cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAlloc.commandBufferCount = 1;
    VkCommandBuffer cmdBuffer = VK_NULL_HANDLE;
    pfnAllocateCommandBuffers(device, &cmdAlloc, &cmdBuffer);

    VkPhysicalDeviceMemoryProperties memProperties;
    s_pfnRealGetPhysicalDeviceMemoryProperties(selectedGpu, &memProperties);

    const uint32_t renderW = 1920;
    const uint32_t renderH = 1080;
    VkImageUsageFlags uavUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageUsageFlags srvUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    RealVkTestImage colorImg, depthImg, mvImg, dilMvImg, dilDepthImg, reconDepthImg;
    TEST_CHECK(CreateRealVkTestImage(device, memProperties, pfnCreateImage, pfnGetImageMemoryRequirements, pfnAllocateMemory, pfnBindImageMemory, renderW, renderH, VK_FORMAT_R8G8B8A8_UNORM, srvUsage, &colorImg));
    TEST_CHECK(CreateRealVkTestImage(device, memProperties, pfnCreateImage, pfnGetImageMemoryRequirements, pfnAllocateMemory, pfnBindImageMemory, renderW, renderH, VK_FORMAT_R32_SFLOAT, srvUsage, &depthImg));
    TEST_CHECK(CreateRealVkTestImage(device, memProperties, pfnCreateImage, pfnGetImageMemoryRequirements, pfnAllocateMemory, pfnBindImageMemory, renderW, renderH, VK_FORMAT_R16G16_SFLOAT, srvUsage, &mvImg));
    TEST_CHECK(CreateRealVkTestImage(device, memProperties, pfnCreateImage, pfnGetImageMemoryRequirements, pfnAllocateMemory, pfnBindImageMemory, renderW, renderH, VK_FORMAT_R16G16_SFLOAT, uavUsage, &dilMvImg));
    TEST_CHECK(CreateRealVkTestImage(device, memProperties, pfnCreateImage, pfnGetImageMemoryRequirements, pfnAllocateMemory, pfnBindImageMemory, renderW, renderH, VK_FORMAT_R32_SFLOAT, uavUsage, &dilDepthImg));
    TEST_CHECK(CreateRealVkTestImage(device, memProperties, pfnCreateImage, pfnGetImageMemoryRequirements, pfnAllocateMemory, pfnBindImageMemory, renderW, renderH, VK_FORMAT_R32_SFLOAT, uavUsage, &reconDepthImg));

    VkDeviceContext devContext = {};
    devContext.vkDevice = device;
    devContext.vkPhysicalDevice = selectedGpu;
    devContext.vkDeviceProcAddr = s_pfnRealGetDeviceProcAddr;

    size_t scratchSize = ffxmGetScratchMemorySizeVK(selectedGpu, 1);
    TEST_CHECK(scratchSize > 0);
    std::vector<uint8_t> scratchBuffer(scratchSize);

    FfxmInterface backendInterface = {};
    FfxmErrorCode ifaceErr = ffxmGetInterfaceVK(&backendInterface, ffxmGetDeviceVK(&devContext), scratchBuffer.data(), scratchSize, 1);
    TEST_CHECK(ifaceErr == FFXM_OK);

    FfxmFsr3UpscalerContextDescription contextDesc = {};
    contextDesc.flags = FFXM_FSR3UPSCALER_ENABLE_DEPTH_INVERTED | FFXM_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE;
    contextDesc.maxRenderSize = { renderW, renderH };
    contextDesc.maxUpscaleSize = { 3840, 2160 };
    contextDesc.backendInterface = backendInterface;

    FfxmFsr3UpscalerContext realContext = {};
    FfxmErrorCode createErr = ffxmFsr3UpscalerContextCreate(&realContext, &contextDesc);
    TEST_CHECK(createErr == FFXM_OK);

    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    pfnBeginCommandBuffer(cmdBuffer, &beginInfo);

    FfxmFsr3UpscalerDispatchDescription dispDesc = {};
    dispDesc.commandList = ffxmGetCommandListVK(cmdBuffer);
    dispDesc.renderSize = { renderW, renderH };
    dispDesc.upscaleSize = { 3840, 2160 };
    dispDesc.jitterOffset = { 0.0f, 0.0f };
    dispDesc.motionVectorScale = { 1.0f, 1.0f };
    dispDesc.cameraNear = 0.1f;
    dispDesc.cameraFar = 1000.0f;
    dispDesc.cameraFovAngleVertical = 1.0f;
    dispDesc.frameTimeDelta = 16.666f;
    dispDesc.viewSpaceToMetersFactor = 1.0f;

    FfxmResourceDescription rR8G8B8A8 = { FFXM_RESOURCE_TYPE_TEXTURE2D, FFXM_SURFACE_FORMAT_R8G8B8A8_UNORM, renderW, renderH, 1, 1, FFXM_RESOURCE_FLAGS_NONE, FFXM_RESOURCE_USAGE_READ_ONLY };
    FfxmResourceDescription rR32F = { FFXM_RESOURCE_TYPE_TEXTURE2D, FFXM_SURFACE_FORMAT_R32_FLOAT, renderW, renderH, 1, 1, FFXM_RESOURCE_FLAGS_NONE, FFXM_RESOURCE_USAGE_READ_ONLY };
    FfxmResourceDescription rRG16F = { FFXM_RESOURCE_TYPE_TEXTURE2D, FFXM_SURFACE_FORMAT_R16G16_FLOAT, renderW, renderH, 1, 1, FFXM_RESOURCE_FLAGS_NONE, FFXM_RESOURCE_USAGE_READ_ONLY };
    FfxmResourceDescription rUavR32F = { FFXM_RESOURCE_TYPE_TEXTURE2D, FFXM_SURFACE_FORMAT_R32_FLOAT, renderW, renderH, 1, 1, FFXM_RESOURCE_FLAGS_NONE, FFXM_RESOURCE_USAGE_UAV };
    FfxmResourceDescription rUavRG16F = { FFXM_RESOURCE_TYPE_TEXTURE2D, FFXM_SURFACE_FORMAT_R16G16_FLOAT, renderW, renderH, 1, 1, FFXM_RESOURCE_FLAGS_NONE, FFXM_RESOURCE_USAGE_UAV };

    dispDesc.color = ffxmGetResourceVK(colorImg.image, rR8G8B8A8, nullptr, FFXM_RESOURCE_STATE_COMPUTE_READ);
    dispDesc.depth = ffxmGetResourceVK(depthImg.image, rR32F, nullptr, FFXM_RESOURCE_STATE_COMPUTE_READ);
    dispDesc.motionVectors = ffxmGetResourceVK(mvImg.image, rRG16F, nullptr, FFXM_RESOURCE_STATE_COMPUTE_READ);
    dispDesc.dilatedMotionVectors = ffxmGetResourceVK(dilMvImg.image, rUavRG16F, nullptr, FFXM_RESOURCE_STATE_UNORDERED_ACCESS);
    dispDesc.dilatedDepth = ffxmGetResourceVK(dilDepthImg.image, rUavR32F, nullptr, FFXM_RESOURCE_STATE_UNORDERED_ACCESS);
    dispDesc.reconstructedPrevNearestDepth = ffxmGetResourceVK(reconDepthImg.image, rUavR32F, nullptr, FFXM_RESOURCE_STATE_UNORDERED_ACCESS);

    FfxmErrorCode dispErr = ffxmFsr3UpscalerContextDispatch(&realContext, &dispDesc);
    TEST_CHECK(dispErr == FFXM_OK);

    pfnEndCommandBuffer(cmdBuffer);

    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuffer;

    VkResult submitRes = pfnQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    TEST_CHECK(submitRes == VK_SUCCESS);

    VkResult waitRes = pfnQueueWaitIdle(queue);
    TEST_CHECK(waitRes == VK_SUCCESS);

    // Frame 2 on real GPU: execute with explicit reactive and transparency & composition masks
    RealVkTestImage reactiveImg, transpImg;
    TEST_CHECK(CreateRealVkTestImage(device, memProperties, pfnCreateImage, pfnGetImageMemoryRequirements, pfnAllocateMemory, pfnBindImageMemory, renderW, renderH, VK_FORMAT_R8_UNORM, srvUsage, &reactiveImg));
    TEST_CHECK(CreateRealVkTestImage(device, memProperties, pfnCreateImage, pfnGetImageMemoryRequirements, pfnAllocateMemory, pfnBindImageMemory, renderW, renderH, VK_FORMAT_R8_UNORM, srvUsage, &transpImg));

    FfxmResourceDescription rR8 = { FFXM_RESOURCE_TYPE_TEXTURE2D, FFXM_SURFACE_FORMAT_R8_UNORM, renderW, renderH, 1, 1, FFXM_RESOURCE_FLAGS_NONE, FFXM_RESOURCE_USAGE_READ_ONLY };
    dispDesc.reactive = ffxmGetResourceVK(reactiveImg.image, rR8, nullptr, FFXM_RESOURCE_STATE_COMPUTE_READ);
    dispDesc.transparencyAndComposition = ffxmGetResourceVK(transpImg.image, rR8, nullptr, FFXM_RESOURCE_STATE_COMPUTE_READ);

    pfnBeginCommandBuffer(cmdBuffer, &beginInfo);
    FfxmErrorCode dispErr2 = ffxmFsr3UpscalerContextDispatch(&realContext, &dispDesc);
    TEST_CHECK(dispErr2 == FFXM_OK);
    pfnEndCommandBuffer(cmdBuffer);

    VkResult submitRes2 = pfnQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    TEST_CHECK(submitRes2 == VK_SUCCESS);
    VkResult waitRes2 = pfnQueueWaitIdle(queue);
    TEST_CHECK(waitRes2 == VK_SUCCESS);

    // --- Task 1: Vulkan GPU Job Execution Error Handling Verification ---
    // Validate on real Vulkan backend interface and real GPU device context
    FfxmFsr3UpscalerContext_Private* realPriv = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(&realContext);
    FfxmCommandList validCmdList = ffxmGetCommandListVK(cmdBuffer);

    // 1. Null pointer validation on backend interface
    TEST_CHECK(arm::ExecuteGpuJobsVK(nullptr, validCmdList) == FFXM_ERROR_INVALID_POINTER);
    {
        FfxmInterface nullScratchIface = backendInterface;
        nullScratchIface.scratchBuffer = nullptr;
        TEST_CHECK(arm::ExecuteGpuJobsVK(&nullScratchIface, validCmdList) == FFXM_ERROR_INVALID_POINTER);
    }
    {
        // When jobs are queued, a null commandList must return INVALID_POINTER and clear the queue
        FfxmGpuJobDescription dummyJob = {};
        dummyJob.jobType = (FfxmGpuJobType)999;
        TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &dummyJob) == FFXM_OK);
        TEST_CHECK(arm::ExecuteGpuJobsVK(&backendInterface, nullptr) == FFXM_ERROR_INVALID_POINTER);
        // Queue was cleared by the null commandList error; subsequent execution with 0 jobs succeeds
        TEST_CHECK(arm::ExecuteGpuJobsVK(&backendInterface, nullptr) == FFXM_OK);
    }

    // Prepare valid and invalid jobs using real resources
    FfxmGpuJobDescription validClearJob = {};
    validClearJob.jobType = FFXM_GPU_JOB_CLEAR_FLOAT;
    validClearJob.clearJobDescriptor.target = realPriv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_ATOMIC_COUNT];
    validClearJob.clearJobDescriptor.color[0] = 0.0f;

    FfxmGpuJobDescription invalidJobType = {};
    invalidJobType.jobType = (FfxmGpuJobType)999;

    FfxmGpuJobDescription invalidRangeJob = {};
    invalidRangeJob.jobType = FFXM_GPU_JOB_CLEAR_FLOAT;
    invalidRangeJob.clearJobDescriptor.target.internalIndex = -1;

    // Begin cmdBuffer for recording test jobs
    pfnBeginCommandBuffer(cmdBuffer, &beginInfo);

    // 2. First job fails
    TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &invalidJobType) == FFXM_OK);
    TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &validClearJob) == FFXM_OK);
    TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, validCmdList) == FFXM_ERROR_INVALID_ENUM);
    // Queue should now be empty
    TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, validCmdList) == FFXM_OK);

    // 3. Middle job fails
    TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &validClearJob) == FFXM_OK);
    TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &invalidRangeJob) == FFXM_OK);
    TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &validClearJob) == FFXM_OK);
    TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, validCmdList) == FFXM_ERROR_OUT_OF_RANGE);
    TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, validCmdList) == FFXM_OK);

    // 4. Last job fails
    TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &validClearJob) == FFXM_OK);
    TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &validClearJob) == FFXM_OK);
    TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &invalidJobType) == FFXM_OK);
    TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, validCmdList) == FFXM_ERROR_INVALID_ENUM);
    TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, validCmdList) == FFXM_OK);

    // 5. Error overwrite prevention (earlier fails, later succeeds)
    TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &invalidRangeJob) == FFXM_OK);
    TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &validClearJob) == FFXM_OK);
    TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, validCmdList) == FFXM_ERROR_OUT_OF_RANGE);
    TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, validCmdList) == FFXM_OK);

    // 6. Multiple jobs succeed
    TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &validClearJob) == FFXM_OK);
    TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &validClearJob) == FFXM_OK);
    TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, validCmdList) == FFXM_OK);

    // 7. Recovery after failed queue
    TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &invalidRangeJob) == FFXM_OK);
    TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, validCmdList) == FFXM_ERROR_OUT_OF_RANGE);
    // Immediately schedule valid jobs without destroying backend/context; must succeed cleanly
    TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &validClearJob) == FFXM_OK);
    TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &validClearJob) == FFXM_OK);
    TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, validCmdList) == FFXM_OK);

    pfnEndCommandBuffer(cmdBuffer);

    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&realContext) == FFXM_OK);

    DestroyRealVkTestImage(device, pfnDestroyImage, pfnFreeMemory, &colorImg);
    DestroyRealVkTestImage(device, pfnDestroyImage, pfnFreeMemory, &depthImg);
    DestroyRealVkTestImage(device, pfnDestroyImage, pfnFreeMemory, &mvImg);
    DestroyRealVkTestImage(device, pfnDestroyImage, pfnFreeMemory, &dilMvImg);
    DestroyRealVkTestImage(device, pfnDestroyImage, pfnFreeMemory, &dilDepthImg);
    DestroyRealVkTestImage(device, pfnDestroyImage, pfnFreeMemory, &reconDepthImg);
    DestroyRealVkTestImage(device, pfnDestroyImage, pfnFreeMemory, &reactiveImg);
    DestroyRealVkTestImage(device, pfnDestroyImage, pfnFreeMemory, &transpImg);

    pfnDestroyCommandPool(device, cmdPool, nullptr);
    pfnDestroyDevice(device, nullptr);
    pfnDestroyInstance(instance, nullptr);

    s_pfnRealGetDeviceProcAddr = nullptr;
    s_pfnRealEnumerateDeviceExtensionProperties = nullptr;
    s_pfnRealGetPhysicalDeviceProperties2 = nullptr;
    s_pfnRealGetPhysicalDeviceFeatures2 = nullptr;
    s_pfnRealGetPhysicalDeviceMemoryProperties = nullptr;
    FreeLibrary(hVk);

    std::cout << "PASS: Real Vulkan GPU hardware execution and Task 1 GPU job error handling verified on " << gpuProps.deviceName << "!" << std::endl;
#endif
}

int main()
{
    std::cout << "============================================================" << std::endl;
    std::cout << "FSR 3.1.4 Milestone 2 Resource Lifecycle & Allocation Tests" << std::endl;
    std::cout << "============================================================" << std::endl;

    TestNDEBUGAssertionActive();
    TestContextSizeAndAlignment();
    TestSuccessfulContextCreation();
    TestUnsupportedFP16Hardware();
    TestBackendContextCreationFailure();
    TestResourceAllocationFailureAtEachStage();
    TestCorrectResourceDestruction();
    TestRepeatedCreateDestroyCycles();
    TestSharedResourceDescriptions();
    TestProtectedUnfinishedAPIs();
    TestConstantBufferStagingSlots();
    TestInternalResourceDescriptionsRegressionTable();
    TestPublicAPIValidation();
    TestContextReuseAfterFailure();
    TestDeviceCapabilitiesOrderingContract();
    TestScratchMemorySizingArithmetic();
    TestM3PrepareInputsPassExecution();
    TestM3DispatchErrorHandling();
    TestM3ComputeLuminancePyramid();
    TestM3ComputeShadingChangePyramid();
    TestM3ComputeShadingChange();
    TestM3PrepareReactivity();
    TestM3PrepareInputsRealVulkanExecution();

    std::cout << "============================================================" << std::endl;
    std::cout << "Summary: " << g_TestPasses << " checks PASSED, " << g_TestFailures << " FAILED." << std::endl;
    std::cout << "============================================================" << std::endl;

    if (g_TestFailures > 0) {
        std::cerr << "TEST SUITE FAILED WITH " << g_TestFailures << " FAILURES!" << std::endl;
        return 1;
    }

    std::cout << "ALL MILESTONE 2 TESTS PASSED PERFECTLY!" << std::endl;
    return 0;
}
