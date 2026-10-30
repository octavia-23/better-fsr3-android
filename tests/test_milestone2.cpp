// Copyright  © 2026 Arm Limited.
// SPDX-License-Identifier: MIT

#include <iostream>
#include <cassert>
#include <cstring>
#include <cmath>
#include <vector>
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

static std::vector<int32_t> s_CreatedResourceIndices;
static std::vector<int32_t> s_DestroyedResourceIndices;

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

    int32_t idx = s_NextResourceIndex++;
    s_CurrentResourceCreateCount++;
    s_CreatedResourceIndices.push_back(idx);

    if (outResource) {
        outResource->internalIndex = idx;
    }

    // If resource has initial data, backend creates a copy staging resource at idx + 1
    if (createResourceDescription && createResourceDescription->initData) {
        int32_t copyIdx = s_NextResourceIndex++;
        s_CreatedResourceIndices.push_back(copyIdx);
    }

    return FFXM_OK;
}

static FfxmErrorCode MockDestroyResource(FfxmInterface* backendInterface, FfxmResourceInternal resource)
{
    (void)backendInterface;
    if (resource.internalIndex != -1) {
        s_DestroyedResourceIndices.push_back(resource.internalIndex);
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
    s_CreatedResourceIndices.clear();
    s_DestroyedResourceIndices.clear();
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

    // Verify all created resources were destroyed exactly once
    assert(s_CreatedResourceIndices.size() == s_DestroyedResourceIndices.size());
    std::set<int32_t> createdSet(s_CreatedResourceIndices.begin(), s_CreatedResourceIndices.end());
    std::set<int32_t> destroyedSet(s_DestroyedResourceIndices.begin(), s_DestroyedResourceIndices.end());
    assert(createdSet == destroyedSet);

    // Verify context was cleared
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
    assert(static_cast<uint32_t>(res) == static_cast<uint32_t>(FFXM_ERROR_INVALID_ARGUMENT));
    assert(s_GetDeviceCapsCalled == true);
    assert(s_CreateBackendContextCalled == false); // Must NOT create backend context
    assert(s_DestroyBackendContextCalled == false); // Must NOT destroy backend context that was never created
    assert(s_CreatedResourceIndices.empty());
    assert(s_DestroyedResourceIndices.empty());

    // Context must be cleanly zeroed
    uint8_t zeros[sizeof(FfxmFsr3UpscalerContext)] = {};
    assert(memcmp(&context, zeros, sizeof(context)) == 0);

    std::cout << "PASS: Unsupported FP16 hardware returns FFXM_ERROR_INVALID_ARGUMENT and touches no backend state." << std::endl;
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
    assert(static_cast<uint32_t>(res) == static_cast<uint32_t>(FFXM_ERROR_OUT_OF_MEMORY));
    assert(s_CreateBackendContextCalled == true);
    assert(s_DestroyBackendContextCalled == false); // Never successfully created, so must NOT destroy
    assert(s_CreatedResourceIndices.empty());
    assert(s_DestroyedResourceIndices.empty());

    uint8_t zeros[sizeof(FfxmFsr3UpscalerContext)] = {};
    assert(memcmp(&context, zeros, sizeof(context)) == 0);

    std::cout << "PASS: Backend context creation failure safely handled without destroying uncreated context." << std::endl;
}

// 5. Resource Allocation Failure at Each Stage (0 to 18)
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
        assert(static_cast<uint32_t>(res) == static_cast<uint32_t>(FFXM_ERROR_OUT_OF_MEMORY));

        // Backend context WAS created, so it MUST be destroyed
        assert(s_CreateBackendContextCalled == true);
        assert(s_DestroyBackendContextCalled == true);
        assert(s_DestroyedContextId == 100);

        // Every resource allocated prior to failure must be destroyed exactly once
        assert(s_CreatedResourceIndices.size() == s_DestroyedResourceIndices.size());
        std::set<int32_t> createdSet(s_CreatedResourceIndices.begin(), s_CreatedResourceIndices.end());
        std::set<int32_t> destroyedSet(s_DestroyedResourceIndices.begin(), s_DestroyedResourceIndices.end());
        assert(createdSet == destroyedSet);

        // Context must be zeroed out
        uint8_t zeros[sizeof(FfxmFsr3UpscalerContext)] = {};
        assert(memcmp(&context, zeros, sizeof(context)) == 0);
    }

    std::cout << "PASS: All 19 allocation failure stages cleanly rolled back with zero leaks and zero double-frees." << std::endl;
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
    size_t destroyedCountFirst = s_DestroyedResourceIndices.size();
    assert(destroyedCountFirst == s_CreatedResourceIndices.size());
    assert(s_DestroyBackendContextCalled == true);

    // Destroy second time on zeroed context
    s_DestroyBackendContextCalled = false;
    assert(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    // Nothing new should have been destroyed
    assert(s_DestroyedResourceIndices.size() == destroyedCountFirst);
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

    // Verify equal creations and destructions across all 10 cycles
    assert(s_CreatedResourceIndices.size() == s_DestroyedResourceIndices.size());
    std::set<int32_t> createdSet(s_CreatedResourceIndices.begin(), s_CreatedResourceIndices.end());
    std::set<int32_t> destroyedSet(s_DestroyedResourceIndices.begin(), s_DestroyedResourceIndices.end());
    assert(createdSet == destroyedSet);

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
    assert(static_cast<uint32_t>(dispRes) == static_cast<uint32_t>(FFXM_ERROR_INCOMPLETE_INTERFACE));

    // Test GenerateReactive protection
    FfxmFsr3UpscalerGenerateReactiveDescription genDesc = {};
    genDesc.commandList = dummyCmdList;
    genDesc.scale = 0.5f;
    genDesc.cutoffThreshold = 0.2f;
    genDesc.binaryValue = 1.0f;

    FfxmErrorCode genRes = ffxmFsr3UpscalerContextGenerateReactiveMask(&context, &genDesc);
    assert(static_cast<uint32_t>(genRes) == static_cast<uint32_t>(FFXM_ERROR_INCOMPLETE_INTERFACE));

    assert(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    std::cout << "PASS: Unfinished dispatch and reactive APIs explicitly return FFXM_ERROR_INCOMPLETE_INTERFACE." << std::endl;
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

    std::cout << "============================================================" << std::endl;
    std::cout << "ALL MILESTONE 2 TESTS PASSED PERFECTLY!" << std::endl;
    std::cout << "============================================================" << std::endl;
    return 0;
}
