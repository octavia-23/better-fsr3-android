// Copyright (c) 2024-2026 Advanced Micro Devices, Inc. All rights reserved.
// Copyright (c) 2024-2026 Arm Limited. All rights reserved.
// SPDX-License-Identifier: MIT

#ifndef FFX_SPD_H
#define FFX_SPD_H

#ifndef FFXM_GROUP_MEMORY_BARRIER
#if defined(FFX_GLSL) || defined(FFXM_GLSL)
#define FFXM_GROUP_MEMORY_BARRIER() groupMemoryBarrier(); barrier()
#elif defined(FFX_HLSL) || defined(FFXM_HLSL)
#define FFXM_GROUP_MEMORY_BARRIER() GroupMemoryBarrierWithGroupSync()
#endif
#endif

#include "spd/ffxm_spd.h"

#endif // FFX_SPD_H
