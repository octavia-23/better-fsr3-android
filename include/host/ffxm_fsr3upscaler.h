// Copyright  © 2024 Advanced Micro Devices, Inc.
// Copyright  © 2026 Arm Limited.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

// Include the interface for the backend of the FSR3 API.
#include "ffxm_interface.h"

namespace arm
{

/// @defgroup ffxmFsr3Upscaler FidelityFX FSR3 Upscaler
/// FidelityFX Super Resolution 3 Upscaler runtime library
///
/// @ingroup SDKComponents

/// FidelityFX Super Resolution 3 major version.
///
/// @ingroup ffxmFsr3Upscaler
#define FFXM_FSR3UPSCALER_VERSION_MAJOR      (3)

/// FidelityFX Super Resolution 3 minor version.
///
/// @ingroup ffxmFsr3Upscaler
#define FFXM_FSR3UPSCALER_VERSION_MINOR      (1)

/// FidelityFX Super Resolution 3 patch version.
///
/// @ingroup ffxmFsr3Upscaler
#define FFXM_FSR3UPSCALER_VERSION_PATCH      (4)

/// FidelityFX Super Resolution 3 context count
///
/// Defines the number of internal effect contexts required by FSR3
///
/// @ingroup ffxmFsr3Upscaler
#define FFXM_FSR3UPSCALER_CONTEXT_COUNT      1

/// The size of the context specified in 32-bit values.
///
/// Windows x64: 65,536 uint32 entries (256 KB = 262,144 bytes).
/// Android arm64 / other: 131,072 uint32 entries (512 KB = 524,288 bytes).
///
/// @ingroup ffxmFsr3Upscaler
#if defined(_WIN32)
#define FFXM_FSR3UPSCALER_CONTEXT_SIZE       (65536)
#else
#define FFXM_FSR3UPSCALER_CONTEXT_SIZE       (131072)
#endif

#if defined(__cplusplus)
#define FFXM_ALIGNAS(x) alignas(x)
#elif defined(_MSC_VER)
#define FFXM_ALIGNAS(x) __declspec(align(x))
#elif defined(__GNUC__) || defined(__clang__)
#define FFXM_ALIGNAS(x) __attribute__((aligned(x)))
#else
#define FFXM_ALIGNAS(x)
#endif

#if defined(__cplusplus)
extern "C" {
#endif // #if defined(__cplusplus)

/// An enumeration of all the passes which constitute the FSR3 algorithm.
///
/// FSR3 is implemented as a composite of several compute passes each
/// computing a key part of the final result. Each call to the
/// <c><i>FfxmFsr3UpscalerScheduleGpuJobFunc</i></c> callback function will
/// correspond to a single pass included in <c><i>FfxmFsr3UpscalerPass</i></c>.
///
/// Please note in some cases e.g.: <c><i>FFXM_FSR3UPSCALER_PASS_ACCUMULATE</i></c>
/// and <c><i>FFXM_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN</i></c> either one pass or the
/// other will be used (they are mutually exclusive). The choice of which will
/// depend on the way the <c><i>FfxmFsr3UpscalerContext</i></c> is created and the
/// precise contents of <c><i>FfxmFsr3UpscalerDispatchDescription</i></c> each time a call
/// is made to <c><i>ffxmFsr3UpscalerContextDispatch</i></c>.
///
/// @ingroup ffxmFsr3Upscaler
typedef enum FfxmFsr3UpscalerPass
{
    FFXM_FSR3UPSCALER_PASS_PREPARE_INPUTS             = 0,  ///< A pass which prepares game inputs for later passes.
    FFXM_FSR3UPSCALER_PASS_LUMA_PYRAMID               = 1,  ///< A pass which generates the luminance mipmap chain for the current frame.
    FFXM_FSR3UPSCALER_PASS_SHADING_CHANGE_PYRAMID     = 2,  ///< A pass which generates the shading change detection mipmap chain for the current frame.
    FFXM_FSR3UPSCALER_PASS_SHADING_CHANGE             = 3,  ///< A pass which estimates shading changes for the current frame.
    FFXM_FSR3UPSCALER_PASS_PREPARE_REACTIVITY         = 4,  ///< A pass which prepares accumulation relevant information.
    FFXM_FSR3UPSCALER_PASS_LUMA_INSTABILITY           = 5,  ///< A pass which estimates temporal instability of the luminance changes.
    FFXM_FSR3UPSCALER_PASS_ACCUMULATE                 = 6,  ///< A pass which performs upscaling.
    FFXM_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN         = 7,  ///< A pass which performs upscaling when sharpening is used.
    FFXM_FSR3UPSCALER_PASS_RCAS                       = 8,  ///< A pass which performs sharpening.
    FFXM_FSR3UPSCALER_PASS_DEBUG_VIEW                 = 9,  ///< A pass which draws some internal resources, for debugging purposes.
    FFXM_FSR3UPSCALER_PASS_GENERATE_REACTIVE          = 10, ///< An optional pass to generate a reactive mask.
    FFXM_FSR3UPSCALER_PASS_TCR_AUTOGENERATE           = 11, ///< DEPRECATED - No longer supported.

    FFXM_FSR3UPSCALER_PASS_COUNT                      = 12  ///< The number of passes defined for FSR3.
} FfxmFsr3UpscalerPass;

/// An enumeration of all the quality modes supported by FidelityFX Super
/// Resolution 3 upscaling.
///
/// In order to provide a consistent user experience across multiple
/// applications which implement FSR3, it is strongly recommended that the
/// following preset scaling factors are made available through your
/// application's user interface.
///
/// @ingroup ffxmFsr3Upscaler
typedef enum FfxmFsr3UpscalerQualityMode {
    FFXM_FSR3UPSCALER_QUALITY_MODE_NATIVEAA           = 0,  ///< Perform upscaling with a per-dimension scaling ratio of 1.0x.
    FFXM_FSR3UPSCALER_QUALITY_MODE_QUALITY            = 1,  ///< Perform upscaling with a per-dimension scaling ratio of 1.5x.
    FFXM_FSR3UPSCALER_QUALITY_MODE_BALANCED           = 2,  ///< Perform upscaling with a per-dimension scaling ratio of 1.7x.
    FFXM_FSR3UPSCALER_QUALITY_MODE_PERFORMANCE        = 3,  ///< Perform upscaling with a per-dimension scaling ratio of 2.0x.
    FFXM_FSR3UPSCALER_QUALITY_MODE_ULTRA_PERFORMANCE  = 4   ///< Perform upscaling with a per-dimension scaling ratio of 3.0x.
} FfxmFsr3UpscalerQualityMode;

/// An alias for compatibility with ASR shader quality mode nomenclature.
///
/// @ingroup ffxmFsr3Upscaler
typedef FfxmFsr3UpscalerQualityMode FfxmFsr3UpscalerShaderQualityMode;

/// An enumeration of bit flags used when creating a
/// <c><i>FfxmFsr3UpscalerContext</i></c>. See <c><i>FfxmFsr3UpscalerContextDescription</i></c>.
///
/// @ingroup ffxmFsr3Upscaler
typedef enum FfxmFsr3UpscalerInitializationFlagBits {
    FFXM_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE                  = (1<<0), ///< A bit indicating if the input color data provided is using high-dynamic range.
    FFXM_FSR3UPSCALER_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS   = (1<<1), ///< A bit indicating if the motion vectors are rendered at display resolution.
    FFXM_FSR3UPSCALER_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION  = (1<<2), ///< A bit indicating that the motion vectors have the jittering pattern applied to them.
    FFXM_FSR3UPSCALER_ENABLE_DEPTH_INVERTED                      = (1<<3), ///< A bit indicating that the input depth buffer data provided is inverted [1..0].
    FFXM_FSR3UPSCALER_ENABLE_DEPTH_INFINITE                      = (1<<4), ///< A bit indicating that the input depth buffer data provided is using an infinite far plane.
    FFXM_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE                       = (1<<5), ///< A bit indicating if automatic exposure should be applied to input color data.
    FFXM_FSR3UPSCALER_ENABLE_DYNAMIC_RESOLUTION                  = (1<<6), ///< A bit indicating that the application uses dynamic resolution scaling.
    FFXM_FSR3UPSCALER_ENABLE_TEXTURE1D_USAGE                     = (1<<7), ///< Deprecated flag maintained for API backwards compatibility.
    FFXM_FSR3UPSCALER_ENABLE_DEBUG_CHECKING                      = (1<<8), ///< A bit indicating that the runtime should check API parameters and report issues.
} FfxmFsr3UpscalerInitializationFlagBits;

/// Bit flags passed to <c><i>FfxmFsr3UpscalerDispatchDescription::flags</i></c>.
///
/// @ingroup ffxmFsr3Upscaler
typedef enum FfxmFsr3UpscalerDispatchFlags
{
    FFXM_FSR3UPSCALER_DISPATCH_DRAW_DEBUG_VIEW = (1 << 0),  ///< A bit indicating that the output resource will contain debug views with relevant information.
} FfxmFsr3UpscalerDispatchFlags;

/// Parameter keys for dynamic constant-buffer configuration via <c><i>ffxmFsr3UpscalerSetConstant</i></c>.
///
/// @ingroup ffxmFsr3Upscaler
typedef enum FfxmFsr3UpscalerConfigureKey
{
    FFXM_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_FVELOCITYFACTOR              = 0, ///< Override constant buffer fVelocityFactor. [0.0f, 1.0f], default 1.0f.
    FFXM_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_FREACTIVENESSSCALE           = 1, ///< Override constant buffer fReactivenessScale. [0.0f, +inf], default 1.0f.
    FFXM_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_FSHADINGCHANGESCALE          = 2, ///< Override constant buffer fShadingChangeScale. [0.0f, +inf], default 1.0f.
    FFXM_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_FACCUMULATIONADDEDPERFRAME   = 3, ///< Override constant buffer fAccumulationAddedPerFrame. [0.0f, 1.0f], default 0.333f.
    FFXM_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_FMINDISOCCLUSIONACCUMULATION = 4  ///< Override constant buffer fMinDisocclusionAccumulation. [-1.0f, 1.0f], default -0.333f.
} FfxmFsr3UpscalerConfigureKey;

/// Pass a string message
///
/// Used for debug messages.
///
/// @param [in] type                       The type of message.
/// @param [in] message                    A string message to pass.
///
/// @ingroup ffxmFsr3Upscaler
typedef void(*FfxmFsr3UpscalerMessage)(
    FfxmMsgType type,
    const wchar_t* message);

/// A structure encapsulating the parameters required to initialize FidelityFX
/// Super Resolution 3 upscaling.
///
/// @ingroup ffxmFsr3Upscaler
typedef struct FfxmFsr3UpscalerContextDescription {
    uint32_t                    flags;                              ///< A collection of <c><i>FfxmFsr3UpscalerInitializationFlagBits</i></c>.
    FfxmDimensions2D            maxRenderSize;                      ///< The maximum size that rendering will be performed at.
    FfxmDimensions2D            maxUpscaleSize;                     ///< The size of the presentation resolution targeted by the upscaling process.
    FfxmFsr3UpscalerMessage     fpMessage;                          ///< A pointer to a function that can receive messages from the runtime.
    FfxmInterface               backendInterface;                   ///< A set of pointers to the backend implementation for FidelityFX SDK.
} FfxmFsr3UpscalerContextDescription;

/// A structure encapsulating the parameters for dispatching the various passes
/// of FidelityFX Super Resolution 3.
///
/// @ingroup ffxmFsr3Upscaler
typedef struct FfxmFsr3UpscalerDispatchDescription {
    FfxmCommandList              commandList;                        ///< The <c><i>FfxmCommandList</i></c> to record FSR3 rendering commands into.
    FfxmResource                 color;                              ///< A <c><i>FfxmResource</i></c> containing the color buffer for the current frame (at render resolution).
    FfxmResource                 depth;                              ///< A <c><i>FfxmResource</i></c> containing 32bit depth values for the current frame (at render resolution).
    FfxmResource                 motionVectors;                      ///< A <c><i>FfxmResource</i></c> containing 2-dimensional motion vectors (at render resolution if <c><i>FFXM_FSR3UPSCALER_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS</i></c> is not set).
    FfxmResource                 exposure;                           ///< An optional <c><i>FfxmResource</i></c> containing a 1x1 exposure value.
    FfxmResource                 reactive;                           ///< An optional <c><i>FfxmResource</i></c> containing alpha value of reactive objects in the scene.
    FfxmResource                 transparencyAndComposition;         ///< An optional <c><i>FfxmResource</i></c> containing alpha value of special objects in the scene.
    FfxmResource                 dilatedDepth;                       ///< A <c><i>FfxmResource</i></c> allocated as described in <c><i>FfxmFsr3UpscalerSharedResourceDescriptions</i></c> that is used to emit dilated depth and share with following effects (e.g., Frame Generation).
    FfxmResource                 dilatedMotionVectors;               ///< A <c><i>FfxmResource</i></c> allocated as described in <c><i>FfxmFsr3UpscalerSharedResourceDescriptions</i></c> that is used to emit dilated motion vectors and share with following effects (e.g., Frame Generation).
    FfxmResource                 reconstructedPrevNearestDepth;      ///< A <c><i>FfxmResource</i></c> allocated as described in <c><i>FfxmFsr3UpscalerSharedResourceDescriptions</i></c> that is used to emit reconstructed previous nearest depth and share with following effects (e.g., Frame Generation).
    FfxmResource                 output;                             ///< A <c><i>FfxmResource</i></c> containing the output color buffer for the current frame (at presentation resolution).
    FfxmFloatCoords2D            jitterOffset;                       ///< The subpixel jitter offset applied to the camera.
    FfxmFloatCoords2D            motionVectorScale;                  ///< The scale factor to apply to motion vectors.
    FfxmDimensions2D             renderSize;                         ///< The resolution that was used for rendering the input resources.
    FfxmDimensions2D             upscaleSize;                        ///< The resolution that the upscaler will output.
    bool                         enableSharpening;                   ///< Enable an additional sharpening pass.
    float                        sharpness;                          ///< The sharpness value between 0 and 1, where 0 is no additional sharpness and 1 is maximum additional sharpness.
    float                        frameTimeDelta;                     ///< The time elapsed since the last frame (expressed in milliseconds).
    float                        preExposure;                        ///< The pre-exposure value (must be > 0.0f).
    bool                         reset;                              ///< A boolean value which when set to true, indicates the camera has moved discontinuously.
    float                        cameraNear;                         ///< The distance to the near plane of the camera.
    float                        cameraFar;                          ///< The distance to the far plane of the camera.
    float                        cameraFovAngleVertical;             ///< The camera angle field of view in the vertical direction (expressed in radians).
    float                        viewSpaceToMetersFactor;            ///< The scale factor to convert view space units to meters.
    uint32_t                     flags;                              ///< Combination of <c><i>FfxmFsr3UpscalerDispatchFlags</i></c>.
} FfxmFsr3UpscalerDispatchDescription;

/// A structure encapsulating the parameters for automatic generation of a reactive mask.
///
/// @ingroup ffxmFsr3Upscaler
typedef struct FfxmFsr3UpscalerGenerateReactiveDescription {
    FfxmCommandList              commandList;                        ///< The <c><i>FfxmCommandList</i></c> to record FSR3 rendering commands into.
    FfxmResource                 colorOpaqueOnly;                    ///< A <c><i>FfxmResource</i></c> containing the opaque only color buffer for the current frame (at render resolution).
    FfxmResource                 colorPreUpscale;                    ///< A <c><i>FfxmResource</i></c> containing the opaque+translucent color buffer for the current frame (at render resolution).
    FfxmResource                 outReactive;                        ///< A <c><i>FfxmResource</i></c> containing the surface to generate the reactive mask into.
    FfxmDimensions2D             renderSize;                         ///< The resolution that was used for rendering the input resources.
    float                        scale;                              ///< A value to scale the output.
    float                        cutoffThreshold;                    ///< A threshold value to generate a binary reactive mask.
    float                        binaryValue;                        ///< A value to set for the binary reactive mask.
    uint32_t                     flags;                              ///< Flags to determine how to generate the reactive mask.
} FfxmFsr3UpscalerGenerateReactiveDescription;

/// A structure encapsulating the resource descriptions for shared resources for this effect.
///
/// Used for sharing resources with downstream Frame Generation (Phase 2).
///
/// @ingroup ffxmFsr3Upscaler
typedef struct FfxmFsr3UpscalerSharedResourceDescriptions {
    FfxmCreateResourceDescription reconstructedPrevNearestDepth; ///< The <c><i>FfxmCreateResourceDescription</i></c> for allocating the <c><i>reconstructedPrevNearestDepth</i></c> shared resource (R32_UINT).
    FfxmCreateResourceDescription dilatedDepth;                  ///< The <c><i>FfxmCreateResourceDescription</i></c> for allocating the <c><i>dilatedDepth</i></c> shared resource (R32_FLOAT).
    FfxmCreateResourceDescription dilatedMotionVectors;          ///< The <c><i>FfxmCreateResourceDescription</i></c> for allocating the <c><i>dilatedMotionVectors</i></c> shared resource (R16G16_FLOAT).
} FfxmFsr3UpscalerSharedResourceDescriptions;

/// A structure encapsulating the FidelityFX Super Resolution 3 context.
///
/// This sets up an object which contains all persistent internal data and
/// resources that are required by FSR3.
///
/// Must be 8-byte aligned to match the private implementation struct.
///
/// @ingroup ffxmFsr3Upscaler
typedef struct FFXM_ALIGNAS(8) FfxmFsr3UpscalerContext
{
    uint32_t data[FFXM_FSR3UPSCALER_CONTEXT_SIZE];  ///< An opaque set of <c>uint32_t</c> which contain the data for the context.
} FfxmFsr3UpscalerContext;

/// A typedef for an effect version number.
///
/// @ingroup ffxmFsr3Upscaler
typedef uint32_t FfxmVersionNumber;

/// @defgroup Fsr3UpscalerResourceIdentifiers Resource Identifiers
/// Identifiers for internal and external resources used by FSR 3.1.4.
///
/// @ingroup ffxmFsr3Upscaler
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_NULL                                           0
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_OPAQUE_ONLY                              1
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_COLOR                                    2
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_MOTION_VECTORS                           3
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_DEPTH                                    4
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_EXPOSURE                                 5
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_REACTIVE_MASK                            6
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_TRANSPARENCY_AND_COMPOSITION_MASK        7
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH           8
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_MOTION_VECTORS                         9
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_DEPTH                                  10
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR                        11
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION                                   12
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_NEW_LOCKS                                      13
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY                                   14
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DEBUG_OUTPUT                                   15
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LANCZOS_LUT                                    16
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_ATOMIC_COUNT                               17
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_UPSCALED_OUTPUT                                18
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_RCAS_INPUT                                     19
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_1                                 20
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_2                                 21
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR_1                      22
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR_2                      23
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_REACTIVITY                    24
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_TRANSPARENCY_AND_COMPOSITION  25
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_REACTIVE_MASKS                         26
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS                                       27
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS_LEVEL_0                               27
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS_LEVEL_1                               28
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS_LEVEL_2                               29
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS_LEVEL_3                               30
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS_LEVEL_4                               31
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS_LEVEL_5                               32
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_EXPOSURE                      33
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_FRAME_INFO                                     34
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_AUTOREACTIVE                                   35
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_AUTOCOMPOSITION_DEPRECATED                     36
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY_1                                 37
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY_2                                 38
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_1                                         40
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_2                                         41
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SHADING_CHANGE                                 42
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_FARTHEST_DEPTH                                 43
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_FARTHEST_DEPTH_MIP1                            44
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_CURRENT_LUMA                                   45
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_PREVIOUS_LUMA                                  46
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_INSTABILITY                               48
#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERMEDIATE_FP16x1                            49

#define FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_COUNT                                          60

#define FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER                             0
#define FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_SPD                                      1
#define FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_RCAS                                     2
#define FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_GENREACTIVE                              3
#define FFXM_FSR3UPSCALER_CONSTANTBUFFER_COUNT                                               4

#define FFXM_FSR3UPSCALER_AUTOREACTIVEFLAGS_APPLY_TONEMAP                                    1
#define FFXM_FSR3UPSCALER_AUTOREACTIVEFLAGS_APPLY_INVERSETONEMAP                             2
#define FFXM_FSR3UPSCALER_AUTOREACTIVEFLAGS_APPLY_THRESHOLD                                  4
#define FFXM_FSR3UPSCALER_AUTOREACTIVEFLAGS_USE_COMPONENTS_MAX                               8

/// Create a FidelityFX Super Resolution 3 context from the parameters
/// programmed to the <c><i>FfxmFsr3UpscalerContextDescription</i></c> structure.
///
/// The context structure is the main object used to interact with the FSR3
/// API, and is responsible for the management of the internal resources used
/// by the FSR3 algorithm. When this API is called, multiple calls will be
/// made via the pointers contained in the <c><i>backendInterface</i></c> structure.
/// These callbacks will attempt to retrieve the device capabilities, and
/// create the internal resources, and pipelines required by FSR3's
/// frame-to-frame function.
///
/// @param [out] pContext                A pointer to a <c><i>FfxmFsr3UpscalerContext</i></c> structure to populate.
/// @param [in]  pContextDescription     A pointer to a <c><i>FfxmFsr3UpscalerContextDescription</i></c> structure.
///
/// @retval
/// FFXM_OK                              The operation completed successfully.
/// @retval
/// FFXM_ERROR_INVALID_POINTER           The operation failed because either <c><i>pContext</i></c> or <c><i>pContextDescription</i></c> was <c><i>NULL</i></c>.
/// @retval
/// FFXM_ERROR_INCOMPLETE_INTERFACE      The operation failed because the <c><i>FfxmFsr3UpscalerContextDescription.backendInterface</i></c> was not fully specified.
/// @retval
/// FFXM_ERROR_BACKEND_API_ERROR         The operation failed because of an error returned from the backend.
/// @retval
/// FFXM_ERROR_UNSUPPORTED_DEVICE        The operation failed because the device lacks required hardware features (e.g. shaderFloat16).
///
/// @ingroup ffxmFsr3Upscaler
FFXM_API FfxmErrorCode ffxmFsr3UpscalerContextCreate(
    FfxmFsr3UpscalerContext* pContext,
    const FfxmFsr3UpscalerContextDescription* pContextDescription);

/// Provides the descriptions for shared resources that must be allocated for this effect.
///
/// Queries resource descriptions for dilated depth, dilated motion vectors, and
/// reconstructed previous nearest depth to be shared with downstream frame generation.
///
/// @param [in]  pContext                A pointer to a <c><i>FfxmFsr3UpscalerContext</i></c> structure.
/// @param [out] pSharedResources        A pointer to a <c><i>FfxmFsr3UpscalerSharedResourceDescriptions</i></c> to populate.
///
/// @retval
/// FFXM_OK                              The operation completed successfully.
/// @retval
/// FFXM_ERROR_INVALID_POINTER           The operation failed because either <c><i>pContext</i></c> or <c><i>pSharedResources</i></c> was <c><i>NULL</i></c>.
///
/// @ingroup ffxmFsr3Upscaler
FFXM_API FfxmErrorCode ffxmFsr3UpscalerGetSharedResourceDescriptions(
    FfxmFsr3UpscalerContext* pContext,
    FfxmFsr3UpscalerSharedResourceDescriptions* pSharedResources);

/// Dispatch the various passes that constitute FidelityFX Super Resolution 3.
///
/// FSR3 is a composite effect, meaning that it is comprised of multiple
/// constituent passes (implemented as one or more clears, copies and compute/fragment
/// dispatches). The <c><i>ffxmFsr3UpscalerContextDispatch</i></c> function generates
/// the sequence of graphics API calls required each frame.
///
/// @param [in] pContext                 A pointer to a <c><i>FfxmFsr3UpscalerContext</i></c> structure.
/// @param [in] pDispatchDescription     A pointer to a <c><i>FfxmFsr3UpscalerDispatchDescription</i></c> structure.
///
/// @retval
/// FFXM_OK                              The operation completed successfully.
/// @retval
/// FFXM_ERROR_INVALID_POINTER           The operation failed because either <c><i>pContext</i></c> or <c><i>pDispatchDescription</i></c> was <c><i>NULL</i></c>.
/// @retval
/// FFXM_ERROR_OUT_OF_RANGE              The operation failed because <c><i>pDispatchDescription->renderSize</i></c> was larger than the maximum render resolution.
/// @retval
/// FFXM_ERROR_NULL_DEVICE               The operation failed because the device inside the context was <c><i>NULL</i></c>.
/// @retval
/// FFXM_ERROR_BACKEND_API_ERROR         The operation failed because of an error returned from the backend.
///
/// @ingroup ffxmFsr3Upscaler
FFXM_API FfxmErrorCode ffxmFsr3UpscalerContextDispatch(
    FfxmFsr3UpscalerContext* pContext,
    const FfxmFsr3UpscalerDispatchDescription* pDispatchDescription);

/// A helper function to generate a Reactive mask from an opaque-only texture and one containing translucent objects.
///
/// @param [in] pContext                 A pointer to a <c><i>FfxmFsr3UpscalerContext</i></c> structure.
/// @param [in] pParams                  A pointer to a <c><i>FfxmFsr3UpscalerGenerateReactiveDescription</i></c> structure.
///
/// @retval
/// FFXM_OK                              The operation completed successfully.
/// @retval
/// FFXM_ERROR_INVALID_POINTER           The operation failed because either <c><i>pContext</i></c> or <c><i>pParams</i></c> was <c><i>NULL</i></c>.
///
/// @ingroup ffxmFsr3Upscaler
FFXM_API FfxmErrorCode ffxmFsr3UpscalerContextGenerateReactiveMask(
    FfxmFsr3UpscalerContext* pContext,
    const FfxmFsr3UpscalerGenerateReactiveDescription* pParams);

/// Destroy the FidelityFX Super Resolution 3 context.
///
/// Releases all resources, pipelines, and descriptor heaps allocated by the context.
///
/// @param [out] pContext                A pointer to a <c><i>FfxmFsr3UpscalerContext</i></c> structure to destroy.
///
/// @retval
/// FFXM_OK                              The operation completed successfully.
/// @retval
/// FFXM_ERROR_INVALID_POINTER           The operation failed because <c><i>pContext</i></c> was <c><i>NULL</i></c>.
///
/// @ingroup ffxmFsr3Upscaler
FFXM_API FfxmErrorCode ffxmFsr3UpscalerContextDestroy(
    FfxmFsr3UpscalerContext* pContext);

/// Get the upscale ratio from the quality mode.
///
/// Quality preset                                              | Scale factor
/// ----------------------------------------------------------- | -------------
/// <c><i>FFXM_FSR3UPSCALER_QUALITY_MODE_NATIVEAA</i></c>       | 1.0x
/// <c><i>FFXM_FSR3UPSCALER_QUALITY_MODE_QUALITY</i></c>        | 1.5x
/// <c><i>FFXM_FSR3UPSCALER_QUALITY_MODE_BALANCED</i></c>       | 1.7x
/// <c><i>FFXM_FSR3UPSCALER_QUALITY_MODE_PERFORMANCE</i></c>    | 2.0x
/// <c><i>FFXM_FSR3UPSCALER_QUALITY_MODE_ULTRA_PERFORMANCE</i></c> | 3.0x
///
/// Passing an invalid <c><i>qualityMode</i></c> will return 0.0f.
///
/// @param [in] qualityMode             The quality mode preset.
///
/// @returns
/// The per-dimension upscaling ratio for <c><i>qualityMode</i></c>.
///
/// @ingroup ffxmFsr3Upscaler
FFXM_API float ffxmFsr3UpscalerGetUpscaleRatioFromQualityMode(
    FfxmFsr3UpscalerQualityMode qualityMode);

/// A helper function to calculate the rendering resolution from a target
/// resolution and desired quality level.
///
/// @param [out] pRenderWidth            A pointer to a <c>uint32_t</c> which will hold the calculated render resolution width.
/// @param [out] pRenderHeight           A pointer to a <c>uint32_t</c> which will hold the calculated render resolution height.
/// @param [in]  displayWidth           The target display resolution width.
/// @param [in]  displayHeight          The target display resolution height.
/// @param [in]  qualityMode            The desired quality mode for FSR3 upscaling.
///
/// @retval
/// FFXM_OK                              The operation completed successfully.
/// @retval
/// FFXM_ERROR_INVALID_POINTER           Either <c><i>pRenderWidth</i></c> or <c><i>pRenderHeight</i></c> was <c>NULL</c>.
/// @retval
/// FFXM_ERROR_INVALID_ENUM              An invalid quality mode was specified.
///
/// @ingroup ffxmFsr3Upscaler
FFXM_API FfxmErrorCode ffxmFsr3UpscalerGetRenderResolutionFromQualityMode(
    uint32_t* pRenderWidth,
    uint32_t* pRenderHeight,
    uint32_t displayWidth,
    uint32_t displayHeight,
    FfxmFsr3UpscalerQualityMode qualityMode);

/// A helper function to calculate the jitter phase count from render and display resolution.
///
/// @param [in] renderWidth             The render resolution width.
/// @param [in] displayWidth            The display resolution width.
///
/// @returns
/// The jitter phase count for the scaling factor between <c><i>renderWidth</i></c> and <c><i>displayWidth</i></c>.
///
/// @ingroup ffxmFsr3Upscaler
FFXM_API int32_t ffxmFsr3UpscalerGetJitterPhaseCount(
    int32_t renderWidth,
    int32_t displayWidth);

/// A helper function to calculate the subpixel jitter offset using a Halton(2,3) sequence.
///
/// @param [out] pOutX                  A pointer to a <c>float</c> which will contain the subpixel jitter offset for the x dimension.
/// @param [out] pOutY                  A pointer to a <c>float</c> which will contain the subpixel jitter offset for the y dimension.
/// @param [in]  index                  The index within the jitter sequence.
/// @param [in]  phaseCount             The length of jitter phase. See <c><i>ffxmFsr3UpscalerGetJitterPhaseCount</i></c>.
///
/// @retval
/// FFXM_OK                              The operation completed successfully.
/// @retval
/// FFXM_ERROR_INVALID_POINTER           Either <c><i>pOutX</i></c> or <c><i>pOutY</i></c> was <c>NULL</c>.
/// @retval
/// FFXM_ERROR_INVALID_ARGUMENT          Argument <c><i>phaseCount</i></c> must be greater than 0.
///
/// @ingroup ffxmFsr3Upscaler
FFXM_API FfxmErrorCode ffxmFsr3UpscalerGetJitterOffset(
    float* pOutX,
    float* pOutY,
    int32_t index,
    int32_t phaseCount);

/// A helper function to check if a resource is null / uninitialized.
///
/// @param [in] resource                A <c><i>FfxmResource</i></c>.
///
/// @returns
/// true                                The <c><i>resource.resource</i></c> pointer is <c><i>NULL</i></c>.
/// @returns
/// false                               The <c><i>resource.resource</i></c> pointer is not <c><i>NULL</i></c>.
///
/// @ingroup ffxmFsr3Upscaler
FFXM_API bool ffxmFsr3UpscalerResourceIsNull(
    FfxmResource resource);

/// Queries the effect version number.
///
/// @returns
/// The SDK version the effect was built with.
///
/// @ingroup ffxmFsr3Upscaler
FFXM_API FfxmVersionNumber ffxmFsr3UpscalerGetEffectVersion(void);

/// Override upscaler constant buffer value after upscaler context creation.
///
/// @param [in] pContext                A pointer to a <c><i>FfxmFsr3UpscalerContext</i></c> structure.
/// @param [in] key                     A key from <c><i>FfxmFsr3UpscalerConfigureKey</i></c> enum.
/// @param [in] valuePtr                A pointer to value to pass to shader in Constant Buffer.
///
/// @retval
/// FFXM_OK                              The operation completed successfully.
/// @retval
/// FFXM_ERROR_INVALID_ENUM              An invalid FfxmFsr3UpscalerConfigureKey was specified.
/// @retval
/// FFXM_ERROR_INVALID_POINTER           <c><i>pContext</i></c> or <c><i>valuePtr</i></c> was NULL.
///
/// @ingroup ffxmFsr3Upscaler
FFXM_API FfxmErrorCode ffxmFsr3UpscalerSetConstant(
    FfxmFsr3UpscalerContext* pContext,
    FfxmFsr3UpscalerConfigureKey key,
    void* valuePtr);

#if defined(__cplusplus)
}
#endif // #if defined(__cplusplus)

} // namespace arm
