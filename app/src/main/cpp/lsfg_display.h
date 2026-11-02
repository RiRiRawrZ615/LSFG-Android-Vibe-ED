/* Shared display queue between the framegen thread and the WSI presenter.
 * Framegen pushes AHardwareBuffer pointers of frames ready for presentation;
 * the WSI render thread pops them vsync-paced.
 *
 * Ownership: push() acquires one AHardwareBuffer reference on behalf of the
 * queue (the producer may release its own reference immediately after
 * pushing). pop() transfers that reference to the caller, which must release
 * it once the GPU is done with the frame. clear() releases queued references. */

#pragma once

#include <cstdint>
#include <mutex>
#include <condition_variable>
#include <deque>

namespace lsfgdisplay {

    /// Push a frame (AHardwareBuffer*) for presentation. Acquires a reference.
    void push(uintptr_t ahbPtr);

    /// Pop a frame, waiting up to timeoutMs. Returns 0 if none available.
    /// The caller owns one AHardwareBuffer reference and must release it.
    uintptr_t pop(int timeoutMs);

    /// Clear the queue (on session stop)
    void clear();

    /// Number of queued frames
    int size();

    /// Stop the current frame batch sequence marker (for pacing resets)
    void reset();

} // namespace lsfgdisplay
