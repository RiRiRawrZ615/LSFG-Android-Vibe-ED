/* Shared display queue between the framegen thread and the WSI presenter.
 * Framegen pushes AHardwareBuffer pointers of frames ready for presentation;
 * the WSI render thread pops them vsync-paced. */

#pragma once

#include <cstdint>
#include <mutex>
#include <condition_variable>
#include <deque>

namespace lsfgdisplay {

    /// Push a frame (AHardwareBuffer*) for presentation
    void push(uintptr_t ahbPtr);

    /// Pop a frame, waiting up to timeoutMs. Returns 0 if none available.
    uintptr_t pop(int timeoutMs);

    /// Clear the queue (on session stop)
    void clear();

    /// Number of queued frames
    int size();

    /// Stop the current frame batch sequence marker (for pacing resets)
    void reset();

} // namespace lsfgdisplay
