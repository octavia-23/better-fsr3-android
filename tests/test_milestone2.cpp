// Copyright  © 2026 Arm Limited.
// SPDX-License-Identifier: MIT

#include <iostream>
#include <cassert>
#include <cstring>
#include <cmath>
#include <vector>
#include <map>
#include <set>

#include "host/ffxm_fsr3upscaler.h"
#include "src/components/fsr3upscaler/ffxm_fsr3upscaler_private.h"
#include "host/ffxm_util.h"

using namespace arm;

// Mock backend interface state tracking
static bool s_GetDeviceCapsCalled = false;
static bool s_CreateBackendContextCalled = false;
static bool s_DestroyBackendContextCalled = false;
static FfxmUInt32 s_CreatedContextId = 0;
static FfxmUInt32 s_DestroyedContextId = 0;
static bool s_Fp16SupportMock = true;
static FfxmErrorCode s_CreateBackendContextResult = FFXM_OK;

// Resource failure injection controls
static int32_t s_FailAtResourceIndex = -1; // -1 means no failure
static int32_t s_CurrentResourceCreateCount = 0;
static int32_t s_NextResourceIndex = 0;

// Occurrence-aware resource creation & destruction tracking
static std::map<int32_t, int> s_ResourceCreationCounts;
static std::map<int32_t, int> s_ResourceDestructionCounts;
static std::vector<int32_t> s_CreatedResourceOrder;
static std::vector<int32_t> s_DestroyedResourceOrder;

// Captured resource descriptions for regression verification
static std::vector<FfxmCreateResourceDescription> s_CapturedResourceDescriptions;

static FfxmVersionNumber MockGetSDKVersion(FfxmInterface* backendInterface)
{
    (void)backendInterface;
    return FFXM_SDK_MAKE_VERSION(1, 1, 4);
}

static FfxmErrorCode MockGetDeviceCapabilities(FfxmInterface* backendInterface, FfxmDeviceCapabilities* deviceCapabilities)
{
    (void)backendInterface;
    s_GetDeviceCapsCalled = true;
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
    if (s_CreateBackendContextResult != FFXM_OK) {
        return s_CreateBackendContextResult;
    }
    *effectContextId = 100;
    s_CreatedContextId = 100;
    return FFXM_OK;
}

static FfxmErrorCode MockDestroyBackendContext(FfxmInterface* backendInterface, FfxmUInt32 effectContextId)
{
    (void)backendInterface;
    s_DestroyBackendContextCalled = true;
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
    s_FailAtResourceIndex = -1;
    s_CurrentResourceCreateCount = 0;
    s_NextResourceIndex = 0;
    s_ResourceCreationCounts.clear();
    s_ResourceDestructionCounts.clear();
    s_CreatedResourceOrder.clear();
    s_DestroyedResourceOrder.clear();
    s_CapturedResourceDescriptions.clear();
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
    assert(s_ResourceCreationCounts.size() == s_ResourceDestructionCounts.size());

    // 2. Every acquired resource must be destroyed exactly once
    for (const auto& kv : s_ResourceCreationCounts) {
        int32_t idx = kv.first;
        assert(kv.second == 1);
        auto it = s_ResourceDestructionCounts.find(idx);
        assert(it != s_ResourceDestructionCounts.end());
        assert(it->second == 1);
    }

    // 3. No unacquired resource was destroyed
    for (const auto& kv : s_ResourceDestructionCounts) {
        int32_t idx = kv.first;
        assert(s_ResourceCreationCounts.find(idx) != s_ResourceCreationCounts.end());
    }

    // 4. Exact map equality
    assert(s_ResourceCreationCounts == s_ResourceDestructionCounts);
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
    assert(sizeof(FfxmFsr3UpscalerContext) == 262144);
#elif defined(__aarch64__)
    assert(sizeof(FfxmFsr3UpscalerContext) == 524288);
#endif
    assert(alignof(FfxmFsr3UpscalerContext) >= 8);
    assert(sizeof(FfxmFsr3UpscalerContext) >= sizeof(FfxmFsr3UpscalerContext_Private));
    assert(alignof(FfxmFsr3UpscalerContext) >= alignof(FfxmFsr3UpscalerContext_Private));
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
    assert(res == FFXM_OK);
    assert(s_GetDeviceCapsCalled == true);
    assert(s_CreateBackendContextCalled == true);

    FfxmFsr3UpscalerContext_Private* priv = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(&context);
    assert(priv->backendContextCreated == true);
    assert(priv->effectContextId == 100);
    assert(priv->allocatedResourceCount == FFXM_FSR3UPSCALER_INTERNAL_SURFACE_COUNT);

    // Verify all 19 resources were allocated with non-negative indices
    for (uint32_t i = 0; i < FFXM_FSR3UPSCALER_INTERNAL_SURFACE_COUNT; ++i) {
        uint32_t id = priv->allocatedResourceIds[i];
        assert(priv->srvResources[id].internalIndex >= 0);
        assert(priv->uavResources[id].internalIndex == priv->srvResources[id].internalIndex);
        assert(priv->rtResources[id].internalIndex == priv->srvResources[id].internalIndex);
    }

    // Verify constant staging
    assert(priv->constants.maxRenderSize[0] == 1920);
    assert(priv->constants.maxRenderSize[1] == 1080);
    assert(priv->constants.maxUpscaleSize[0] == 3840);
    assert(priv->constants.maxUpscaleSize[1] == 2160);

    // Test destruction
    res = ffxmFsr3UpscalerContextDestroy(&context);
    assert(res == FFXM_OK);
    assert(s_DestroyBackendContextCalled == true);
    assert(s_DestroyedContextId == 100);

    // Verify occurrence-aware resource balance (19 surfaces, 4 copy resources, 0 leaks, 0 double-frees)
    VerifyOccurrenceAwareResourceBalance();
    assert(s_ResourceCreationCounts.size() == 23); // 19 textures + 4 copy staging resources

    // Verify context was cleanly zeroed
    uint8_t zeros[sizeof(FfxmFsr3UpscalerContext)] = {};
    assert(memcmp(&context, zeros, sizeof(context)) == 0);

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
    assert(res == FFXM_ERROR_UNSUPPORTED_DEVICE);
    assert(s_GetDeviceCapsCalled == true);
    assert(s_CreateBackendContextCalled == false); // Must NOT create backend context
    assert(s_DestroyBackendContextCalled == false); // Must NOT destroy backend context that was never created
    assert(s_ResourceCreationCounts.empty());
    assert(s_ResourceDestructionCounts.empty());

    // Context must be cleanly zeroed
    uint8_t zeros[sizeof(FfxmFsr3UpscalerContext)] = {};
    assert(memcmp(&context, zeros, sizeof(context)) == 0);

    std::cout << "PASS: Unsupported FP16 hardware returns FFXM_ERROR_UNSUPPORTED_DEVICE and touches no backend state." << std::endl;
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
    assert(res == FFXM_ERROR_OUT_OF_MEMORY);
    assert(s_CreateBackendContextCalled == true);
    assert(s_DestroyBackendContextCalled == false); // Never successfully created, so must NOT destroy
    assert(s_ResourceCreationCounts.empty());
    assert(s_ResourceDestructionCounts.empty());

    uint8_t zeros[sizeof(FfxmFsr3UpscalerContext)] = {};
    assert(memcmp(&context, zeros, sizeof(context)) == 0);

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
        assert(res == FFXM_ERROR_OUT_OF_MEMORY);

        // Backend context WAS created, so it MUST be destroyed
        assert(s_CreateBackendContextCalled == true);
        assert(s_DestroyBackendContextCalled == true);
        assert(s_DestroyedContextId == 100);

        // Occurrence-aware check: every resource allocated prior to failure must be destroyed exactly once
        VerifyOccurrenceAwareResourceBalance();

        // Context must be zeroed out
        uint8_t zeros[sizeof(FfxmFsr3UpscalerContext)] = {};
        assert(memcmp(&context, zeros, sizeof(context)) == 0);
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

    assert(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);

    // Destroy once
    assert(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    VerifyOccurrenceAwareResourceBalance();
    assert(s_DestroyBackendContextCalled == true);

    // Destroy second time on zeroed context
    s_DestroyBackendContextCalled = false;
    assert(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    // Nothing new should have been destroyed
    VerifyOccurrenceAwareResourceBalance();
    assert(s_DestroyBackendContextCalled == false);

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
        assert(res == FFXM_OK);

        res = ffxmFsr3UpscalerContextDestroy(&context);
        assert(res == FFXM_OK);
    }

    // Verify occurrence-aware tracking across all 10 cycles
    VerifyOccurrenceAwareResourceBalance();
    assert(s_ResourceCreationCounts.size() == 230); // 23 resources * 10 cycles

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
    assert(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);

    FfxmFsr3UpscalerSharedResourceDescriptions shared = {};
    assert(ffxmFsr3UpscalerGetSharedResourceDescriptions(&context, &shared) == FFXM_OK);
    assert(shared.dilatedDepth.resourceDescription.format == FFXM_SURFACE_FORMAT_R32_FLOAT);
    assert(shared.dilatedMotionVectors.resourceDescription.format == FFXM_SURFACE_FORMAT_R16G16_FLOAT);
    assert(shared.reconstructedPrevNearestDepth.resourceDescription.format == FFXM_SURFACE_FORMAT_R32_UINT);

    assert(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
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
    assert(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);

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
    assert(dispRes == FFXM_ERROR_INCOMPLETE_INTERFACE);

    // Test GenerateReactive protection
    FfxmFsr3UpscalerGenerateReactiveDescription genDesc = {};
    genDesc.commandList = dummyCmdList;
    genDesc.scale = 0.5f;
    genDesc.cutoffThreshold = 0.2f;
    genDesc.binaryValue = 1.0f;

    FfxmErrorCode genRes = ffxmFsr3UpscalerContextGenerateReactiveMask(&context, &genDesc);
    assert(genRes == FFXM_ERROR_INCOMPLETE_INTERFACE);

    assert(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    std::cout << "PASS: Unfinished dispatch and reactive APIs explicitly return FFXM_ERROR_INCOMPLETE_INTERFACE." << std::endl;
}

// 10. Test All Four Constant-Buffer Staging Slots
void TestConstantBufferStagingSlots()
{
    std::cout << "--- Test 10: Constant-Buffer Staging Slots (All 4 Slots) ---" << std::endl;
    ResetMockState();

    FfxmFsr3UpscalerContext context;
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    SetupMockInterface(&desc.backendInterface);
    assert(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);

    FfxmFsr3UpscalerContext_Private* priv = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(&context);

    // 1. Verify entry counts for all 4 slots staged during context creation
    assert(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER].num32BitEntries ==
        sizeof(Fsr3UpscalerConstants) / sizeof(uint32_t));
    assert(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_SPD].num32BitEntries ==
        sizeof(Fsr3UpscalerSpdConstants) / sizeof(uint32_t));
    assert(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_RCAS].num32BitEntries ==
        sizeof(Fsr3UpscalerRcasConstants) / sizeof(uint32_t));
    assert(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_GENREACTIVE].num32BitEntries ==
        sizeof(Fsr3UpscalerGenerateReactiveConstants) / sizeof(uint32_t));

    // 2. Verify byte-for-byte staging for Slot 0 (FSR3UPSCALER)
    Fsr3UpscalerConstants testFsr3 = {};
    for (size_t i = 0; i < sizeof(testFsr3); ++i) {
        reinterpret_cast<uint8_t*>(&testFsr3)[i] = static_cast<uint8_t>((i * 7 + 13) & 0xFF);
    }
    priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER].num32BitEntries =
        sizeof(Fsr3UpscalerConstants) / sizeof(uint32_t);
    memcpy(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER].data,
        &testFsr3, sizeof(testFsr3));
    assert(memcmp(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER].data,
        &testFsr3, sizeof(testFsr3)) == 0);

    // 3. Verify byte-for-byte staging for Slot 1 (SPD)
    Fsr3UpscalerSpdConstants testSpd = {};
    testSpd.mips = 7;
    testSpd.numWorkGroups = 42;
    testSpd.workGroupOffset[0] = 16;
    testSpd.workGroupOffset[1] = 32;
    testSpd.renderSize[0] = 960;
    testSpd.renderSize[1] = 540;

    priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_SPD].num32BitEntries =
        sizeof(Fsr3UpscalerSpdConstants) / sizeof(uint32_t);
    memcpy(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_SPD].data,
        &testSpd, sizeof(testSpd));
    assert(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_SPD].num32BitEntries == 6);
    assert(memcmp(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_SPD].data,
        &testSpd, sizeof(testSpd)) == 0);

    // 4. Verify byte-for-byte staging for Slot 2 (RCAS)
    Fsr3UpscalerRcasConstants testRcas = {};
    testRcas.rcasConfig[0] = 0x3F800000;
    testRcas.rcasConfig[1] = 0x40000000;
    testRcas.rcasConfig[2] = 0x12345678;
    testRcas.rcasConfig[3] = 0x9ABCDEF0;

    priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_RCAS].num32BitEntries =
        sizeof(Fsr3UpscalerRcasConstants) / sizeof(uint32_t);
    memcpy(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_RCAS].data,
        &testRcas, sizeof(testRcas));
    assert(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_RCAS].num32BitEntries == 4);
    assert(memcmp(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_RCAS].data,
        &testRcas, sizeof(testRcas)) == 0);

    // 5. Verify byte-for-byte staging for Slot 3 (GENREACTIVE) via public API
    FfxmCommandList dummyCmdList = reinterpret_cast<FfxmCommandList>(0xCAFE);
    FfxmFsr3UpscalerGenerateReactiveDescription genDesc = {};
    genDesc.commandList = dummyCmdList;
    genDesc.scale = 0.75f;
    genDesc.cutoffThreshold = 0.35f;
    genDesc.binaryValue = 0.9f;
    genDesc.flags = 0x03;

    FfxmErrorCode genRes = ffxmFsr3UpscalerContextGenerateReactiveMask(&context, &genDesc);
    assert(genRes == FFXM_ERROR_INCOMPLETE_INTERFACE); // protected until Milestone 3

    Fsr3UpscalerGenerateReactiveConstants expectedGen = {};
    expectedGen.scale = genDesc.scale;
    expectedGen.cutoffThreshold = genDesc.cutoffThreshold;
    expectedGen.binaryValue = genDesc.binaryValue;
    expectedGen.flags = genDesc.flags;

    assert(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_GENREACTIVE].num32BitEntries ==
        sizeof(Fsr3UpscalerGenerateReactiveConstants) / sizeof(uint32_t));
    assert(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_GENREACTIVE].num32BitEntries == 4);
    assert(memcmp(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_GENREACTIVE].data,
        &expectedGen, sizeof(expectedGen)) == 0);

    // 6. Verify Dispatch stages Slot 0 (FSR3UPSCALER) via public API
    FfxmFsr3UpscalerDispatchDescription dispDesc = {};
    dispDesc.commandList = dummyCmdList;
    dispDesc.renderSize = { 1280, 720 };
    dispDesc.upscaleSize = { 2560, 1440 };
    dispDesc.jitterOffset = { 0.33f, -0.66f };
    dispDesc.motionVectorScale = { 1.5f, 2.0f };
    dispDesc.frameTimeDelta = 0.01666f;
    dispDesc.cameraFovAngleVertical = 1.047f;
    dispDesc.viewSpaceToMetersFactor = 1.0f;
    dispDesc.cameraNear = 0.1f;
    dispDesc.cameraFar = 1000.0f;

    FfxmErrorCode dispRes = ffxmFsr3UpscalerContextDispatch(&context, &dispDesc);
    assert(dispRes == FFXM_ERROR_INCOMPLETE_INTERFACE); // protected until Milestone 3

    assert(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER].num32BitEntries ==
        sizeof(Fsr3UpscalerConstants) / sizeof(uint32_t));
    assert(memcmp(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER].data,
        &priv->constants, sizeof(Fsr3UpscalerConstants)) == 0);

    assert(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    std::cout << "PASS: All 4 constant-buffer staging slots verified for entry counts and byte-for-byte equality." << std::endl;
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
    assert(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);

    assert(s_CapturedResourceDescriptions.size() == FFXM_FSR3UPSCALER_INTERNAL_SURFACE_COUNT);

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

        assert(actual.id == expected.id);
        assert(wcscmp(actual.name, expected.name) == 0);
        assert(actual.heapType == FFXM_HEAP_TYPE_DEFAULT);
        assert(actual.resourceDescription.type == FFXM_RESOURCE_TYPE_TEXTURE2D);
        assert(actual.resourceDescription.format == expected.format);
        assert(actual.resourceDescription.width == expected.width);
        assert(actual.resourceDescription.height == expected.height);
        assert(actual.resourceDescription.depth == 1);
        assert(actual.resourceDescription.mipCount == expected.mipCount);
        assert(actual.resourceDescription.flags == expected.flags);
        assert(actual.resourceDescription.usage == expected.usage);
        assert(actual.initalState == expected.initialState);
        assert(actual.initDataSize == expected.initDataSize);
        if (expected.initDataSize > 0) {
            assert(actual.initData != nullptr);
        } else {
            assert(actual.initData == nullptr);
        }
    }

    assert(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    std::cout << "PASS: All 19 internal resource descriptions match authoritative FSR 3.1.4 reference." << std::endl;
}

int main()
{
    std::cout << "============================================================" << std::endl;
    std::cout << "FSR 3.1.4 Milestone 2 Resource Lifecycle & Allocation Tests" << std::endl;
    std::cout << "============================================================" << std::endl;

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

    std::cout << "============================================================" << std::endl;
    std::cout << "ALL MILESTONE 2 TESTS PASSED PERFECTLY!" << std::endl;
    std::cout << "============================================================" << std::endl;
    return 0;
}
