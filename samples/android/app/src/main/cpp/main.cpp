// Copyright © 2026 Arm Limited.
// SPDX-License-Identifier: MIT
//
// Minimal NativeActivity host for the Arm ASR Vulkan sample. Renders a
// procedural scene at a reduced resolution, upscales it with Arm ASR, and
// presents the result.

#include "common.h"
#include "fsr3_upscaler.h"
#include "gpu_profiler.h"
#include "scene.h"
#include "swapchain_recovery.h"
#include "vk_context.h"

#include <android_native_app_glue.h>

#include <cmath>
#include <ctime>
#include <memory>

namespace {

struct App {
    VkContext       ctx;
    Scene           scene;
    Fsr3Upscaler    upscaler;
    arm::GpuProfiler profiler;

    bool     vulkanReady = false;
    bool     renderReady = false;
    bool     swapchainDirty = false;
    bool     renderingTerminated = false;
    uint32_t consecutiveRecreateFailures = 0;
    int32_t  frameIndex  = 0;

    VkExtent2D renderExtent  = {0, 0};
    VkExtent2D displayExtent = {0, 0};

    float camZ     = 0.0f;
    float camZPrev = 0.0f;
    double lastTimeSeconds = 0.0;
    ANativeWindow* window = nullptr;
};

double nowSeconds()
{
    timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

bool createSizedResources(App& app)
{
    app.displayExtent = app.ctx.swapchainExtent();

    // 2x per-dimension upscaling: a 1080p panel is fed from 960x540.
    const float ratio = arm::ffxmFsr3UpscalerGetUpscaleRatioFromQualityMode(arm::FFXM_FSR3UPSCALER_QUALITY_MODE_PERFORMANCE);
    app.renderExtent  = {(uint32_t)(app.displayExtent.width / ratio),
                         (uint32_t)(app.displayExtent.height / ratio)};
    LOGI("FSR 3.1.4: Rendering at %ux%u, presenting at %ux%u (ratio %.2f)", app.renderExtent.width,
         app.renderExtent.height, app.displayExtent.width, app.displayExtent.height, ratio);

    if (!app.scene.init(app.ctx, app.renderExtent, app.displayExtent)) {
        LOGE("Scene initialisation failed");
        return false;
    }
    if (!app.upscaler.init(app.ctx, app.renderExtent, app.displayExtent,
                           arm::FFXM_FSR3UPSCALER_QUALITY_MODE_PERFORMANCE)) {
        LOGE("FSR 3.1.4 initialisation failed");
        return false;
    }

    app.frameIndex = 0; // Ensures next dispatch triggers clean temporal reset
    return true;
}

void destroySizedResources(App& app)
{
    app.ctx.waitIdle();
    app.profiler.onContextReset();
    app.upscaler.destroy();
    app.scene.destroy(app.ctx);
}

SwapchainRebuildResult rebuildSwapchainResources(App& app)
{
    if (!app.window) {
        return SwapchainRebuildResult::Minimized;
    }
    if (app.ctx.sync().isTerminalError()) {
        LOGE("Terminal Vulkan error (%s); cannot rebuild Android swapchain",
             app.ctx.sync().isDeviceLost() ? "device lost" :
             app.ctx.sync().isSurfaceLost() ? "surface lost" : "sync recovery failed");
        return SwapchainRebuildResult::Failed;
    }

    destroySizedResources(app);
    app.renderReady = false;

    if (!app.ctx.recreateSwapchain(app.window)) {
        if (app.ctx.sync().isTerminalError()) {
            LOGE("Terminal Vulkan error (%s) during Android swapchain recreation; aborting",
                 app.ctx.sync().isDeviceLost() ? "device lost" :
                 app.ctx.sync().isSurfaceLost() ? "surface lost" : "sync recovery failed");
        } else {
            LOGE("Android swapchain recreation failed");
        }
        return SwapchainRebuildResult::Failed;
    }
    if (!createSizedResources(app)) {
        LOGE("Could not rebuild Android resources after swapchain recreation");
        return SwapchainRebuildResult::Failed;
    }
    app.lastTimeSeconds = nowSeconds();
    app.renderReady     = true;
    app.swapchainDirty  = false;
    return SwapchainRebuildResult::Success;
}

bool initRendering(App& app, ANativeWindow* window)
{
    app.window = window;
    if (!app.ctx.init(window)) {
        LOGE("Vulkan initialisation failed");
        return false;
    }
    app.vulkanReady = true;

    const DeviceCapabilities& caps = app.ctx.caps();
    if (!caps.sufficientForArmAsr()) {
        LOGE("This device cannot run FSR 3.1.4's prebuilt shaders.");
        LOGE("  They are SPIR-V 1.4 with explicit 16-bit types and require extended storage image formats.");
        LOGE("  Missing:%s%s%s%s%s%s%s",
             caps.spirv14 ? "" : " [SPIR-V 1.4 / Vulkan 1.2]",
             caps.shaderFloat16 ? "" : " [shaderFloat16]",
             caps.storage16bit ? "" : " [16-bit storage]",
             caps.storageExtendedFormats ? "" : " [shaderStorageImageExtendedFormats]",
             caps.storageWriteWithoutFormat ? "" : " [shaderStorageImageWriteWithoutFormat]",
             caps.storageFormatsSupported ? "" : " [storage format support]",
             caps.sampledFormatsSupported ? "" : " [sampled format support]");
        LOGE("  The x86_64 emulator (SwiftShader) is expected to fail here.");
        return false;
    }

    if (!createSizedResources(app))
        return false;

    app.profiler.initFromEnvironmentOrDefaults();
    app.lastTimeSeconds = nowSeconds();
    app.renderReady     = true;
    app.swapchainDirty  = false;
    LOGI("FSR 3.1.4 sample ready");
    return true;
}

void teardownRendering(App& app)
{
    if (app.vulkanReady)
        app.ctx.waitIdle();
    if (app.profiler.sampleCount() > 0 && !app.profiler.isComplete()) {
        app.profiler.saveReports(app.ctx, app.renderExtent, app.displayExtent);
    }
    app.profiler.onContextReset();
    app.upscaler.destroy();
    if (app.vulkanReady)
        app.scene.destroy(app.ctx);
    app.ctx.destroy();
    app.vulkanReady = false;
    app.renderReady = false;
    app.window = nullptr;
}

void drawFrame(App& app)
{
    if (app.swapchainDirty) {
        SwapchainRebuildResult res = rebuildSwapchainResources(app);
        if (app.ctx.sync().isTerminalError()) {
            LOGE("Terminal Vulkan error (%s); aborting Android rendering loop immediately",
                 app.ctx.sync().isDeviceLost() ? "device lost" :
                 app.ctx.sync().isSurfaceLost() ? "surface lost" : "sync recovery failed");
            app.renderReady = false;
            app.renderingTerminated = true;
            return;
        }
        if (!SwapchainRecoveryPolicy::handleRebuildResult(res, app.swapchainDirty, app.consecutiveRecreateFailures)) {
            LOGE("Repeated swapchain recreation failure on Android (%u consecutive failures); aborting rendering loop",
                 app.consecutiveRecreateFailures);
            app.renderReady = false;
            app.renderingTerminated = true;
            return;
        }
        if (res == SwapchainRebuildResult::Failed || res == SwapchainRebuildResult::Minimized) {
            return;
        }
    }

    if (!app.renderReady)
        return;

    const double now   = nowSeconds();
    float        delta = (float)(now - app.lastTimeSeconds);
    app.lastTimeSeconds = now;
    delta = fminf(fmaxf(delta, 1.0f / 240.0f), 1.0f / 15.0f);

    app.camZPrev = app.camZ;
    app.camZ -= 6.0f * delta;   // fly forward

    uint32_t imageIndex = 0;
    if (!app.ctx.beginFrame(&imageIndex)) {
        if (app.ctx.sync().isTerminalError()) {
            LOGE("Terminal Vulkan error (%s) during Android beginFrame; aborting rendering loop",
                 app.ctx.sync().isDeviceLost() ? "device lost" :
                 app.ctx.sync().isSurfaceLost() ? "surface lost" : "sync recovery failed");
            app.renderReady = false;
            app.renderingTerminated = true;
            return;
        }
        app.swapchainDirty = true;
        return;
    }

    const uint32_t frameSlot = app.ctx.currentFrameSlot();
    VkCommandBuffer cmd = app.ctx.commandBuffer();

    arm::FfxmVkProfilingDescription profilingDesc{};
    const bool profilingActive = app.profiler.onBeginFrame(frameSlot, cmd, app.ctx, &profilingDesc);

    if (app.profiler.isComplete()) {
        app.profiler.saveReports(app.ctx, app.renderExtent, app.displayExtent);
    }

    float jitterX = 0.0f, jitterY = 0.0f;
    app.upscaler.jitterForFrame(app.frameIndex, &jitterX, &jitterY);

    ScenePushConstants pc = {};
    pc.renderSize[0] = (float)app.renderExtent.width;
    pc.renderSize[1] = (float)app.renderExtent.height;
    pc.jitter[0]     = jitterX;
    pc.jitter[1]     = jitterY;
    pc.camZ          = app.camZ;
    pc.camZPrev      = app.camZPrev;
    pc.tanHalfFov    = tanf(1.0471975512f * 0.5f);
    pc.aspect        = (float)app.renderExtent.width / (float)app.renderExtent.height;
    pc.nearZ         = 0.1f;
    pc.farZ          = 200.0f;
    pc.planeY        = -2.0f;
    pc.time          = (float)now;

    app.scene.record(cmd, pc);
    app.scene.barrierForUpscale(cmd);

    const bool dispatchOk = app.upscaler.dispatch(cmd, app.scene, app.renderExtent, delta * 1000.0f, app.frameIndex,
                                                  app.frameIndex == 0,
                                                  profilingActive ? &profilingDesc : nullptr);
    if (!dispatchOk) {
        app.profiler.onFrameAborted(frameSlot);
        LOGE("FSR 3.1.4 dispatch failed at frame %d; recovering via abortFrame", app.frameIndex);
        if (!app.ctx.abortFrame(imageIndex)) {
            if (app.ctx.sync().isTerminalError()) {
                LOGE("Terminal Vulkan error (%s) during Android abortFrame; aborting rendering loop",
                     app.ctx.sync().isDeviceLost() ? "device lost" :
                     app.ctx.sync().isSurfaceLost() ? "surface lost" : "sync recovery failed");
                app.renderReady = false;
                app.renderingTerminated = true;
                return;
            }
            app.swapchainDirty = true;
        }
        return;
    }

    app.scene.recordBlit(cmd, app.ctx.swapchainImageView(imageIndex),
                         app.ctx.swapchainExtent());

    if (!app.ctx.endFrame(imageIndex)) {
        app.profiler.onFrameAborted(frameSlot);
        if (app.ctx.sync().isTerminalError()) {
            LOGE("Terminal Vulkan error (%s) during Android endFrame; aborting rendering loop",
                 app.ctx.sync().isDeviceLost() ? "device lost" :
                 app.ctx.sync().isSurfaceLost() ? "surface lost" : "sync recovery failed");
            app.renderReady = false;
            app.renderingTerminated = true;
            return;
        }
        app.swapchainDirty = true;
    } else {
        if (profilingActive) {
            app.profiler.onFrameSubmitted(frameSlot, profilingDesc.mode, app.frameIndex);
        }
    }

    if (app.frameIndex == 0)
        LOGI("First FSR 3.1.4 frame dispatched successfully");
    ++app.frameIndex;
}

void handleCommand(android_app* androidApp, int32_t cmd)
{
    App& app = *static_cast<App*>(androidApp->userData);

    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        if (androidApp->window) {
            app.window = androidApp->window;
            app.renderingTerminated = false;
            if (!app.vulkanReady) {
                if (!initRendering(app, androidApp->window))
                    teardownRendering(app);
            } else {
                app.swapchainDirty = true;
            }
        }
        break;
    case APP_CMD_TERM_WINDOW:
        teardownRendering(app);
        break;
    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONFIG_CHANGED:
        if (app.vulkanReady && app.window) {
            int32_t w = ANativeWindow_getWidth(app.window);
            int32_t h = ANativeWindow_getHeight(app.window);
            if (w > 0 && h > 0 && (uint32_t)w == app.displayExtent.width && (uint32_t)h == app.displayExtent.height) {
                break;
            }
        }
        app.swapchainDirty = true;
        break;
    default:
        break;
    }
}

} // namespace

void android_main(android_app* androidApp)
{
    auto app = std::make_unique<App>();
    androidApp->userData     = app.get();
    androidApp->onAppCmd     = handleCommand;

    while (true) {
        int                   events = 0;
        android_poll_source*  source = nullptr;

        int timeoutMs = (app->renderReady && !app->swapchainDirty && !app->renderingTerminated && app->window) ? 0 :
                        (app->swapchainDirty ? 16 : -1);

        while (ALooper_pollOnce(timeoutMs, nullptr, &events,
                                reinterpret_cast<void**>(&source)) >= 0) {
            if (source)
                source->process(androidApp, source);
            if (androidApp->destroyRequested) {
                teardownRendering(*app);
                return;
            }
            // Once an event is processed, drain remaining pending events without blocking.
            timeoutMs = 0;
        }

        if (!app->renderingTerminated && app->window && app->renderReady) {
            drawFrame(*app);
        }
    }
}
