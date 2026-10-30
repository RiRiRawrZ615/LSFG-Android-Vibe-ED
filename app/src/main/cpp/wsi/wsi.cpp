/* LSFG-Fork WSI presenter */

#define VK_NO_PROTOTYPES
#include <jni.h>
#include <android/hardware_buffer_jni.h>
#include <vulkan/vulkan.h>
#include <android/log.h>
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#include "volk.h"
#include "lsfg_display.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#define LOG_TAG "lsfg-wsi"
#define WLOG(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define WLOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// --- Native crash diagnostics -------------------------------------------------
// When the Vulkan/driver path segfaults (which is what kills the app ~1.3s after
// 'beginSession'), the Android tombstone usually only says "Process died" and
// the real call stack is lost.  Install a SIGSEGV/SIGBUS/SIGABRT handler that
// logs a one-line marker to logcat (tag "lsfg-wsi") so the user's next crash
// dump tells us EXACTLY which native step died and the fault address.  The
// handler is minimal (async-signal-safe: only __android_log_print, no malloc /
// std::string / printf) and always re-raises the signal so the normal Android
// crash dump is still produced.
#include <csignal>
#include <string.h>
#include <cstddef>
static void nativeCrashHandler(int sig, siginfo_t* info, void* ctx) {
    // Async-signal-safe: __android_log_print only.  No file I/O (the NDK 'open'
    // and 'write' collide with other headers and fail to compile in this
    // environment).  logcat is the canonical crash sink and is already captured.
    //
    // Format the signal number + fault address as a fixed-width string without
    // malloc / snprintf / std::string (not async-signal-safe).
    char line[96];
    int n = 0;
    const char* pre = "=== LSFG NATIVE CRASH === sig=";
    for (const char* p = pre; *p && n < (int)sizeof(line) - 2; ++p) line[n++] = *p;
    // signal number (decimal)
    int v = sig;
    char num[16]; int m = 0;
    do { num[m++] = (char)('0' + (v % 10)); v /= 10; } while (v && m < 15);
    for (int i = m - 1; i >= 0; --i) line[n++] = num[i];
    const char* mid = " fault_addr=0x";
    for (const char* p = mid; *p && n < (int)sizeof(line) - 2; ++p) line[n++] = *p;
    // fault address as 16 hex digits
    uintptr_t a = (uintptr_t)(info ? info->si_addr : nullptr);
    for (int i = 15; i >= 0 && n < (int)sizeof(line) - 2; --i) {
        int d = (int)((a >> (i * 4)) & 0xF);
        line[n++] = (char)(d < 10 ? ('0' + d) : ('a' + d - 10));
    }
    line[n] = '\0';
    __android_log_print(ANDROID_LOG_ERROR, "lsfg-wsi", "%s", line);
    // Re-raise so Android still produces its normal tombstone.
    signal(sig, SIG_DFL);
    raise(sig);
}
static void installCrashHandler() {
    struct sigaction sa{};
    sa.sa_sigaction = nativeCrashHandler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
}
// --- end native crash diagnostics -------------------------------------------

#define VK_CHECK(expr) do { \
        VkResult r_ = (expr); \
        if (r_ != VK_SUCCESS) { \
            WLOGE("Vulkan error %d at %s:%s (line %d)", (int)r_, #expr, __FILE__, __LINE__); \
            throw std::runtime_error(std::string("vk error: ") + #expr); \
        } \
    } while (0)

namespace {

    struct Presenter {
        VkInstance instance{};
        VkPhysicalDevice physdev{};
        VkDevice device{};
        VkQueue queue{};
        uint32_t qfi{0};
        uint32_t swapchainCount{0};
        VkFormat swapchainFormat{};
        VkSwapchainKHR swapchain{};
        VkSurfaceKHR surface{};
        VkImage swapchainImages[8]{};
        VkImageView swapchainViews[8]{};
        VkSemaphore imageAcquired{};
        VkSemaphore frameReady{};
        VkFence presentFence{};
        VkCommandPool cmdPool{};

        std::map<uintptr_t, VkImage> ahbImages;
        std::map<uintptr_t, VkImageView> ahbViews;

        ANativeWindow* window{nullptr};
        uint32_t width{0};
        uint32_t height{0};
        bool surfaceValid{false};

        std::thread thread;
        std::atomic<bool> running{false};
        std::atomic<bool> paused{false};

        float targetFps{60.0f};
        uint64_t framesPresented{0};
        bool constructed{false};
    };

    Presenter g_presenter;
    std::mutex g_initLock;

    void stopPresenter() {
        if (!g_presenter.running) return;
        g_presenter.running = false;
        if (g_presenter.thread.joinable())
            g_presenter.thread.join();
        g_presenter.paused = false;
        lsfgdisplay::clear();
        vkDeviceWaitIdle(g_presenter.device);
        for (auto& [key, view] : g_presenter.ahbViews)
            vkDestroyImageView(g_presenter.device, view, nullptr);
        g_presenter.ahbViews.clear();
        for (auto& [key, image] : g_presenter.ahbImages)
            vkDestroyImage(g_presenter.device, image, nullptr);
        g_presenter.ahbImages.clear();
        for (uint32_t i = 0; i < g_presenter.swapchainCount; i++) {
            if (g_presenter.swapchainViews[i])
                vkDestroyImageView(g_presenter.device, g_presenter.swapchainViews[i], nullptr);
            g_presenter.swapchainViews[i] = VK_NULL_HANDLE;
        }
        if (g_presenter.swapchain)
            vkDestroySwapchainKHR(g_presenter.device, g_presenter.swapchain, nullptr);
        g_presenter.swapchain = VK_NULL_HANDLE;
        if (g_presenter.cmdPool)
            vkDestroyCommandPool(g_presenter.device, g_presenter.cmdPool, nullptr);
        g_presenter.cmdPool = VK_NULL_HANDLE;
        if (g_presenter.presentFence)
            vkDestroyFence(g_presenter.device, g_presenter.presentFence, nullptr);
        if (g_presenter.imageAcquired)
            vkDestroySemaphore(g_presenter.device, g_presenter.imageAcquired, nullptr);
        if (g_presenter.frameReady)
            vkDestroySemaphore(g_presenter.device, g_presenter.frameReady, nullptr);
        g_presenter.framesPresented = 0;
        if (g_presenter.device)
            vkDestroyDevice(g_presenter.device, nullptr);
        g_presenter.device = VK_NULL_HANDLE;
        if (g_presenter.surface)
            vkDestroySurfaceKHR(g_presenter.instance, g_presenter.surface, nullptr);
        g_presenter.surface = VK_NULL_HANDLE;
        if (g_presenter.instance)
            vkDestroyInstance(g_presenter.instance, nullptr);
        g_presenter.instance = VK_NULL_HANDLE;
        g_presenter.surfaceValid = false;
        g_presenter.window = nullptr;
        WLOG("presenter stopped");
    }

    void createSwapchain(uint32_t width, uint32_t height) {
        if (g_presenter.swapchain)
            vkDestroySwapchainKHR(g_presenter.device, g_presenter.swapchain, nullptr);

        VkSurfaceFormatKHR formats[16];
        uint32_t formatCount = 16;
        VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(g_presenter.physdev, g_presenter.surface, &formatCount, formats));
        VkSurfaceCapabilitiesKHR caps;
        VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_presenter.physdev, g_presenter.surface, &caps));

        VkSurfaceFormatKHR chosen = formats[0];
        if (formatCount > 1) {
            for (uint32_t i = 0; i < formatCount; i++) {
                if (formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
                    chosen = formats[i];
                    break;
                }
            }
        }
        g_presenter.swapchainFormat = chosen.format;

        uint32_t count = caps.minImageCount + 1;
        if (caps.maxImageCount > 0 && count > caps.maxImageCount)
            count = caps.maxImageCount;
        count = std::min(count, 8u);

        VkSwapchainCreateInfoKHR sci{};
        sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        sci.surface = g_presenter.surface;
        sci.minImageCount = count;
        sci.imageFormat = chosen.format;
        sci.imageColorSpace = chosen.colorSpace;
        sci.imageExtent = { width, height };
        sci.imageArrayLayers = 1;
        sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sci.preTransform = caps.currentTransform;
        sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        sci.clipped = VK_TRUE;

        VK_CHECK(vkCreateSwapchainKHR(g_presenter.device, &sci, nullptr, &g_presenter.swapchain));

        uint32_t imageCount = 0;
        VK_CHECK(vkGetSwapchainImagesKHR(g_presenter.device, g_presenter.swapchain, &imageCount, nullptr));
        std::vector<VkImage> images(imageCount);
        VK_CHECK(vkGetSwapchainImagesKHR(g_presenter.device, g_presenter.swapchain, &imageCount, images.data()));
        g_presenter.swapchainCount = imageCount;
        for (uint32_t i = 0; i < imageCount; i++) {
            g_presenter.swapchainImages[i] = images[i];
            VkImageViewCreateInfo vci{};
            vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vci.image = images[i];
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = chosen.format;
            vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            VK_CHECK(vkCreateImageView(g_presenter.device, &vci, nullptr, &g_presenter.swapchainViews[i]));
        }

        g_presenter.width = width;
        g_presenter.height = height;
        g_presenter.surfaceValid = true;
        WLOG("swapchain created: %ux%u, %u images, format=%d", width, height, imageCount, (int)chosen.format);
    }

    VkImage importAhbLocked(const void* ahb) {
        uintptr_t key = reinterpret_cast<uintptr_t>(ahb);
        auto it = g_presenter.ahbImages.find(key);
        if (it != g_presenter.ahbImages.end())
            return it->second;

        AHardwareBuffer* buffer = const_cast<AHardwareBuffer*>(reinterpret_cast<const AHardwareBuffer*>(ahb));
        AHardwareBuffer_Desc desc;
        AHardwareBuffer_describe(buffer, &desc);

        VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
        switch (desc.format) {
            case AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM:          fmt = VK_FORMAT_R8G8B8A8_UNORM; break;
            case AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM:      fmt = VK_FORMAT_A2B10G10R10_UNORM_PACK32; break;
            case AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT:     fmt = VK_FORMAT_R16G16B16A16_SFLOAT; break;
            case AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM:           fmt = VK_FORMAT_R8G8B8_UNORM; break;
            default: break;
        }

        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = fmt;
        ici.extent = { static_cast<uint32_t>(desc.width), static_cast<uint32_t>(desc.height), 1 };
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkImage image = VK_NULL_HANDLE;
        VK_CHECK(vkCreateImage(g_presenter.device, &ici, nullptr, &image));

        VkAndroidHardwareBufferPropertiesANDROID props{};
        props.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
        vkGetAndroidHardwareBufferPropertiesANDROID(g_presenter.device, buffer, &props);

        VkMemoryRequirements mreq;
        vkGetImageMemoryRequirements(g_presenter.device, image, &mreq);

        uint32_t usable = mreq.memoryTypeBits & props.memoryTypeBits;
        if (!usable) usable = mreq.memoryTypeBits;
        uint32_t memTypeIndex = 0;
        for (uint32_t i = 0; i < 64; ++i) {
            if (usable & (1u << i)) { memTypeIndex = i; break; }
        }

        VkImportAndroidHardwareBufferInfoANDROID importInfo{};
        importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
        importInfo.buffer = buffer;

        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.pNext = &importInfo;
        mai.allocationSize = mreq.size;
        mai.memoryTypeIndex = memTypeIndex;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VK_CHECK(vkAllocateMemory(g_presenter.device, &mai, nullptr, &mem));
        VK_CHECK(vkBindImageMemory(g_presenter.device, image, mem, 0));

        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = fmt;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageView view = VK_NULL_HANDLE;
        VK_CHECK(vkCreateImageView(g_presenter.device, &vci, nullptr, &view));

        g_presenter.ahbImages[key] = image;
        g_presenter.ahbViews[key] = view;
        WLOG("imported AHB %p (%dx%d)", ahb, desc.width, desc.height);
        return image;
    }

    void presentLoop() {
        using namespace std::chrono_literals;
        while (g_presenter.running) {
            // Pop a source frame (non-blocking-ish, with a short timeout so the
            // loop can also service 'paused' and shutdown promptly).
            int waitMs = std::max(1, static_cast<int>(1000.0 / g_presenter.targetFps));
            uintptr_t frame = lsfgdisplay::pop(waitMs);
            if (!frame) {
                if (g_presenter.paused) std::this_thread::sleep_for(5ms);
                continue;
            }
            if (!g_presenter.surfaceValid) continue;

            VkCommandBuffer cmdbuf = VK_NULL_HANDLE;
            try {
                VkImage srcImage = importAhbLocked(reinterpret_cast<const void*>(frame));
                if (!srcImage) continue;

                // Non-blocking acquire: 0 timeout returns VK_TIMEOUT if the
                // swapchain has no free image right now.  Using UINT64_MAX here
                // was a hard hang (the loop would block forever on a busy / 
                // out-of-date swapchain).  0 is the correct 'poll' value.
                uint32_t imageIndex = 0;
                VkResult ar = vkAcquireNextImageKHR(
                    g_presenter.device, g_presenter.swapchain, 0,
                    g_presenter.imageAcquired, VK_NULL_HANDLE, &imageIndex);
                if (ar == VK_TIMEOUT || ar == VK_NOT_READY) {
                    std::this_thread::sleep_for(2ms);
                    continue;
                }
                if (ar == VK_SUBOPTIMAL_KHR || ar == VK_ERROR_OUT_OF_DATE_KHR) {
                    createSwapchain(g_presenter.width, g_presenter.height);
                    continue;
                }
                if (ar != VK_SUCCESS) { WLOGE("acquire=%d", (int)ar); continue; }

                vkResetCommandPool(g_presenter.device, g_presenter.cmdPool, 0);
                VkCommandBufferAllocateInfo cabi{};
                cabi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                cabi.commandPool = g_presenter.cmdPool;
                cabi.commandBufferCount = 1;
                vkAllocateCommandBuffers(g_presenter.device, &cabi, &cmdbuf);
                VkCommandBufferBeginInfo bci{};
                bci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                bci.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkBeginCommandBuffer(cmdbuf, &bci);

                VkImageMemoryBarrier ib{};
                ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                ib.srcAccessMask = 0;
                ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                ib.image = g_presenter.swapchainImages[imageIndex];
                ib.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                vkCmdPipelineBarrier(cmdbuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);

                AHardwareBuffer_Desc desc{};
                AHardwareBuffer_describe(const_cast<AHardwareBuffer*>(reinterpret_cast<const AHardwareBuffer*>(frame)), &desc);

                VkImageCopy region{};
                region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.srcOffset = { 0, 0, 0 };
                region.dstOffset = { 0, 0, 0 };
                region.extent = { desc.width, desc.height, 1 };
                vkCmdCopyImage(cmdbuf, srcImage, VK_IMAGE_LAYOUT_GENERAL,
                    g_presenter.swapchainImages[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

                ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                ib.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                ib.dstAccessMask = 0;
                vkCmdPipelineBarrier(cmdbuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);

                vkEndCommandBuffer(cmdbuf);

                VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                VkSubmitInfo si{};
                si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                si.waitSemaphoreCount = 1;
                si.pWaitSemaphores = &g_presenter.imageAcquired;
                si.pWaitDstStageMask = &waitStage;
                si.commandBufferCount = 1;
                si.pCommandBuffers = &cmdbuf;
                if (vkQueueSubmit(g_presenter.queue, 1, &si, g_presenter.presentFence) != VK_SUCCESS) {
                    vkFreeCommandBuffers(g_presenter.device, g_presenter.cmdPool, 1, &cmdbuf);
                    cmdbuf = VK_NULL_HANDLE;
                    continue;
                }

                VkPresentInfoKHR pi{};
                pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
                // CRITICAL FIX (was the root cause of the ~5s freeze + kill):
                // vkQueuePresentKHR must NOT wait on a semaphore that no
                // command buffer signals.  The old code waited on frameReady
                // (created but never signaled) -> the GPU present hung forever
                // -> the app was killed.  Present is allowed to start as soon
                // as the submitted work finishes; the fence below provides the
                // real completion signal.
                pi.waitSemaphoreCount = 0;
                pi.pWaitSemaphores = nullptr;
                pi.swapchainCount = 1;
                static VkSwapchainKHR chains[1];
                chains[0] = g_presenter.swapchain;
                static uint32_t indices[1];
                indices[0] = imageIndex;
                pi.pSwapchains = chains;
                pi.pImageIndices = indices;
                vkQueuePresentKHR(g_presenter.queue, &pi);

                // Wait (finite timeout) until the GPU is done with this frame
                // before reusing any of its resources on the next iteration.
                VkResult wr = vkWaitForFences(g_presenter.device, 1, &g_presenter.presentFence, VK_TRUE, 3000000000ull);
                if (wr == VK_SUCCESS) vkResetFences(g_presenter.device, 1, &g_presenter.presentFence);
                cmdbuf = VK_NULL_HANDLE; // consumed by the GPU; don't free
                g_presenter.framesPresented++;
            } catch (const std::exception& e) {
                WLOGE("present loop: %s", e.what());
                std::this_thread::sleep_for(10ms);
            }
            if (cmdbuf) vkFreeCommandBuffers(g_presenter.device, g_presenter.cmdPool, 1, &cmdbuf);
        }
    }
} // namespace

extern "C" {

JNIEXPORT void JNICALL
Java_com_lsfg_fork_wsi_WsiBridge_nativeStart(JNIEnv* env, jclass, jlong windowPtr, jint width, jint height, jfloat targetFps) {
    std::lock_guard<std::mutex> lock(g_initLock);
    if (g_presenter.running) return;
    installCrashHandler();  // install once, before any Vulkan/driver work
    try {
        WLOG("starting presenter %dx%d targetFps=%.1f", width, height, static_cast<double>(targetFps));

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "LSFG-Fork";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "lsfg-wsi";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_1;
        const char* exts[] = { "VK_KHR_surface", "VK_KHR_android_surface" };
        VkInstanceCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ici.pApplicationInfo = &appInfo;
        ici.enabledExtensionCount = 2;
        ici.ppEnabledExtensionNames = exts;
        VK_CHECK(vkCreateInstance(&ici, nullptr, &g_presenter.instance));
        volkLoadInstance(g_presenter.instance);

        uint32_t deviceCount = 1;
        vkEnumeratePhysicalDevices(g_presenter.instance, &deviceCount, nullptr);
        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(g_presenter.instance, &deviceCount, devices.data());
        g_presenter.physdev = devices[0];

        float priority = 1.0f;
        VkDeviceQueueCreateInfo dqci{};
        dqci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        dqci.queueFamilyIndex = 0;
        dqci.queueCount = 1;
        dqci.pQueuePriorities = &priority;
        const char* devExts[] = { "VK_KHR_swapchain", "VK_ANDROID_external_memory_android_hardware_buffer" };
        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &dqci;
        dci.enabledExtensionCount = 2;
        dci.ppEnabledExtensionNames = devExts;
        VK_CHECK(vkCreateDevice(g_presenter.physdev, &dci, nullptr, &g_presenter.device));
        volkLoadDevice(g_presenter.device);
        vkGetDeviceQueue(g_presenter.device, 0, 0, &g_presenter.queue);

        VkSemaphoreCreateInfo semCi{};
        semCi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VK_CHECK(vkCreateSemaphore(g_presenter.device, &semCi, nullptr, &g_presenter.imageAcquired));
        VK_CHECK(vkCreateSemaphore(g_presenter.device, &semCi, nullptr, &g_presenter.frameReady));
        VkFenceCreateInfo fenceCi{};
        fenceCi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VK_CHECK(vkCreateFence(g_presenter.device, &fenceCi, nullptr, &g_presenter.presentFence));
        VkCommandPoolCreateInfo poolCi{};
        poolCi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolCi.queueFamilyIndex = 0;
        VK_CHECK(vkCreateCommandPool(g_presenter.device, &poolCi, nullptr, &g_presenter.cmdPool));

        ANativeWindow* window = reinterpret_cast<ANativeWindow*>(windowPtr);
        g_presenter.window = window;
        VkAndroidSurfaceCreateInfoKHR sci{};
        sci.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        sci.window = window;
        vkCreateAndroidSurfaceKHR(g_presenter.instance, &sci, nullptr, &g_presenter.surface);

        int32_t winW = ANativeWindow_getWidth(window);
        int32_t winH = ANativeWindow_getHeight(window);
        if (winW <= 0 || winH <= 0) {
            winW = static_cast<int32_t>(width);
            winH = static_cast<int32_t>(height);
        }
        g_presenter.targetFps = targetFps > 1.0f ? targetFps : 60.0f;
        createSwapchain(static_cast<uint32_t>(winW), static_cast<uint32_t>(winH));

        g_presenter.running = true;
        g_presenter.constructed = true;
        g_presenter.thread = std::thread(presentLoop);
        WLOG("presenter started");
    } catch (const std::exception& e) {
        WLOGE("nativeStart failed: %s", e.what());
        g_presenter.running = false;
    }
}

JNIEXPORT void JNICALL
Java_com_lsfg_fork_wsi_WsiBridge_nativeStop(JNIEnv*, jclass) {
    ::stopPresenter();
}

JNIEXPORT jboolean JNICALL
Java_com_lsfg_fork_wsi_WsiBridge_nativeIsRunning(JNIEnv*, jclass) {
    return g_presenter.running ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_lsfg_fork_wsi_WsiBridge_nativeSetPaused(JNIEnv*, jclass, jboolean paused) {
    g_presenter.paused = paused ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_lsfg_fork_wsi_WsiBridge_nativeSetTargetFps(JNIEnv*, jclass, jfloat fps) {
    if (fps > 1.0f) g_presenter.targetFps = fps;
}

JNIEXPORT jlong JNICALL
Java_com_lsfg_fork_wsi_WsiBridge_nativeAhbPtr(JNIEnv* env, jclass, jobject obj) {
    if (!obj) return 0;
    AHardwareBuffer* ahb = AHardwareBuffer_fromHardwareBuffer(env, obj);
    return reinterpret_cast<jlong>(ahb);
}

JNIEXPORT void JNICALL
Java_com_lsfg_fork_wsi_WsiBridge_nativePushFrame(JNIEnv*, jclass, jlong ahbPtr) {
    if (ahbPtr)
        lsfgdisplay::push(static_cast<uintptr_t>(ahbPtr));
}

JNIEXPORT jint JNICALL
Java_com_lsfg_fork_wsi_WsiBridge_nativeQueueSize(JNIEnv*, jclass) {
    return lsfgdisplay::size();
}

/** Number of frames the presenter has actually handed to vkQueuePresentKHR.
 *  Used by the in-app pipeline self-test to report an HONEST pass/fail
 *  (instead of claiming success when the Vulkan path never produced a frame). */
JNIEXPORT jlong JNICALL
Java_com_lsfg_fork_wsi_WsiBridge_nativeFramesPresented(JNIEnv*, jclass) {
    return (jlong)g_presenter.framesPresented;
}

/**
 * Fast CPU fill of a HardwareBuffer ring slot from a raw RGBA8888 byte source
 * (e.g. an ImageReader RGBA_8888 plane from a MediaProjection VirtualDisplay).
 * Copies row by row, respecting the source row stride.
 */
JNIEXPORT void JNICALL
Java_com_lsfg_fork_wsi_WsiBridge_nativeFillAhb(
        JNIEnv* env, jclass, jobject ahbObj, jbyteArray bytes, jint srcRowStride) {
    if (!ahbObj || !bytes || srcRowStride <= 0) return;
    AHardwareBuffer* ahb = AHardwareBuffer_fromHardwareBuffer(env, ahbObj);
    if (!ahb) return;
    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(ahb, &desc);
    if (desc.width == 0 || desc.height == 0) return;
    void* data = nullptr;
    AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY,
                         /*fence*/ 0, /*rect*/ nullptr, &data);
    if (!data) return;
    uint8_t* src = static_cast<uint8_t*>(env->GetPrimitiveArrayCritical(bytes, nullptr));
    if (src) {
        const uint32_t rowBytes = desc.width * 4; // RGBA8888 (4 bytes/pixel)
        uint8_t* d = static_cast<uint8_t*>(data);
        for (uint32_t y = 0; y < desc.height; ++y) {
            std::memcpy(d + y * static_cast<size_t>(desc.stride),
                        src + y * static_cast<size_t>(srcRowStride),
                        rowBytes);
        }
        env->ReleasePrimitiveArrayCritical(bytes, src, JNI_ABORT);
    }
    AHardwareBuffer_unlock(ahb, nullptr);
}

JNIEXPORT jlong JNICALL
Java_com_lsfg_fork_wsi_WsiBridge_nativeSurfaceToWindow(JNIEnv* env, jclass, jobject surface) {
    if (!surface) return 0;
    ANativeWindow* w = ANativeWindow_fromSurface(env, surface);
    return reinterpret_cast<jlong>(w);
}

JNIEXPORT void JNICALL
Java_com_lsfg_fork_wsi_WsiBridge_nativeReleaseWindow(JNIEnv*, jclass, jlong windowPtr) {
    if (windowPtr)
        ANativeWindow_release(reinterpret_cast<ANativeWindow*>(windowPtr));
}

} // extern "C"
