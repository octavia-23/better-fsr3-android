// Copyright  © 2026 Arm Limited.
// SPDX-License-Identifier: MIT

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

#if defined(FFXM_HAS_VK_BACKEND)
#include "host/backends/vk/ffxm_vk.h"

#if !defined(ANDROID)
// On host platforms when compiling tests without the full Vulkan SDK loader (using header-only Vulkan-Headers),
// provide stubs for Vulkan loader functions so test_milestone2 can link Arm_ASR_backend
// to directly test ffxmGetScratchMemorySizeVK and ffxmGetInterfaceVK.
extern "C" {
    VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(
        VkPhysicalDevice physicalDevice, const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties)
    {
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
        (void)device;
        (void)pName;
        return nullptr;
    }

    VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(
        VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2* pProperties)
    {
        (void)physicalDevice;
        (void)pProperties;
    }

    VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(
        VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2* pFeatures)
    {
        (void)physicalDevice;
        (void)pFeatures;
    }

    VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(
        VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties* pMemoryProperties)
    {
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
    if (outResource) {
        outResource->internalIndex = 999;
    }
    return FFXM_OK;
}

static FfxmResource MockGetResource(FfxmInterface* backendInterface, FfxmResourceInternal res)
{
    (void)backendInterface; (void)res;
    FfxmResource r = {};
    return r;
}

static FfxmErrorCode MockUnregisterResources(FfxmInterface* backendInterface, FfxmCommandList commandList, FfxmUInt32 effectContextId)
{
    (void)backendInterface; (void)commandList; (void)effectContextId;
    return FFXM_OK;
}

static FfxmResourceDescription MockGetResourceDescription(FfxmInterface* backendInterface, FfxmResourceInternal res)
{
    (void)backendInterface; (void)res;
    FfxmResourceDescription d = {};
    return d;
}

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
    (void)backendInterface; (void)effect; (void)pass; (void)qualityPreset;
    (void)permutationOptions; (void)pipelineDescription; (void)effectContextId; (void)outPipeline;
    return FFXM_OK;
}

static FfxmErrorCode MockDestroyPipeline(FfxmInterface* backendInterface, FfxmPipelineState* pipeline, FfxmUInt32 effectContextId)
{
    (void)backendInterface; (void)pipeline; (void)effectContextId;
    return FFXM_OK;
}

static FfxmErrorCode MockScheduleGpuJob(FfxmInterface* backendInterface, const FfxmGpuJobDescription* job)
{
    (void)backendInterface; (void)job;
    return FFXM_OK;
}

static FfxmErrorCode MockExecuteGpuJobs(FfxmInterface* backendInterface, FfxmCommandList commandList)
{
    (void)backendInterface; (void)commandList;
    return FFXM_OK;
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
    TEST_CHECK(dispRes == FFXM_ERROR_INCOMPLETE_INTERFACE);

    // Test GenerateReactive protection
    FfxmFsr3UpscalerGenerateReactiveDescription genDesc = {};
    genDesc.commandList = dummyCmdList;
    genDesc.scale = 0.5f;
    genDesc.cutoffThreshold = 0.2f;
    genDesc.binaryValue = 1.0f;

    FfxmErrorCode genRes = ffxmFsr3UpscalerContextGenerateReactiveMask(&context, &genDesc);
    TEST_CHECK(genRes == FFXM_ERROR_INCOMPLETE_INTERFACE);

    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    std::cout << "PASS: Unfinished dispatch and reactive APIs explicitly return FFXM_ERROR_INCOMPLETE_INTERFACE." << std::endl;
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
    TEST_CHECK(dispRes == FFXM_ERROR_INCOMPLETE_INTERFACE); // protected until Milestone 3

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

    // Verify Slot 1 (SPD) - computed via ffxSpdSetup
    uint32_t expDispatchThreadGroupCountXY[2];
    uint32_t expWorkGroupOffset[2];
    uint32_t expNumWorkGroupsAndMips[2];
    uint32_t expRectInfo[4] = { 0, 0, 1280, 720 };
    ffxSpdSetup(expDispatchThreadGroupCountXY, expWorkGroupOffset, expNumWorkGroupsAndMips, expRectInfo);

    Fsr3UpscalerSpdConstants expectedSpd = {};
    expectedSpd.mips = expNumWorkGroupsAndMips[1];
    expectedSpd.numWorkGroups = expNumWorkGroupsAndMips[0];
    expectedSpd.workGroupOffset[0] = expWorkGroupOffset[0];
    expectedSpd.workGroupOffset[1] = expWorkGroupOffset[1];
    expectedSpd.renderSize[0] = 1280;
    expectedSpd.renderSize[1] = 720;

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

    size_t large64BitContextSize = ffxmGetScratchMemorySizeVK(nullptr, (static_cast<size_t>(1) << 33));
    TEST_CHECK(large64BitContextSize == 0);

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
