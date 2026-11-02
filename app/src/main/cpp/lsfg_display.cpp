#include "lsfg_display.h"

#include <android/hardware_buffer.h>

#include <algorithm>
#include <deque>

namespace {
    std::mutex g_mutex;
    std::condition_variable g_cv;
    std::deque<uintptr_t> g_queue;
    constexpr size_t kMaxQueue = 64;
}

namespace lsfgdisplay {

void push(uintptr_t ahbPtr) {
    if (!ahbPtr)
        return;
    // Acquire the queue's reference BEFORE enqueueing so the producer may
    // release its own reference (e.g. close the Java HardwareBuffer) right
    // after pushing without freeing a buffer that is still queued.
    AHardwareBuffer_acquire(reinterpret_cast<AHardwareBuffer*>(ahbPtr));
    uintptr_t dropped{0};
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_queue.size() >= kMaxQueue) {
            dropped = g_queue.front();
            g_queue.pop_front(); // drop oldest (backpressure)
        }
        g_queue.push_back(ahbPtr);
    }
    if (dropped)
        AHardwareBuffer_release(reinterpret_cast<AHardwareBuffer*>(dropped));
    g_cv.notify_one();
}

uintptr_t pop(int timeoutMs) {
    std::unique_lock<std::mutex> lock(g_mutex);
    if (g_queue.empty()) {
        if (timeoutMs > 0)
            g_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs));
        else
            g_cv.wait(lock, [] { return !g_queue.empty(); });
    }
    if (g_queue.empty())
        return 0;
    uintptr_t item = g_queue.front();
    g_queue.pop_front();
    // The queue's reference is transferred to the caller, who must release it
    // once the GPU is done with the frame.
    return item;
}

void clear() {
    std::deque<uintptr_t> queued;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        queued.swap(g_queue);
    }
    for (uintptr_t item : queued)
        AHardwareBuffer_release(reinterpret_cast<AHardwareBuffer*>(item));
}

int size() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return static_cast<int>(g_queue.size());
}

void reset() { clear(); }

} // namespace lsfgdisplay
