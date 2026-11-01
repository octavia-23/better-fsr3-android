#include "ffxm_fsr3upscaler_luma_pyramid_pass_1401dc4626c7b4af721b6eab8b9e1622.h"

typedef union ffxm_fsr3upscaler_luma_pyramid_pass_PermutationKey {
    struct {
        uint32_t FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE : 1;
        uint32_t FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT : 1;
        uint32_t FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS : 1;
        uint32_t FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS : 1;
        uint32_t FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH : 1;
        uint32_t FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING : 1;
    };
    uint32_t index;
} ffxm_fsr3upscaler_luma_pyramid_pass_PermutationKey;

typedef struct ffxm_fsr3upscaler_luma_pyramid_pass_PermutationInfo {
    const uint32_t       blobSize;
    const unsigned char* blobData;


    const uint32_t  numConstantBuffers;
    const char**    constantBufferNames;
    const uint32_t* constantBufferBindings;
    const uint32_t* constantBufferCounts;
    const uint32_t* constantBufferSpaces;

    const uint32_t  numSRVTextures;
    const char**    srvTextureNames;
    const uint32_t* srvTextureBindings;
    const uint32_t* srvTextureCounts;
    const uint32_t* srvTextureSpaces;

    const uint32_t  numUAVTextures;
    const char**    uavTextureNames;
    const uint32_t* uavTextureBindings;
    const uint32_t* uavTextureCounts;
    const uint32_t* uavTextureSpaces;

    const uint32_t  numSRVBuffers;
    const char**    srvBufferNames;
    const uint32_t* srvBufferBindings;
    const uint32_t* srvBufferCounts;
    const uint32_t* srvBufferSpaces;

    const uint32_t  numUAVBuffers;
    const char**    uavBufferNames;
    const uint32_t* uavBufferBindings;
    const uint32_t* uavBufferCounts;
    const uint32_t* uavBufferSpaces;

    const uint32_t  numSamplers;
    const char**    samplerNames;
    const uint32_t* samplerBindings;
    const uint32_t* samplerCounts;
    const uint32_t* samplerSpaces;

    const uint32_t  numRTAccelerationStructures;
    const char**    rtAccelerationStructureNames;
    const uint32_t* rtAccelerationStructureBindings;
    const uint32_t* rtAccelerationStructureCounts;
    const uint32_t* rtAccelerationStructureSpaces;

    const uint32_t  numRTTextures;
    const char**    rtTextureNames;
    const uint32_t* rtTextureBindings;
    const uint32_t* rtTextureCounts;
    const uint32_t* rtTextureSpaces;
} ffxm_fsr3upscaler_luma_pyramid_pass_PermutationInfo;

static const uint32_t g_ffxm_fsr3upscaler_luma_pyramid_pass_IndirectionTable[] = {
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
};

static const ffxm_fsr3upscaler_luma_pyramid_pass_PermutationInfo g_ffxm_fsr3upscaler_luma_pyramid_pass_PermutationInfo[] = {
    { g_ffxm_fsr3upscaler_luma_pyramid_pass_1401dc4626c7b4af721b6eab8b9e1622_size, g_ffxm_fsr3upscaler_luma_pyramid_pass_1401dc4626c7b4af721b6eab8b9e1622_data, 2, g_ffxm_fsr3upscaler_luma_pyramid_pass_1401dc4626c7b4af721b6eab8b9e1622_CBVResourceNames, g_ffxm_fsr3upscaler_luma_pyramid_pass_1401dc4626c7b4af721b6eab8b9e1622_CBVResourceBindings, g_ffxm_fsr3upscaler_luma_pyramid_pass_1401dc4626c7b4af721b6eab8b9e1622_CBVResourceCounts, g_ffxm_fsr3upscaler_luma_pyramid_pass_1401dc4626c7b4af721b6eab8b9e1622_CBVResourceSets, 2, g_ffxm_fsr3upscaler_luma_pyramid_pass_1401dc4626c7b4af721b6eab8b9e1622_TextureSRVResourceNames, g_ffxm_fsr3upscaler_luma_pyramid_pass_1401dc4626c7b4af721b6eab8b9e1622_TextureSRVResourceBindings, g_ffxm_fsr3upscaler_luma_pyramid_pass_1401dc4626c7b4af721b6eab8b9e1622_TextureSRVResourceCounts, g_ffxm_fsr3upscaler_luma_pyramid_pass_1401dc4626c7b4af721b6eab8b9e1622_TextureSRVResourceSets, 4, g_ffxm_fsr3upscaler_luma_pyramid_pass_1401dc4626c7b4af721b6eab8b9e1622_TextureUAVResourceNames, g_ffxm_fsr3upscaler_luma_pyramid_pass_1401dc4626c7b4af721b6eab8b9e1622_TextureUAVResourceBindings, g_ffxm_fsr3upscaler_luma_pyramid_pass_1401dc4626c7b4af721b6eab8b9e1622_TextureUAVResourceCounts, g_ffxm_fsr3upscaler_luma_pyramid_pass_1401dc4626c7b4af721b6eab8b9e1622_TextureUAVResourceSets, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, },
};

