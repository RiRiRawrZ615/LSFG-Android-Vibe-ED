/* LSFG-Fork framegen JNI bridge */

#include "lsfgvk.hpp"
#include "lsfg_display.h"
#include "vkhelper.hpp"
#include "jnifile_log.h"

#include <android/log.h>
#include <android/hardware_buffer.h>
#include <jni.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan.hpp>

#include <unistd.h>   // dup(), close() for the exported frame-exchange fds

#include <atomic>
#include <array>
#include <bit>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vk>

#define LOG_TAG "lsfg-framegen"
#define FLOG(...) lsfgjlog::logprintf(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define FLOGE(...) lsfgjlog::logprintf(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

    struct JniState {
        std::unique_ptr<lsfgvk::Instance> instance;
        std::unique_ptr<lsfgvk::Context> context;

        vk::detail::DispatchLoaderDynamic* dld{nullptr};
        vk::Device device{};
        vk::Queue queue{};
        uint32_t qfi{0};

        // Imported destination AHardwareBuffers (dst AHB ptr -> VkImage), cached so the
        // fixed presenter pool is imported once each.
        //
        // The backing device memory is cached alongside the image and MUST stay
        // alive for as long as the image is used. Importing the image and then
        // letting its allocation go out of scope frees the memory while the
        // image is still bound to it, and every later copy into that image traps
        // in the driver (Adreno: SIGSEGV/SEGV_MAPERR in vkCmdCopyImage).
        std::unordered_map<uintptr_t, vk::Image> dstImages;
        std::unordered_map<uintptr_t, vk::DeviceMemory> dstMemories;
        // Pre-recorded blits: dst AHB ptr -> command buffer that copies the pipeline
        // output image into that imported destination image.
        // Cached per-destination command buffers. Held as UniqueCommandBuffer so
        // the handles stay valid for the life of the state; storing a raw handle
        // from a UniqueCommandBuffer local leaves a dangling, unrecorded buffer.
        std::unordered_map<uintptr_t, vk::UniqueCommandBuffer> dstBlits;

        // Frame-exchange ring: Vulkan-created, AHB-exportable destination images.
        //
        // ringImages/ringMemory are the destination images the pipeline output is
        // copied into, one per generated frame slot. They are created by THIS
        // device with VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_
        // ANDROID so each can be exported as an AHardwareBuffer for the presenter.
        // Because the copy destination is the driver's own image, the GPU never
        // writes into app-allocated memory, which is the operation that faults on
        // Adreno at 0x80.
        std::vector<vk::Image> ringImages;
        std::vector<vk::DeviceMemory> ringMemory;
        // One pre-recorded copy command buffer per ring slot.
        std::vector<vk::UniqueCommandBuffer> ringBlits;
        // Exported AHardwareBuffer for each ring slot. Owned here (the export
        // grants a new reference); released in nativeDestroy.
        std::vector<AHardwareBuffer*> ringAhbs;

        // Held as Unique handles. Assigning `*createX()` into a raw handle member
        // destroys the Unique wrapper immediately, so the member holds a stale
        // handle and resetFences/waitForFences/vkFreeCommandBuffers on it traps
        // inside the driver (Adreno: SIGSEGV/SEGV_MAPERR at vkWaitForFences).
        vk::UniqueCommandPool blitPool{};
        vk::UniqueFence blitFence{};

        // When true, the pipeline is driven in "unsignaled" (self-driven) mode:
        // dispatch()/acquire() do NOT wait on the context's external timeline
        // semaphore, because nothing outside this file submits the signal for
        // that semaphore. The upstream lsfg-vk LAYER always signals it from its
        // own queue submit (wrapper.cpp: nextSyncValue = sync.second + 1), but
        // that layer owns the game's queue and we do not. The reference
        // self-contained drivers (lsfg-vk-cli benchmark.cpp / debug.cpp) drive
        // the context with the unsignaled flag for exactly this reason.
        bool selfDriven{false};

        // Last error raised by a frame-path JNI call (cleared on read).
        std::string lastError;

        // FD-shared frame exchange (the Android analogue of upstream lsfg-vk's
        // desktop path).
        //
        // Instead of copying each generated frame into an app-allocated
        // AHardwareBuffer (which faults inside Adreno's copy helper at fault
        // address 0x80 on this GPU, and forces a CPU lock of that AHB which
        // trips an fdsan double-close under Mesa's fallback gralloc), we hand the
        // PRESENTER an openable fd for the pipeline's OWN destination image.
        //
        // The pipeline already allocates its external output with an
        // OPAQUE_FD external handle type, so the memory is exportable. The
        // presenter imports that fd into its own device and copies it into the
        // swapchain, which is the one operation it already performs reliably.
        //
        // Ownership: dstFd and srcFd are duplicated for the caller, which must
        // close() them. The underlying Vulkan memory stays owned by the pipeline
        // and outlives them.
        int dstFd{-1};
        uint32_t dstWidth{0};
        uint32_t dstHeight{0};
        // Frame-exchange RING: N destination images allocated by VULKAN with
        // AHardwareBuffer export enabled, exported once each.
        //
        // This is the fix for the Adreno fault at 0x80. The pipeline generates
        // several frames per source frame and each one overwrites the single
        // external output image, so each generated frame must be copied out
        // immediately into a distinct destination. Previously those destinations
        // were app-allocated HardwareBuffers, and having the GPU write into an
        // app-allocated AHB faults inside Adreno's copy helper. Here the
        // destinations are the driver's OWN images, so the copy is an ordinary
        // intra-device image copy between two images we created, and the
        // presenter still receives an AHardwareBuffer (the only cross-VkDevice
        // share that works on this driver).
        //
        // The images and their memory are stored in dstImages/dstMemories so the
        // existing lifetime and destruction order covers them; ringAhbs holds the
        // exported references (owned here, released in nativeDestroy).
        uint32_t ringNext{0};

        std::mutex lock;
    };

    JniState* fromJni(jlong h) { return reinterpret_cast<JniState*>(h); }

    // Every GPU wait in this file goes through here. The Vulkan default is
    // UINT64_MAX (wait forever), which turned a never-signalled timeline
    // semaphore into an unrecoverable hang on Adreno: the capture thread parked
    // in sync_wait -> gsl_syncobj_wait -> vkWaitForFences with no way out, so
    // the session reported RUNNING at 0 fps forever instead of surfacing an
    // error. A finite timeout converts that silent freeze into a logged,
    // recoverable failure.
    constexpr uint64_t kFenceTimeoutNs{2'000'000'000ULL}; // 2s

    // Human-readable name for a vk::Result, WITHOUT relying on Vulkan-Hpp's
    // to_string (this build is compiled with VULKAN_HPP_NO_TO_STRING, so the
    // library's own error text prints a bare number - which is exactly how a
    // later investigation was misled into reading -3 as VK_TIMEOUT when the
    // real code is VK_ERROR_INITIALIZATION_FAILED).
    const char* resultName(vk::Result r) {
        switch (r) {
            case vk::Result::eSuccess:                  return "VK_SUCCESS";
            case vk::Result::eNotReady:                 return "VK_NOT_READY";
            case vk::Result::eTimeout:                  return "VK_TIMEOUT";
            case vk::Result::eErrorOutOfHostMemory:     return "VK_ERROR_OUT_OF_HOST_MEMORY";
            case vk::Result::eErrorOutOfDeviceMemory:   return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
            case vk::Result::eErrorInitializationFailed:return "VK_ERROR_INITIALIZATION_FAILED";
            case vk::Result::eErrorDeviceLost:          return "VK_ERROR_DEVICE_LOST";
            case vk::Result::eErrorSurfaceLostKHR:      return "VK_ERROR_SURFACE_LOST_KHR";
            case vk::Result::eErrorOutOfDateKHR:        return "VK_ERROR_OUT_OF_DATE_KHR";
            default:                                    return "(other)";
        }
    }

    // Raw, non-throwing fence wait.
    //
    // CRITICAL: this must NOT use Vulkan-Hpp's checked Device::waitForFences.
    // That wrapper runs resultCheck(..., {eSuccess, eTimeout}) and THROWS a
    // vk::SystemError for any other code BEFORE the result is returned to us.
    //
    // That is precisely why the earlier "bounded and tolerated" version logged
    // nothing at all: -3 (VK_ERROR_INITIALIZATION_FAILED) is not in that allowed
    // set, so the throw happened inside the header and escaped straight to the
    // JNI catch block as "nativeDispatch failed: vk::Device::waitForFences: -3".
    // Our tolerance branch was unreachable code. Calling the dispatch-loader
    // entry point directly yields the raw VkResult and lets the caller decide.
    vk::Result waitFenceRaw(JniState& st, vk::Fence fence, uint64_t timeoutNs) {
        const VkFence handle{static_cast<VkFence>(fence)};
        const VkDevice dev{static_cast<VkDevice>(st.device)};
        return static_cast<vk::Result>(
            st.dld->vkWaitForFences(dev, 1, &handle, VK_TRUE, timeoutNs));
    }

    void waitFenceBounded(JniState& st, vk::Fence fence, const char* what) {
        const auto result{waitFenceRaw(st, fence, kFenceTimeoutNs)};
        if (result == vk::Result::eSuccess)
            return;
        throw std::runtime_error(
            std::string(what) + ": GPU fence wait returned " +
            std::to_string(static_cast<int>(result)) + " (" + resultName(result) +
            ") after " + std::to_string(kFenceTimeoutNs / 1'000'000) + "ms bound"
        );
    }

    // Diagnostic probe: create a fresh fence on THIS device, submit a real
    // (empty) command buffer to the context's compute queue, and wait on it.
    //
    // Rationale: on this Adreno driver a wait on a freshly created fence has
    // been observed to fail immediately with -3 (VK_ERROR_INITIALIZATION_FAILED).
    // If this probe fails, the DEVICE/queue is already wedged at that point and
    // no amount of frame-accounting work in the Kotlin layer can matter. If it
    // succeeds, the device is healthy and the fault is in our own fence
    // ownership. That distinction is the whole point of the probe.
    //
    // Never throws: the return code IS the measurement.
    void probeFence(JniState& st, const char* label) {
        auto& dld{*st.dld};
        vk::Result code{vk::Result::eErrorOutOfHostMemory};
        std::string extra;
        try {
            auto pool{vkhelper::createCommandPool(dld, st.device, st.qfi, true)};
            auto cmdbuf{vkhelper::createCommandBuffer(dld, st.device, *pool)};
            cmdbuf->begin({ .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit }, dld);
            cmdbuf->end(dld);

            auto fence{vkhelper::createFence(dld, st.device)};
            const vk::SubmitInfo si{
                .commandBufferCount = 1,
                .pCommandBuffers = &*cmdbuf
            };
            const vk::SubmitInfo submits[1]{si};
            st.queue.submit(submits, *fence, dld);
            code = waitFenceRaw(st, *fence, kFenceTimeoutNs);
        } catch (const std::exception& e) {
            extra = std::string(" threw=") + e.what();
            code = vk::Result::eErrorOutOfHostMemory;
        }
        FLOG("PROBE %s = %d (%s)%s", label, static_cast<int>(code),
             resultName(code), extra.c_str());
    }

    // Diagnostic probe for the TIMELINE SEMAPHORE path specifically.
    //
    // PROBE A/C proved that a plain submit + fence wait works on this device.
    // The frame path differs in exactly one way: its submits carry a timeline
    // semaphore wait/signal (dispatch and acquire both do). If a submit that
    // WAITS on a timeline semaphore never completes, the fence attached to it
    // never signals, and the next dispatch's wait sends the driver into the
    // -3 path. This probe isolates that mechanism with a self-contained
    // signal-then-wait pair, so the result cannot be confused with the
    // pipeline's own state.
    //
    // PROBE T = 0   -> timeline semaphores work; the fault is in the pipeline's
    //                  own submit ordering/values.
    // PROBE T != 0  -> submitting with a timeline wait is what breaks, and the
    //                  fix is in how the frame path synchronizes.
    void probeTimeline(JniState& st, const char* label) {
        auto& dld{*st.dld};
        vk::Result code{vk::Result::eErrorOutOfHostMemory};
        std::string extra;
        try {
            auto sem{vkhelper::createSemaphore(dld, st.device, true /*timeline*/, false)};
            auto pool{vkhelper::createCommandPool(dld, st.device, st.qfi, true)};

            auto cb1{vkhelper::createCommandBuffer(dld, st.device, *pool)};
            cb1->begin({ .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit }, dld);
            cb1->end(dld);

            auto cb2{vkhelper::createCommandBuffer(dld, st.device, *pool)};
            cb2->begin({ .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit }, dld);
            cb2->end(dld);

            const vk::PipelineStageFlags stage{vk::PipelineStageFlagBits::eTopOfPipe};

            // Submit 1: signal the timeline semaphore at value 1.
            uint64_t signalValue{1};
            const vk::TimelineSemaphoreSubmitInfo sigInfo{
                .waitSemaphoreValueCount = 0,
                .pWaitSemaphoreValues = nullptr,
                .signalSemaphoreValueCount = 1,
                .pSignalSemaphoreValues = &signalValue
            };
            vk::Semaphore signalSems[1]{*sem};
            const vk::SubmitInfo submit1[1]{
                vk::SubmitInfo{
                    .pNext = &sigInfo,
                    .waitSemaphoreCount = 0,
                    .pWaitSemaphores = nullptr,
                    .pWaitDstStageMask = nullptr,
                    .commandBufferCount = 1,
                    .pCommandBuffers = &*cb1,
                    .signalSemaphoreCount = 1,
                    .pSignalSemaphores = signalSems
                }
            };
            st.queue.submit(submit1, nullptr, dld);

            // Submit 2: WAIT on that value, and attach a fence so we can tell
            // whether the queue ever gets past the wait.
            uint64_t waitValue{1};
            const vk::TimelineSemaphoreSubmitInfo waitInfo{
                .waitSemaphoreValueCount = 1,
                .pWaitSemaphoreValues = &waitValue,
                .signalSemaphoreValueCount = 0,
                .pSignalSemaphoreValues = nullptr
            };
            vk::Semaphore waitSems[1]{*sem};
            auto fence{vkhelper::createFence(dld, st.device)};
            const vk::SubmitInfo submit2[1]{
                vk::SubmitInfo{
                    .pNext = &waitInfo,
                    .waitSemaphoreCount = 1,
                    .pWaitSemaphores = waitSems,
                    .pWaitDstStageMask = &stage,
                    .commandBufferCount = 1,
                    .pCommandBuffers = &*cb2,
                    .signalSemaphoreCount = 0,
                    .pSignalSemaphores = nullptr
                }
            };
            st.queue.submit(submit2, *fence, dld);

            code = waitFenceRaw(st, *fence, kFenceTimeoutNs);
        } catch (const std::exception& e) {
            extra = std::string(" threw=") + e.what();
            code = vk::Result::eErrorOutOfHostMemory;
        }
        FLOG("PROBE %s = %d (%s)%s", label, static_cast<int>(code),
             resultName(code), extra.c_str());
    }

    // Forward declarations for the AHB import / blit helpers (defined below).
    vk::Image ensureDstImage(JniState& st, const void* ahb);
    vk::CommandBuffer ensureDstBlit(JniState& st, const void* dstAhb);

    void submitAndWait(JniState& st, vk::CommandBuffer cmdbuf) {
        auto& dld{*st.dld};
        st.queue.submit({{
            .commandBufferCount = 1,
            .pCommandBuffers = &cmdbuf
        }}, *st.blitFence, dld);
        waitFenceBounded(st, *st.blitFence, "framegen blit");
        st.device.resetFences(*st.blitFence, dld);
    }

    // Ensure a pre-recorded command buffer exists that blits the imported source AHB
    // into the requested layer of the pipeline's external input image (cached by
    // (src AHB ptr, layer)).
    // Import a source AHB, blit it into the requested pipeline input layer, wait for
    // completion, then release the one-shot import. Source AHBs are one-shot (captured,
    // blitted, then closed by the producer), so no import cache is kept (avoids leaks).
    void writeSourceLayer(JniState& st, const void* srcAhb, uint32_t layer) {
        auto& dld{*st.dld};

        // PROBE B: first frame entry. Compare against PROBE A. If A succeeded and
        // this one fails, something between session creation and the first frame
        // (the ring construction) wedged the queue.
        probeFence(st, "B_writeSource_entry");

        // Two-phase import so the bound memory is owned here and can be freed
        // together with the image after the copy completes (see
        // createImageFromAhbAlloc).
        auto [srcImage, srcMemory] = vkhelper::createImageFromAhbAlloc(dld, st.device, srcAhb);

        // Transition the imported source to a general layout (ready to blit from).
        {
            auto cmdbuf = vkhelper::createCommandBuffer(dld, st.device, *st.blitPool);
            auto fence = vkhelper::createFence(dld, st.device);
            vk::ImageMemoryBarrier ib{
                .sType = vk::StructureType::eImageMemoryBarrier,
                .pNext = nullptr,
                .srcAccessMask = vk::AccessFlags(),
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = *srcImage,
                .subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 }
            };
            cmdbuf->begin({ .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit }, dld);
            cmdbuf->pipelineBarrier(
                vk::PipelineStageFlagBits::eTopOfPipe,
                vk::PipelineStageFlagBits::eTransfer,
                vk::DependencyFlags(),
                0, nullptr,
                0, nullptr,
                1, &ib,
                dld
            );
            cmdbuf->end(dld);
            st.queue.submit({{
                .commandBufferCount = 1,
                .pCommandBuffers = &*cmdbuf
            }}, *fence, dld);
            waitFenceBounded(st, *fence, "src AHB transition");
        }

        const auto inImage = st.context->pipeline().getExternalInputs().front().image;
        const auto inExtent = st.context->pipeline().getExternalInputs().front().extent;

        // Blit the imported source into the pipeline input layer, synchronously.
        {
            auto cmdbuf = vkhelper::createCommandBuffer(dld, st.device, *st.blitPool);
            cmdbuf->begin({ .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit }, dld);

            vk::ImageMemoryBarrier inBarrier{
                .sType = vk::StructureType::eImageMemoryBarrier,
                .pNext = nullptr,
                .srcAccessMask = vk::AccessFlags(),
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = inImage,
                .subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 2 }
            };
            cmdbuf->pipelineBarrier(
                vk::PipelineStageFlagBits::eTopOfPipe,
                vk::PipelineStageFlagBits::eTransfer,
                vk::DependencyFlags(),
                0, nullptr,
                0, nullptr,
                1, &inBarrier,
                dld
            );

            vkhelper::blitImage(
                dld, *cmdbuf,
                *srcImage, 0,
                inImage, layer,
                inExtent
            );
            cmdbuf->end(dld);

            st.queue.submit({{
                .commandBufferCount = 1,
                .pCommandBuffers = &*cmdbuf
            }}, *st.blitFence, dld);
            waitFenceBounded(st, *st.blitFence, "src blit");
            st.device.resetFences(*st.blitFence, dld);

            // PROBE C: our own blit fence signalled correctly, so the submit ->
            // fence -> reset cycle in this file is sound on this device.
            probeFence(st, "C_after_src_blit");
        }

        // Release the one-shot import now that the copy has completed. Destroy the
        // image first, then free the memory it was bound to (Vulkan requires that
        // order). reset() destroys the handle AND releases ownership, so the
        // Unique wrappers do not double-destroy here.
        srcImage.reset();
        srcMemory.reset();
    }

    // Ensure a destination AHardwareBuffer is imported (cached by pointer) and
    // transitioned to a general layout ready to be blitted into.
    vk::Image ensureDstImage(JniState& st, const void* ahb) {
        auto& dld{*st.dld};
        const uintptr_t key{reinterpret_cast<uintptr_t>(ahb)};
        const auto it{st.dstImages.find(key)};
        if (it != st.dstImages.end())
            return it->second;

        // Two-phase import: take ownership of BOTH the image and its memory, and
        // stash the memory in dstMemories so it outlives every copy that targets
        // this image. Releasing only the image here (the previous behaviour) is
        // what made the destination a dangling reference.
        auto [image, memory] = vkhelper::createImageFromAhbAlloc(dld, st.device, ahb);

        // Transition to general (ready to be the blit destination).
        {
            auto cmdbuf = vkhelper::createCommandBuffer(dld, st.device, *st.blitPool);
            auto fence = vkhelper::createFence(dld, st.device);
            vk::ImageMemoryBarrier ib{
                .sType = vk::StructureType::eImageMemoryBarrier,
                .pNext = nullptr,
                .srcAccessMask = vk::AccessFlags(),
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = *image,
                .subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 }
            };
            cmdbuf->begin({ .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit }, dld);
            cmdbuf->pipelineBarrier(
                vk::PipelineStageFlagBits::eTopOfPipe,
                vk::PipelineStageFlagBits::eTransfer,
                vk::DependencyFlags(),
                0, nullptr,
                0, nullptr,
                1, &ib,
                dld
            );
            cmdbuf->end(dld);
            st.queue.submit({{
                .commandBufferCount = 1,
                .pCommandBuffers = &*cmdbuf
            }}, *fence, dld);
            waitFenceBounded(st, *fence, "dst AHB transition");
        }

        st.dstImages.emplace(key, *image);
        st.dstMemories.emplace(key, *memory);
        return *image;
    }

    // Ensure a pre-recorded command buffer exists that blits the pipeline's external
    // output image into the given imported destination AHB (cached by AHB pointer).
    vk::CommandBuffer ensureDstBlit(JniState& st, const void* dstAhb) {
        auto& dld{*st.dld};
        const uintptr_t key{reinterpret_cast<uintptr_t>(dstAhb)};
        const auto it{st.dstBlits.find(key)};
        if (it != st.dstBlits.end())
            return *it->second;

        auto dstImage = ensureDstImage(st, dstAhb);
        auto outImage = st.context->pipeline().getExternalOutputs().front().image;
        auto outExtent = st.context->pipeline().getExternalOutputs().front().extent;

        auto cmdbuf = vkhelper::createCommandBuffer(dld, st.device, *st.blitPool);
        cmdbuf->begin({ .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit }, dld);

        // The pipeline's output image is written by compute; make it transfer-readable.
        vk::ImageMemoryBarrier outBarrier{
            .sType = vk::StructureType::eImageMemoryBarrier,
            .pNext = nullptr,
            .srcAccessMask = vk::AccessFlags(),
            .dstAccessMask = vk::AccessFlagBits::eTransferRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = outImage,
            .subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 }
        };
        cmdbuf->pipelineBarrier(
            vk::PipelineStageFlagBits::eTopOfPipe,
            vk::PipelineStageFlagBits::eTransfer,
            vk::DependencyFlags(),
            0, nullptr,
            0, nullptr,
            1, &outBarrier,
            dld
        );

        vkhelper::blitImage(
            dld, *cmdbuf,
            outImage, 0,
            dstImage, 0,
            outExtent
        );
        cmdbuf->end(dld);

        st.dstBlits.emplace(key, std::move(cmdbuf));
        return *st.dstBlits.at(key);
    }

    // Ensure a pre-recorded command buffer exists that copies the pipeline's
    // external output image into the given RING destination image.
    //
    // Both images are created and owned by this device, so this is an ordinary
    // intra-device image copy. That is the whole point: the previous design
    // imported an app-allocated AHardwareBuffer as the copy destination, and
    // having the GPU write into an app-allocated AHB faults inside Adreno's copy
    // helper (SIGSEGV at 0x80) through both vkCmdBlitImage and vkCmdCopyImage.
    // Here the destination is the driver's own exportable image, and the
    // presenter still receives an AHardwareBuffer.
    vk::CommandBuffer ensureRingCopy(JniState& st, size_t index) {
        auto& dld{*st.dld};
        if (index < st.ringBlits.size() && st.ringBlits[index])
            return *st.ringBlits[index];

        const auto& out{st.context->pipeline().getExternalOutputs().front()};
        vk::Image dstImage{st.ringImages.at(index)};
        const vk::Extent2D extent{
            std::min(out.extent.width, st.dstWidth),
            std::min(out.extent.height, st.dstHeight)
        };

        auto cmdbuf = vkhelper::createCommandBuffer(dld, st.device, *st.blitPool);
        cmdbuf->begin({ .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit }, dld);

        // The output image is written by compute in GENERAL; make that write
        // visible to the transfer read below. Kept in GENERAL so the compute
        // pass can keep writing it every iteration.
        vk::ImageMemoryBarrier outBarrier{
            .sType = vk::StructureType::eImageMemoryBarrier,
            .pNext = nullptr,
            .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = out.image,
            .subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 }
        };
        // The ring destination stayed in GENERAL since creation; only the
        // transfer write needs to be made visible to the presenter's read.
        vk::ImageMemoryBarrier dstBarrier{
            .sType = vk::StructureType::eImageMemoryBarrier,
            .pNext = nullptr,
            .srcAccessMask = vk::AccessFlags(),
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = dstImage,
            .subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 }
        };
        const std::array<vk::ImageMemoryBarrier, 2> barriers{outBarrier, dstBarrier};
        cmdbuf->pipelineBarrier(
            vk::PipelineStageFlagBits::eComputeShader,
            vk::PipelineStageFlagBits::eTransfer,
            vk::DependencyFlags(),
            0, nullptr,
            0, nullptr,
            static_cast<uint32_t>(barriers.size()), barriers.data(),
            dld
        );

        const vk::ImageCopy copyInfo{
            .srcSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
            .srcOffset = { 0, 0, 0 },
            .dstSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
            .dstOffset = { 0, 0, 0 },
            .extent = { extent.width, extent.height, 1 }
        };
        cmdbuf->copyImage(
            out.image, vk::ImageLayout::eGeneral,
            dstImage, vk::ImageLayout::eGeneral,
            copyInfo,
            dld
        );
        cmdbuf->end(dld);

        if (st.ringBlits.size() <= index)
            st.ringBlits.resize(index + 1);
        st.ringBlits[index] = std::move(cmdbuf);
        return *st.ringBlits[index];
    }

    // Ensure the ring destination image is importable by the presenter's device
    // by transitioning it once, then return the pre-recorded copy that moves the
    // pipeline output into it.
    void driveRingSlot(JniState& st, size_t index) {
        if (index >= st.ringImages.size())
            return;
        auto cmdbuf = ensureRingCopy(st, index);
        submitAndWait(st, cmdbuf);
    }

} // namespace

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_lsfg_fork_framegen_FramegenBridge_nativeInit(JNIEnv* env, jclass, jstring dllPath, jboolean allowFP16) {
    try {
        if (!dllPath) return 0;
        const char* dll = env->GetStringUTFChars(dllPath, nullptr);
        std::string dllStr = dll;
        env->ReleaseStringUTFChars(dllPath, dll);

        auto* st = new JniState();
        st->instance = std::make_unique<lsfgvk::Instance>(
            "", std::filesystem::path(dllStr), allowFP16 == JNI_TRUE, false,
            [](const std::string& msg) { FLOG("%s", msg.c_str()); }
        );

        auto& vk{st->instance->_impl().vk};
        st->dld = vk.dld.get();
        st->device = *vk.device;
        st->queue = vk.queue;
        st->qfi = vk.qfi;

        // Drive the pipeline ourselves: see JniState::selfDriven. The frame
        // exchange with the pipeline goes through AHardwareBuffer imports and
        // the blit fence (submitAndWait), not through the context's external
        // timeline semaphore, so that semaphore must never be waited on.
        st->selfDriven = true;

        FLOG("framegen instance created, dll=%s (self-driven, no external sync signal)", dllStr.c_str());
        return reinterpret_cast<jlong>(st);
    } catch (const std::exception& e) {
        FLOGE("nativeInit failed: %s", e.what());
        return 0;
    }
}

JNIEXPORT void JNICALL
Java_com_lsfg_fork_framegen_FramegenBridge_nativeSetCacheDir(JNIEnv* env, jclass, jstring dir) {
    if (!dir) return;
    const char* d = env->GetStringUTFChars(dir, nullptr);
    try { vkhelper::setPipelineCacheDir(d); }
    catch (const std::exception& e) { FLOGE("setCacheDir: %s", e.what()); }
    env->ReleaseStringUTFChars(dir, d);
}

JNIEXPORT jboolean JNICALL
Java_com_lsfg_fork_framegen_FramegenBridge_nativeCreateSession(
    JNIEnv* env, jclass, jlong handle,
    jint width, jint height, jfloat flowScale, jboolean performance) {
    auto* st = fromJni(handle);
    if (!st || !st->instance) return JNI_FALSE;
    std::lock_guard<std::mutex> lock(st->lock);
    try {
        st->context = std::make_unique<lsfgvk::Context>(
            *st->instance, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
            static_cast<float>(flowScale), performance == JNI_TRUE,
            lsfgvk::BitDepth::R8G8B8A8, lsfgvk::ColorSpace::SRGB,
            lsfgvk::TransferFunction::SRGB, nullptr, nullptr
        );

        auto& dld{*st->dld};
        st->blitPool = vkhelper::createCommandPool(dld, st->device, st->qfi);
        st->blitFence = vkhelper::createFence(dld, st->device);

        // PROBE A: baseline. Does a plain submit+wait on this device work at all
        // before the frame path runs? If this fails, nothing downstream can work.
        probeFence(*st, "A_after_session");
        // PROBE T: same thing, but with a timeline semaphore wait in the chain.
        // This is the only structural difference between a working submit and
        // the failing dispatch/acquire submits.
        probeTimeline(*st, "T_timeline_semaphore");

        // Export the pipeline's external images as openable fds and put them in
        // the layout the presenter expects. This replaces the AHB round-trip that
        // crashed on Adreno (see JniState::dstFd).
        {
            const auto& out{st->context->pipeline().getExternalOutputs().front()};
            const auto& in{st->context->pipeline().getExternalInputs().front()};
            st->dstWidth = out.extent.width;
            st->dstHeight = out.extent.height;

            // Keep the output in GENERAL forever: the compute pipeline writes it
            // in GENERAL every iteration and the presenter reads it in GENERAL,
            // so no per-frame layout juggling is needed on either side. The
            // pipeline already transitions all images into GENERAL at build time,
            // so exporting the memory here is enough.
            // Frame-exchange destination fd, used only as a FALLBACK when the
            // output could not be made AHB-exportable.
            //
            // Only valid for images created with the OPAQUE_FD external handle
            // type; for an AHB-exportable output vkGetMemoryFdKHR fails inside the
            // driver (result -1), which used to abort the whole session and throw
            // away a fully built pipeline. That failure is now tolerated.
            //
            // The SOURCE fd is deliberately NOT exported any more: it was exported
            // on every session and never read by anything, so it was a pure leaked
            // descriptor. Leaked descriptors are directly implicated in the fdsan
            // abort under investigation, where the driver closes a descriptor it
            // believes it owns but whose number has been reused by the app's log
            // writer. Dropping it removes that risk at no functional cost.
            try {
                if (!out.ahbExportable)
                    st->dstFd = vkhelper::exportMemoryFd(dld, st->device, out.memory);
                // Never hold a descriptor numbered 0/1/2: those are the process's
                // std streams. If a driver hands one back, rename it out of the way
                // so our later close() cannot collide with the app's log writer
                // (the fdsan abort class under investigation).
                st->dstFd = vkhelper::safeExportedFd(st->dstFd);
                FLOG("frame-exchange dst fd=%d (%ux%u)",
                    st->dstFd, st->dstWidth, st->dstHeight);
            } catch (const std::exception& e) {
                FLOG("fd export unavailable (expected when AHB-exportable): %s", e.what());
            }

            // Frame-exchange ring: N exporter-capable destination images owned by
            // this device, one per generated frame, each exported as an
            // AHardwareBuffer for the presenter.
            //
            // The pipeline writes every generated frame into the SAME single-layer
            // output image, so each generated frame must be copied out before the
            // next acquire() overwrites it. Previously that copy targeted
            // app-allocated HardwareBuffers, and the GPU writing into an
            // app-allocated AHB faults inside Adreno's copy helper at 0x80 through
            // both vkCmdBlitImage and vkCmdCopyImage. Making the destinations
            // driver-owned images turns the failing operation into an ordinary
            // intra-device copy while still presenting an AHardwareBuffer.
            const vk::ImageUsageFlags ringUsage{
                vk::ImageUsageFlagBits::eTransferDst |
                vk::ImageUsageFlagBits::eTransferSrc |
                vk::ImageUsageFlagBits::eSampled
            };
            const bool ringExportable{vkhelper::supportsAhbExport(
                dld, st->instance->_impl().vk.physdev,
                { st->dstWidth, st->dstHeight }, vk::Format::eR8G8B8A8Unorm, ringUsage
            )};
            if (ringExportable) {
                constexpr size_t kRingSize = 8;
                const vk::ExternalMemoryImageCreateInfo ringExternal{
                    .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eAndroidHardwareBufferANDROID
                };
                const vk::ImageCreateInfo ringInfo{
                    .pNext = &ringExternal,
                    .imageType = vk::ImageType::e2D,
                    .format = vk::Format::eR8G8B8A8Unorm,
                    .extent = { st->dstWidth, st->dstHeight, 1 },
                    .mipLevels = 1,
                    .arrayLayers = 1,
                    .samples = vk::SampleCountFlagBits::e1,
                    .tiling = vk::ImageTiling::eOptimal,
                    .usage = ringUsage
                };
                const vk::ExportMemoryAllocateInfo ringExport{
                    .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eAndroidHardwareBufferANDROID
                };
                for (size_t i = 0; i < kRingSize; ++i) {
                    vk::Image image{st->device.createImage(ringInfo, nullptr, dld)};
                    const auto req{st->device.getImageMemoryRequirements(image, dld)};
                    const vk::MemoryDedicatedAllocateInfo dedicated{
                        .pNext = &ringExport,
                        .image = image
                    };
                    const vk::MemoryAllocateInfo mai{
                        .pNext = &dedicated,
                        .allocationSize = req.size,
                        .memoryTypeIndex = static_cast<uint32_t>(
                            std::countr_zero(req.memoryTypeBits))
                    };
                    vk::DeviceMemory memory{st->device.allocateMemory(mai, nullptr, dld)};
                    st->device.bindImageMemory(image, memory, 0, dld);
                    st->ringImages.push_back(image);
                    st->ringMemory.push_back(memory);

                    // Export before anything writes it, and acquire the presenter-
                    // facing reference we keep for the life of the session.
                    AHardwareBuffer* ahb{vkhelper::exportMemoryAhb(dld, st->device, memory)};
                    if (!ahb) {
                        FLOGE("ring AHB export failed at slot %zu", i);
                        break;
                    }
                    st->ringAhbs.push_back(ahb);
                }
                FLOG("frame-exchange ring ready: %zu slabs %ux%u (AHB-exportable)",
                    st->ringAhbs.size(), st->dstWidth, st->dstHeight);
            } else {
                FLOG("destination image is not AHB-exportable; presenter will use the fd path");
            }

            // PROBE B0: immediately after the frame-exchange ring was built. This
            // isolates whether ring construction itself wedges the queue.
            probeFence(*st, "B0_after_ring");
        }
        // Source/destination AHBs (and their blit command buffers) are imported
        // lazily on the first writeSource/acquireToDst for each buffer.

        FLOG("session created %dx%d flow=%.2f perf=%d",
            width, height, static_cast<double>(flowScale), performance ? 1 : 0);
        return JNI_TRUE;
    } catch (const std::exception& e) {
        FLOGE("nativeCreateSession failed: %s", e.what());
        st->context = nullptr;
        return JNI_FALSE;
    }
}

JNIEXPORT void JNICALL
Java_com_lsfg_fork_framegen_FramegenBridge_nativeWriteSource(
    JNIEnv* env, jclass, jlong handle, jlong srcAhbPtr, jint slot) {
    auto* st = fromJni(handle);
    if (!st || !st->context) return;
    std::lock_guard<std::mutex> lock(st->lock);
    try {
        const void* src = reinterpret_cast<const void*>(srcAhbPtr);
        const uint32_t layer = static_cast<uint32_t>(slot) % 2;
        writeSourceLayer(*st, src, layer);
    } catch (const std::exception& e) {
        FLOGE("nativeWriteSource failed: %s", e.what());
        st->lastError = e.what();
    }
}

JNIEXPORT void JNICALL
Java_com_lsfg_fork_framegen_FramegenBridge_nativeDispatch(
    JNIEnv* env, jclass, jlong handle, jint total) {
    auto* st = fromJni(handle);
    if (!st || !st->context) return;
    // Serialize against writeSource/acquireToDst. Those two hold st->lock but
    // this one did not, even though all three touch the same device, queue,
    // fence and semaphore counters. An unlocked dispatch racing a concurrent
    // acquire is exactly the kind of shared-fence misuse that yields a bogus
    // result code on the next wait.
    std::lock_guard<std::mutex> lock(st->lock);
    try {
        // unsignaled = true: we own the submit and never signal the context's
        // external timeline semaphore, so dispatch must not wait on it (see
        // JniState::selfDriven). Signalling it from outside would also break
        // the documented protocol: dispatch() increments the value it waits on
        // internally, so an external signal at the wrong value either hangs or
        // releases the pipeline against a frame that was never queued.
        st->context->dispatch(static_cast<uint32_t>(total), st->selfDriven);
    } catch (const std::exception& e) {
        FLOGE("nativeDispatch failed: %s", e.what());
        st->lastError = e.what();
    }
}

JNIEXPORT void JNICALL
Java_com_lsfg_fork_framegen_FramegenBridge_nativeAcquire(
    JNIEnv* env, jclass, jlong handle) {
    auto* st = fromJni(handle);
    if (!st || !st->context) return;
    std::lock_guard<std::mutex> lock(st->lock);
    try {
        st->context->acquire(st->selfDriven);
    } catch (const std::exception& e) {
        FLOGE("nativeAcquire failed: %s", e.what());
    }
}

/// Acquire the next generated frame and blit it into the shared destination
/// AHardwareBuffer that the WSI presenter reads from.
JNIEXPORT void JNICALL
Java_com_lsfg_fork_framegen_FramegenBridge_nativeAcquireToDst(
    JNIEnv* env, jclass, jlong handle, jlong dstAhbPtr) {
    auto* st = fromJni(handle);
    if (!st || !st->context) return;
    std::lock_guard<std::mutex> lock(st->lock);
    try {
        // Generate the next frame (into the pipeline's external output image).
        // unsignaled = true for the same reason as dispatch() above.
        st->context->acquire(st->selfDriven);

        // Copy the generated frame into the frame-exchange ring slot.
        //
        // The ring destination is an image THIS device created, so this is an
        // ordinary intra-device copy. The old path copied into an app-allocated
        // AHardwareBuffer, which faults inside Adreno's copy helper at fault
        // address 0x80 (SIGSEGV/SEGV_MAPERR) through BOTH vkCmdBlitImage and
        // vkCmdCopyImage. The presenter still receives an AHardwareBuffer because
        // every ring slot was created exportable.
        //
        // Only fall back to the app-AHB destination when no ring exists (a driver
        // that cannot export AHBs at all).
        if (!st->ringImages.empty()) {
            const size_t slot{
                static_cast<size_t>(st->ringNext % st->ringImages.size())
            };
            driveRingSlot(*st, slot);
            st->ringNext = (st->ringNext + 1) %
                static_cast<uint32_t>(st->ringImages.size());
        } else {
            const void* dst = reinterpret_cast<const void*>(dstAhbPtr);
            auto blit = ensureDstBlit(*st, dst);
            submitAndWait(*st, blit);
        }
    } catch (const std::exception& e) {
        FLOGE("nativeAcquireToDst failed: %s", e.what());
        st->lastError = e.what();
    }
}

/// Hand the presenter an openable fd for the pipeline's destination image, so
/// frames can be fetched without copying into an app-allocated AHardwareBuffer.
///
/// Returns an int[3] = {fd, width, height}, or a zero-length array on failure.
/// The caller owns the fd and must close() it.
JNIEXPORT jintArray JNICALL
Java_com_lsfg_fork_framegen_FramegenBridge_nativeDestinationFd(
    JNIEnv* env, jclass, jlong handle) {
    auto* st = fromJni(handle);
    if (!st || !st->context || st->dstFd < 0)
        return env->NewIntArray(0);
    std::lock_guard<std::mutex> lock(st->lock);
    // dup() so the pipeline keeps its own fd and the caller has a lifetime it
    // fully controls (it may close it whenever it finishes importing).
    const int fd{::dup(st->dstFd)};
    if (fd < 0)
        return env->NewIntArray(0);
    const jint values[3]{
        static_cast<jint>(fd),
        static_cast<jint>(st->dstWidth),
        static_cast<jint>(st->dstHeight)
    };
    jintArray out{env->NewIntArray(3)};
    env->SetIntArrayRegion(out, 0, 3, values);
    return out;
}

JNIEXPORT jint JNICALL
Java_com_lsfg_fork_framegen_FramegenBridge_nativeRingCount(
    JNIEnv*, jclass, jlong handle) {
    auto* st = fromJni(handle);
    if (!st) return 0;
    return static_cast<jint>(st->ringAhbs.size());
}

JNIEXPORT jlong JNICALL
Java_com_lsfg_fork_framegen_FramegenBridge_nativeRingAhb(
    JNIEnv*, jclass, jlong handle, jint index) {
    auto* st = fromJni(handle);
    if (!st) return 0;
    std::lock_guard<std::mutex> lock(st->lock);
    if (index < 0 || static_cast<size_t>(index) >= st->ringAhbs.size()) return 0;
    // Raw pointer WITHOUT transferring ownership: the state owns the export
    // reference. The presenter's queue push takes its own reference.
    return reinterpret_cast<jlong>(st->ringAhbs[static_cast<size_t>(index)]);
}

JNIEXPORT void JNICALL
Java_com_lsfg_fork_framegen_FramegenBridge_nativeIdle(
    JNIEnv* env, jclass, jlong handle) {
    auto* st = fromJni(handle);
    if (!st || !st->context) return;
    try {
        st->context->idle();
    } catch (const std::exception& e) {
        FLOGE("nativeIdle failed: %s", e.what());
        st->lastError = e.what();
    }
}

/// Retrieve and clear the last framegen error (empty string = healthy).
/// Lets the Kotlin layer distinguish "produced 0 frames because the GPU
/// stalled" from "produced 0 frames because nothing was fed yet".
JNIEXPORT jstring JNICALL
Java_com_lsfg_fork_framegen_FramegenBridge_nativeLastError(
    JNIEnv* env, jclass, jlong handle) {
    auto* st = fromJni(handle);
    if (!st) return env->NewStringUTF("");
    std::lock_guard<std::mutex> lock(st->lock);
    const std::string msg{st->lastError};
    st->lastError.clear();
    return env->NewStringUTF(msg.c_str());
}

JNIEXPORT void JNICALL
Java_com_lsfg_fork_framegen_FramegenBridge_nativeDestroy(
    JNIEnv* env, jclass, jlong handle) {
    auto* st = fromJni(handle);
    if (!st) return;
    st->context.reset();
    auto& dld{*st->dld};
    // Release any imported destination AHB images before the device goes away.
    //
    // The ring slots are destroyed FIRST: they are OUR images and memory, and the
    // exported AHB references taken from them must be dropped before the memory
    // that backs them (and before the device itself). Command buffers that copy
    // into those images must also go before the images.
    st->ringBlits.clear();
    for (auto& image : st->ringImages)
        if (image) st->device.destroyImage(image, nullptr, dld);
    st->ringImages.clear();
    for (auto& memory : st->ringMemory)
        if (memory) st->device.freeMemory(memory, nullptr, dld);
    st->ringMemory.clear();
    for (auto& [key, image] : st->dstImages)
        if (image)
            st->device.destroyImage(image, nullptr, dld);
    st->dstImages.clear();
    // Free the backing memory AFTER the images that were bound to it (Vulkan
    // requires the image to be destroyed first).
    for (auto& [key, memory] : st->dstMemories)
        if (memory)
            st->device.freeMemory(memory, nullptr, dld);
    st->dstMemories.clear();
    // Command buffers must be freed before the pool they came from; the Unique
    // wrappers then release the fence and the pool exactly once.
    st->dstBlits.clear();
    st->blitFence.reset();
    st->blitPool.reset();
    // Close the exported fds. The pipeline memory itself is released by
    // st->instance.reset() below; these are our own descriptors, so closing them
    // here does not invalidate the memory.
    if (st->dstFd >= 0) { ::close(st->dstFd); st->dstFd = -1; }
    // Release the exported references we hold on the frame-exchange ring. The ring
    // images and memory are destroyed by the dstImages/dstMemories loops above;
    // this only drops the extra reference the export call granted us.
    for (auto* ahb : st->ringAhbs) {
        if (ahb) AHardwareBuffer_release(ahb);
    }
    st->ringAhbs.clear();
    st->instance.reset();
    delete st;
}

} // extern "C"

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK)
        lsfgjlog::init(env, vm);
    return JNI_VERSION_1_6;
}
