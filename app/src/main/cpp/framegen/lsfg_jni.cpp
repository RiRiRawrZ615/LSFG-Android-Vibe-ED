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

#include <atomic>
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
        std::unordered_map<uintptr_t, vk::Image> dstImages;
        // Pre-recorded blits: dst AHB ptr -> command buffer that copies the pipeline
        // output image into that imported destination image.
        std::unordered_map<uintptr_t, vk::CommandBuffer> dstBlits;

        vk::CommandPool blitPool{};
        vk::Fence blitFence{};

        std::mutex lock;
    };

    JniState* fromJni(jlong h) { return reinterpret_cast<JniState*>(h); }

    // Forward declarations for the AHB import / blit helpers (defined below).
    vk::Image ensureDstImage(JniState& st, const void* ahb);
    vk::CommandBuffer ensureDstBlit(JniState& st, const void* dstAhb);

    void submitAndWait(JniState& st, vk::CommandBuffer cmdbuf) {
        auto& dld{*st.dld};
        st.queue.submit({{
            .commandBufferCount = 1,
            .pCommandBuffers = &cmdbuf
        }}, st.blitFence, dld);
        if (st.device.waitForFences(st.blitFence, VK_TRUE, UINT64_MAX, dld) != vk::Result::eSuccess) {
            throw std::runtime_error("framegen blit wait failed");
        }
        st.device.resetFences(st.blitFence, dld);
    }

    // Ensure a pre-recorded command buffer exists that blits the imported source AHB
    // into the requested layer of the pipeline's external input image (cached by
    // (src AHB ptr, layer)).
    // Import a source AHB, blit it into the requested pipeline input layer, wait for
    // completion, then release the one-shot import. Source AHBs are one-shot (captured,
    // blitted, then closed by the producer), so no import cache is kept (avoids leaks).
    void writeSourceLayer(JniState& st, const void* srcAhb, uint32_t layer) {
        auto& dld{*st.dld};

        auto srcImage = vkhelper::createImageFromAhb(dld, st.device, srcAhb);

        // Transition the imported source to a general layout (ready to blit from).
        {
            auto cmdbuf = vkhelper::createCommandBuffer(dld, st.device, st.blitPool);
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
            if (st.device.waitForFences(*fence, VK_TRUE, UINT64_MAX, dld) != vk::Result::eSuccess)
                throw std::runtime_error("src AHB transition failed");
        }

        const auto inImage = st.context->pipeline().getExternalInputs().front().image;
        const auto inExtent = st.context->pipeline().getExternalInputs().front().extent;

        // Blit the imported source into the pipeline input layer, synchronously.
        {
            auto cmdbuf = vkhelper::createCommandBuffer(dld, st.device, st.blitPool);
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
            }}, st.blitFence, dld);
            if (st.device.waitForFences(st.blitFence, VK_TRUE, UINT64_MAX, dld) != vk::Result::eSuccess)
                throw std::runtime_error("src blit wait failed");
            st.device.resetFences(st.blitFence, dld);
        }

        // Release the one-shot import now that the blit has completed.
        st.device.destroyImage(*srcImage, nullptr, dld);
    }

    // Ensure a destination AHardwareBuffer is imported (cached by pointer) and
    // transitioned to a general layout ready to be blitted into.
    vk::Image ensureDstImage(JniState& st, const void* ahb) {
        auto& dld{*st.dld};
        const uintptr_t key{reinterpret_cast<uintptr_t>(ahb)};
        const auto it{st.dstImages.find(key)};
        if (it != st.dstImages.end())
            return it->second;

        auto image = vkhelper::createImageFromAhb(dld, st.device, ahb);

        // Transition to general (ready to be the blit destination).
        {
            auto cmdbuf = vkhelper::createCommandBuffer(dld, st.device, st.blitPool);
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
            if (st.device.waitForFences(*fence, VK_TRUE, UINT64_MAX, dld) != vk::Result::eSuccess)
                throw std::runtime_error("dst AHB transition failed");
        }

        st.dstImages.emplace(key, *image);
        return *image;
    }

    // Ensure a pre-recorded command buffer exists that blits the pipeline's external
    // output image into the given imported destination AHB (cached by AHB pointer).
    vk::CommandBuffer ensureDstBlit(JniState& st, const void* dstAhb) {
        auto& dld{*st.dld};
        const uintptr_t key{reinterpret_cast<uintptr_t>(dstAhb)};
        const auto it{st.dstBlits.find(key)};
        if (it != st.dstBlits.end())
            return it->second;

        auto dstImage = ensureDstImage(st, dstAhb);
        auto outImage = st.context->pipeline().getExternalOutputs().front().image;
        auto outExtent = st.context->pipeline().getExternalOutputs().front().extent;

        auto cmdbuf = vkhelper::createCommandBuffer(dld, st.device, st.blitPool);
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

        st.dstBlits.emplace(key, *cmdbuf);
        return *cmdbuf;
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

        FLOG("framegen instance created, dll=%s", dllStr.c_str());
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
        st->blitPool = *vkhelper::createCommandPool(dld, st->device, st->qfi);
        st->blitFence = *vkhelper::createFence(dld, st->device);
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
    }
}

JNIEXPORT void JNICALL
Java_com_lsfg_fork_framegen_FramegenBridge_nativeDispatch(
    JNIEnv* env, jclass, jlong handle, jint total) {
    auto* st = fromJni(handle);
    if (!st || !st->context) return;
    try {
        st->context->dispatch(static_cast<uint32_t>(total));
    } catch (const std::exception& e) {
        FLOGE("nativeDispatch failed: %s", e.what());
    }
}

JNIEXPORT void JNICALL
Java_com_lsfg_fork_framegen_FramegenBridge_nativeAcquire(
    JNIEnv* env, jclass, jlong handle) {
    auto* st = fromJni(handle);
    if (!st || !st->context) return;
    try {
        st->context->acquire();
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
        const void* dst = reinterpret_cast<const void*>(dstAhbPtr);

        // Generate the next frame (into the pipeline's external output image).
        st->context->acquire();

        // Blit the generated frame into the presenter's destination AHB (importing /
        // recording the blit on first use for this buffer), synchronously.
        auto blit = ensureDstBlit(*st, dst);
        submitAndWait(*st, blit);
    } catch (const std::exception& e) {
        FLOGE("nativeAcquireToDst failed: %s", e.what());
    }
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
    }
}

JNIEXPORT void JNICALL
Java_com_lsfg_fork_framegen_FramegenBridge_nativeDestroy(
    JNIEnv* env, jclass, jlong handle) {
    auto* st = fromJni(handle);
    if (!st) return;
    st->context.reset();
    auto& dld{*st->dld};
    // Release any imported destination AHB images before the device goes away.
    for (auto& [key, image] : st->dstImages)
        if (image)
            st->device.destroyImage(image, nullptr, dld);
    st->dstImages.clear();
    if (st->blitFence) st->device.destroyFence(st->blitFence, nullptr, dld);
    if (st->blitPool) st->device.destroyCommandPool(st->blitPool, nullptr, dld);
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
