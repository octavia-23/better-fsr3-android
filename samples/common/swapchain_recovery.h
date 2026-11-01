// Copyright  © 2024-2026 Arm Limited.
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

#include <cstdint>

namespace arm {

enum class SwapchainRebuildResult {
    Success,
    Failed,
    Minimized
};

struct SwapchainRecoveryPolicy {
    static constexpr uint32_t kMaxConsecutiveFailures = 10;

    /// Evaluates the outcome of a swapchain rebuild attempt.
    /// Returns true if the application should continue running (or retry/idle),
    /// or false if consecutive recreation failures exceed the threshold and the app should terminate.
    static bool handleRebuildResult(SwapchainRebuildResult result,
                                   bool& swapchainDirty,
                                   uint32_t& consecutiveFailures)
    {
        if (result == SwapchainRebuildResult::Success) {
            consecutiveFailures = 0;
            swapchainDirty = false;
            return true;
        } else if (result == SwapchainRebuildResult::Failed) {
            consecutiveFailures++;
            swapchainDirty = true; // remains dirty on failure
            if (consecutiveFailures > kMaxConsecutiveFailures) {
                return false; // threshold exceeded, terminate
            }
            return true; // retry
        } else {
            // Minimized / no window: do NOT increment failure counter, stay dirty, idle/wait
            swapchainDirty = true;
            return true;
        }
    }
};

} // namespace arm

using arm::SwapchainRebuildResult;
using arm::SwapchainRecoveryPolicy;
