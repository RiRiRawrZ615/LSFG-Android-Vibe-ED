#include "lsfg_display.h"

#include <algorithm>

namespace {
    std::mutex g_mutex;
    std::condition_variable g_cv;
    std::deque<uintptr_t> g_queue;
    constexpr size_t kMaxQueue = 64;
}

namespace lsfgdisplay {

void push(uintptr_t ahbPtr) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_queue.size() >= kMaxQueue)
            g_queue.pop_front(); // drop oldest (backpressure)
        g_queue.push_back(ahbPtr);
    }
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
    return item;
}

void clear() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_queue.clear();
}

int size() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return static_cast<int>(g_queue.size());
}

void reset() { clear(); }

} // namespace lsfgdisplay
