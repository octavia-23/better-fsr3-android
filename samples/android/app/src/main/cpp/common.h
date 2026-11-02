// Copyright © 2026 Arm Limited.
// SPDX-License-Identifier: MIT
#pragma once

#if defined(__ANDROID__)
#include <android/log.h>
#define ASR_TAG "ArmASR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  ASR_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  ASR_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, ASR_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define ASR_TAG "ArmASR"
#define LOGI(...) do { printf("[INFO] " __VA_ARGS__); printf("\n"); } while(0)
#define LOGW(...) do { printf("[WARN] " __VA_ARGS__); printf("\n"); } while(0)
#define LOGE(...) do { printf("[ERROR] " __VA_ARGS__); printf("\n"); } while(0)
#endif
#include <vulkan/vulkan.h>

#include <cstdlib>

// Vulkan calls in this sample are checked but never fatal on their own: the
// point of the sample is to report *why* a device cannot run Arm ASR, not to
// abort with no explanation.
#define VK_CHECK(expr)                                                      \
    do {                                                                    \
        VkResult vkCheckResult_ = (expr);                                   \
        if (vkCheckResult_ != VK_SUCCESS) {                                 \
            LOGE("%s:%d: %s failed with VkResult %d", __FILE__, __LINE__,   \
                 #expr, (int)vkCheckResult_);                               \
            return false;                                                   \
        }                                                                   \
    } while (0)
