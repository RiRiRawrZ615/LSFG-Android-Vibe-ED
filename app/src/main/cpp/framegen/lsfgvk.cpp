/* SPDX-License-Identifier: CC-BY-NC-ND-4.0 */

#include "lsfgvk.hpp"
#include "library.hpp"
#include "pipeline.hpp"
#include "pipelines/v3_1.hpp"

#include "vkhelper.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <vk>

using namespace lsfgvk;

namespace {
    /// Build the set of device extensions to enable, based strictly on what the
    /// physical device reports it supports. This is the universal path: we never
    /// hardcode an assumption that a given extension exists. We query
    /// vkEnumerateDeviceExtensionProperties and enable only the supported
    /// entries, then require the ones the pipeline actually depends on.
    ///
    /// On Linux the frame exchange is fd-based, so VK_KHR_external_memory_fd and
    /// VK_KHR_external_semaphore_fd are required (used by Context::exportFds).
    /// On Android the frame exchange is AHardwareBuffer-based, so
    /// VK_ANDROID_external_memory_android_hardware_buffer is required (used by
    /// vkhelper::createImageFromAhb -> vkGetAndroidHardwareBufferPropertiesANDROID).
    /// Enabling an extension the device lacks is what previously caused a NULL
    /// device-level function pointer to be called (the SIGSEGV in
    /// createImageFromAhb).
    std::vector<const char*> selectDeviceExtensions(
        const vk::detail::DispatchLoaderDynamic& dld,
        const vk::PhysicalDevice& physdev,
        std::function<void(const std::string&)> logCallback
    ) {
        const auto supported{physdev.enumerateDeviceExtensionProperties(nullptr, dld)};

        const auto hasSupport{[&](const char* name) {
            for (const auto& prop : supported)
                if (std::string(prop.extensionName) == name)
                    return true;
            return false;
        }};

        const auto logExt{[&](const char* name) {
            if (logCallback)
                logCallback(std::string("device ext ") + name + ": " +
                    (hasSupport(name) ? "supported" : "absent"));
        }};

        std::vector<const char*> enabled;

        // synchronization2 (vkCmdPipelineBarrier2) is core in Vulkan 1.3 but
        // only an extension (VK_KHR_synchronization2) below 1.3. The pipeline
        // calls pipelineBarrier2KHR unconditionally (pipeline.cpp), so we must
        // guarantee the function pointer is non-NULL. This device inherits its
        // API version from the instance (created at Vulkan 1.2), so on a 1.2
        // driver synchronization2 is NOT core and the extension is mandatory.
        // Enabling it on a 1.3 driver is a harmless no-op (it is promoted to
        // core), so we enable it whenever the physical device reports support
        // and only *require* it when the device is below 1.3.
        const auto props{physdev.getProperties(dld)};
        const bool is13Plus{props.apiVersion >= vk::makeVersion(1, 3, 0)};
        const bool sync2{hasSupport(vk::KHRSynchronization2ExtensionName)};
        if (sync2)
            enabled.push_back(vk::KHRSynchronization2ExtensionName);

        // fd-based external memory / semaphore (Linux frame exchange).
        const bool memFd{hasSupport(vk::KHRExternalMemoryFdExtensionName)};
        const bool semFd{hasSupport(vk::KHRExternalSemaphoreFdExtensionName)};
        if (memFd)
            enabled.push_back(vk::KHRExternalMemoryFdExtensionName);
        if (semFd)
            enabled.push_back(vk::KHRExternalSemaphoreFdExtensionName);

        // AHardwareBuffer external memory (Android frame exchange).
        const bool ahb{hasSupport(vk::ANDROIDExternalMemoryAndroidHardwareBufferExtensionName)};
        if (ahb)
            enabled.push_back(vk::ANDROIDExternalMemoryAndroidHardwareBufferExtensionName);

        logExt(vk::ANDROIDExternalMemoryAndroidHardwareBufferExtensionName);
        logExt(vk::KHRSynchronization2ExtensionName);
        logExt(vk::KHRExternalMemoryFdExtensionName);
        logExt(vk::KHRExternalSemaphoreFdExtensionName);
        if (logCallback)
            logCallback(std::string("device apiVersion=") +
                std::to_string(props.apiVersion >> 22) + "." +
                std::to_string((props.apiVersion >> 12) & 0x3FF));

        // Require the extensions the pipeline actually depends on. On Android the
        // AHB extension is mandatory (createImageFromAhb imports the source and
        // destination buffers through it). The fd extensions are required for the
        // desktop exportFds path but are optional on Android.
#if defined(__ANDROID__)
        if (!ahb) {
            throw std::runtime_error(
                "device does not support VK_ANDROID_external_memory_android_hardware_buffer "
                "(required for AHardwareBuffer import on Android)");
        }
#else
        if (!memFd || !semFd) {
            throw std::runtime_error(
                "device is missing VK_KHR_external_memory_fd / VK_KHR_external_semaphore_fd "
                "(required for fd-based frame exchange)");
        }
#endif

        // The pipeline always calls pipelineBarrier2KHR, so synchronization2
        // must be resolvable. It is core at Vulkan 1.3+; below that the
        // extension must have been enabled above.
        if (!is13Plus && !sync2) {
            throw std::runtime_error(
                "device is below Vulkan 1.3 and lacks VK_KHR_synchronization2 "
                "(required by pipelineBarrier2KHR in the framegen pipeline)");
        }

        return enabled;
    }

    /// Create a Vulkan device, enabling only the extensions the physical device
    /// actually reports (see selectDeviceExtensions).
    std::pair<vk::UniqueDevice, vk::Queue> createDevice(
        vk::detail::DispatchLoaderDynamic& dld,
        const vk::PhysicalDevice& physdev,
        uint32_t qfi,
        bool fp16,
        std::function<void(const std::string&)> logCallback
    ) {
        auto extensions{selectDeviceExtensions(dld, physdev, logCallback)};

        // Request the Vulkan 1.2 core feature set (timeline semaphores, optional
        // FP16) and the KHR synchronization2 feature. Match the upstream pNext
        // chain exactly (Synchronization2FeaturesKHR chained to Vulkan12Features).
        VkPhysicalDeviceSynchronization2FeaturesKHR sync2Info{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR,
            .synchronization2 = VK_TRUE
        };
        vk::PhysicalDeviceVulkan12Features vk12Info{
            .sType = vk::StructureType::ePhysicalDeviceVulkan12Features,
            .pNext = &sync2Info,
            .shaderFloat16 = fp16,
            .timelineSemaphore = vk::True
        };

        const float queuePriority{1.0F}; // Highest priority
        const vk::DeviceQueueCreateInfo queueInfo{
            .queueFamilyIndex = qfi,
            .queueCount = 1,
            .pQueuePriorities = &queuePriority
        };
        const vk::DeviceCreateInfo deviceInfo{
            .pNext = &vk12Info,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &queueInfo,
            .enabledExtensionCount = static_cast<uint32_t>(extensions.size()),
            .ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data()
        };
        auto device{physdev.createDeviceUnique(deviceInfo, nullptr, dld)};
        // Populate device-level function pointers (including the AHB extension's
        // vkGetAndroidHardwareBufferPropertiesANDROID) from the created device.
        dld.init(*device);

        return{
            std::move(device),
            device->getQueue(qfi, 0, dld)
        };
    }
}

Instance::Instance(
    const std::string& deviceId,
    const std::filesystem::path& lsfgvkDllPath,
    bool allowFP16,
    bool leakInstance,
    std::function<void(const std::string&)> logCallback
) {
    // Create Vulkan context
    auto dld{std::make_unique<vk::detail::DispatchLoaderDynamic>()};

    auto instance{vkhelper::createInstance(
        *dld,
        "lsfg-vk",
        vk::ApiVersion12 // Fully supported by all Vulkan-capable GPUs
    )};
    const auto physdev{vkhelper::findPhysicalDevice(*dld, *instance, deviceId)};

    const uint32_t qfi{vkhelper::findQueueFamilyIndex(
        *dld,
        physdev,
        vk::QueueFlagBits::eCompute
    )};
    const bool fp16{allowFP16 && vkhelper::checkHalfPrecisionSupport(*dld, physdev)};

    auto [device, queue] = createDevice(*dld, physdev, qfi, fp16, logCallback);

    // Construct instance
    ShaderLibrary library{
        *dld,
        *device,
        fp16,
        lsfgvkDllPath,
        logCallback
    };

    this->m_impl = std::make_unique<impl::Instance>(impl::Instance {
        .vk = {
            .dld = std::move(dld),
            .instance = std::move(instance),
            .physdev = physdev,
            .device = std::move(device),
            .queue = queue,
            .qfi = qfi,
            .fp16 = fp16,
            .leakInstance = leakInstance
        },
        .shaderLibrary = std::move(library),
        .logCallback = std::move(logCallback)
    });
}

Context::Context(
    const Instance& instance,
    uint32_t width,
    uint32_t height,
    float flowScale,
    bool performanceMode,
    BitDepth depth,
    ColorSpace colorSpace,
    TransferFunction transferFunction,
    const void* ahbInput,
    const void* ahbOutput
) {
    (void)ahbInput;
    (void)ahbOutput;
    const auto& vk{instance.m_impl->vk};

    Pipeline pipeline{
        *vk.dld,
        *vk.device,
        vk.physdev,
        vk.queue,
        vk.qfi,
        instance.m_impl->shaderLibrary,
        v3_1::getPipelineSignature(performanceMode),
        { width, height },
        flowScale,
        performanceMode,
        static_cast<uint8_t>(depth),
        colorSpace,
        transferFunction,
        instance.m_impl->logCallback
    };

    this->m_impl = std::make_unique<impl::Context>(impl::Context {
        .instance = std::ref(*instance.m_impl),
        .pipeline = std::move(pipeline),
        // The sync semaphore is created as a timeline semaphore, but NOT with an
        // exported OPAQUE_FD handle.
        //
        // Requesting the opaque-fd export unconditionally made session creation
        // fail outright on Mesa/Turnip with
        //   vk::Device::createSemaphoreUnique: -1000072003
        // which decodes to VK_ERROR_INVALID_EXTERNAL_HANDLE, throwing away an
        // otherwise fully built pipeline (26 pipelines, 46 stages, 101 images).
        //
        // The export was only ever needed by Context::exportFds() on the desktop
        // fd-sharing path. On Android this app exchanges frames through
        // AHardwareBuffer imports plus its own blit fence, and drives the
        // pipeline with the unsignaled (self-driven) mode, so an exportable
        // semaphore buys nothing and only costs portability.
        .syncSemaphore = { vkhelper::createSemaphore(*vk.dld, *vk.device, true, false), 0 },
        .internalSemaphores = { vkhelper::createSemaphore(*vk.dld, *vk.device, true), 0 },
        .fence = { vkhelper::createFence(*vk.dld, *vk.device), false }
    });
}

FileDescriptors Context::exportFds() const {
    const auto& vk{this->m_impl->instance.get().vk};
    const auto& pipeline{this->m_impl->pipeline};

    return{
        .sourceFd = vkhelper::exportMemoryFd(
            *vk.dld, *vk.device,
            pipeline.getExternalInputs().front().memory
        ),
        .destinationFd = vkhelper::exportMemoryFd(
            *vk.dld, *vk.device,
            pipeline.getExternalOutputs().front().memory
        ),
        .syncFd = vkhelper::exportSemaphoreFd(
            *vk.dld, *vk.device,
            *this->m_impl->syncSemaphore.first
        )
    };
}

void Context::dispatch(uint32_t total, bool unsignaled) {
    auto& ctx{*this->m_impl};
    const auto& vk{ctx.instance.get().vk};

    // Wait for the previous iteration to complete, then reset the fence.
    //
    // This wait is BOUNDED and TOLERATED, matching the reference layer
    // (lsfg-vk-layer/src/wrapper.cpp: `const auto _ = waitForFences(..., 1s)`,
    // result discarded, then resetFences).
    //
    // Our port previously waited with UINT64_MAX and THREW on any non-success.
    // That is stricter than the reference and it converts ONE unresolved frame
    // into a permanent failure: the throw escapes dispatch(), the iterator and
    // semaphore counters keep the values they had mid-cycle, and every later
    // iteration re-enters in the same broken state (observed on Adreno as a wall
    // of identical `waitForFences: -3` immediately after the first cycle).
    //
    // -3 here is VK_ERROR_INITIALIZATION_FAILED, not VK_TIMEOUT (which is 2).
    // It is reported rather than fatal, which is what lets the pipeline recover
    // on the next iteration instead of wedging for the rest of the session.
    if (ctx.fence.second) {
        // Raw dispatch-loader call, for the same reason as waitFenceRaw in
        // lsfg_jni.cpp: the checked Vulkan-Hpp wrapper THROWS for -3 before the
        // result reaches us, which made the tolerated branch below unreachable
        // and turned every frame into a hard dispatch failure. Going through
        // vk.dld->vkWaitForFences gives us the code itself.
        const VkFence fenceHandle{static_cast<VkFence>(*ctx.fence.first)};
        const VkDevice devHandle{static_cast<VkDevice>(*vk.device)};
        const vk::Result result{static_cast<vk::Result>(
            vk.dld->vkWaitForFences(
                devHandle, 1, &fenceHandle, VK_TRUE, 1'000'000'000ULL))};
        if (result == vk::Result::eSuccess) {
            vk.device->resetFences(*ctx.fence.first, *vk.dld);
        } else {
            // The wait failed, so this fence is NOT in a usable state. Do not
            // touch it further.
            //
            // vkResetFences on such a fence aborts inside the driver:
            //   vkResetFences -> gsl_syncobj_destroy -> close(0) -> fdsan abort
            // (confirmed on device, tombstone_08, dispatched from this exact
            // line). -3 is VK_ERROR_INITIALIZATION_FAILED, i.e. the driver is
            // telling us the fence handle is unusable, NOT that we timed out.
            //
            // Leaving it unreset just means the next cycle skips the wait, which
            // is harmless: an unsignalled fence only ever gates the NEXT
            // dispatch, and a failed wait already means the previous iteration is
            // not going to complete. Resetting or recreating it re-enters the
            // same crashing driver path, so we deliberately do neither.
            if (ctx.instance.get().logCallback) {
                ctx.instance.get().logCallback(
                    std::string("dispatch: previous-iteration fence wait returned ") +
                    std::to_string(static_cast<int>(result)) +
                    " (" +
                    (result == vk::Result::eTimeout ? "VK_TIMEOUT"
                        : result == vk::Result::eErrorInitializationFailed
                            ? "VK_ERROR_INITIALIZATION_FAILED"
                            : "other") +
                    "); fence left alone, continuing"
                );
            }
        }

        ctx.fence.second = false;
    }

    auto* mapped{ctx.pipeline.getMappedBuffer()};
    if (ctx.firstIteration) {
        ctx.firstIteration = false;
        mapped->iteration = 0;
    } else {
        mapped->iteration++;
    }

    // Dispatch pre-pass
    const auto& cmdbuf{ctx.pipeline.getCmdbufs().at(0)};

    auto& sync{ctx.syncSemaphore};
    sync.second++;

    auto& internal{ctx.internalSemaphores};
    internal.second++;

    vk::TimelineSemaphoreSubmitInfo timelineInfo{
        .waitSemaphoreValueCount = unsignaled ? 0U : 1U,
        .pWaitSemaphoreValues = &sync.second,
        .signalSemaphoreValueCount = 1,
        .pSignalSemaphoreValues = &internal.second
    };

    const vk::PipelineStageFlags waitStage{vk::PipelineStageFlagBits::eTopOfPipe};
    vk.queue.submit(
        {{
            .pNext = &timelineInfo,
            .waitSemaphoreCount = timelineInfo.waitSemaphoreValueCount,
            .pWaitSemaphores = &*sync.first,
            .pWaitDstStageMask = &waitStage,
            .commandBufferCount = 1U,
            .pCommandBuffers = &*cmdbuf,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &*internal.first
        }},
        nullptr,
        *vk.dld
    );

    // Update context with dispatch info
    ctx.iteration = mapped->iteration;
    ctx.total = total;
    ctx.index = 0;
}

void Context::acquire(bool unsignaled) {
    auto& ctx{*this->m_impl};
    const auto& vk{ctx.instance.get().vk};

    if (ctx.total == 0 || ctx.index >= ctx.total) {
        // Report the actual counters, so a mismatch against the caller's loop is
        // visible as numbers instead of inferred. The Kotlin driver decides its
        // acquire count from the multiplier, so if these disagree the fault is on
        // the caller's side of the boundary, not in the pipeline.
        throw std::runtime_error(
            "No more frames to acquire (index=" + std::to_string(ctx.index) +
            " total=" + std::to_string(ctx.total) +
            " iteration=" + std::to_string(ctx.iteration) + ")"
        );
    }

    // Build transition command buffer to transition the timestamp
    const auto& transCmdbuf{ctx.pipeline.buildTransCmdbuf(
        *vk.dld, *vk.device,
        ctx.iteration,
        ctx.index, ctx.total
    )};

    auto& sync{ctx.syncSemaphore};
    auto& internal{ctx.internalSemaphores};

    vk::TimelineSemaphoreSubmitInfo timelineInfo{
        .waitSemaphoreValueCount = 1,
        .signalSemaphoreValueCount = 1
    };

    uint64_t prevInternal{};
    if (ctx.index == 0) {
        prevInternal = internal.second;
        timelineInfo.pWaitSemaphoreValues = &prevInternal;
    } else if (!unsignaled) {
        sync.second++;
        timelineInfo.pWaitSemaphoreValues = &sync.second;
    } else {
        timelineInfo.waitSemaphoreValueCount = 0;
    }

    internal.second++;
    timelineInfo.pSignalSemaphoreValues = &internal.second;

    const vk::PipelineStageFlags waitStage{vk::PipelineStageFlagBits::eTopOfPipe};
    vk.queue.submit(
        {{
            .pNext = &timelineInfo,
            .waitSemaphoreCount = timelineInfo.waitSemaphoreValueCount,
            .pWaitSemaphores = ctx.index == 0 ? &*internal.first : &*sync.first,
            .pWaitDstStageMask = &waitStage,
            .commandBufferCount = 1,
            .pCommandBuffers = &transCmdbuf,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &*internal.first
        }},
        nullptr,
        *vk.dld
    );

    // Dispatch main pass
    const auto& cmdbuf{ctx.pipeline.getCmdbufs().at(1)};

    timelineInfo.pWaitSemaphoreValues = &internal.second;

    sync.second++;
    timelineInfo.pSignalSemaphoreValues = &sync.second;

    if (unsignaled) {
        timelineInfo.waitSemaphoreValueCount = 1;
        timelineInfo.signalSemaphoreValueCount = 0;
    }

    vk.queue.submit(
        {{
            .pNext = &timelineInfo,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &*internal.first,
            .pWaitDstStageMask = &waitStage,
            .commandBufferCount = 1,
            .pCommandBuffers = &*cmdbuf,
            .signalSemaphoreCount = unsignaled ? 0U : 1U,
            .pSignalSemaphores = &*sync.first
        }},
        ctx.index == (ctx.total - 1) ? *ctx.fence.first : nullptr,
        *vk.dld
    );

    if (ctx.index == (ctx.total - 1)) {
        ctx.fence.second = true;
    }

    ctx.index++;
}

void Context::idle() {
    if (!this->m_impl->fence.second)
        return;

    auto& ctx{*this->m_impl};
    const auto& vk{this->m_impl->instance.get().vk};

    const auto result{vk.device->waitForFences(*ctx.fence.first, true, UINT64_MAX, *vk.dld)};
    if (result != vk::Result::eSuccess)
        throw std::runtime_error("Unable to wait for completion of current iteration");
}

Context::~Context() {
    try {
        this->idle();
    } catch (...) { // NOLINT (empty catch)
        // Ignore errors during destruction
    }
}

VkInstance Instance::_instance() const {
    return this->m_impl->vk.instance.get();
}

VkDevice Instance::_device() const {
    return *this->m_impl->vk.device;
}

const impl::Instance& Instance::_impl() const {
    return *this->m_impl;
}

Pipeline& Context::pipeline() {
    return this->m_impl->pipeline;
}

Instance::~Instance() {
    // Skip cleanup when leakInstance is set (handles are left for the process
    // to reclaim). Otherwise let the RAII owners do the destruction:
    // reset() destroys the handle AND clears ownership, so the member
    // destructors that run afterwards are no-ops (a manual destroy() while
    // the wrappers still owned the handles would destroy them twice).
    //
    // The implicit member order already matches Vulkan's requirements:
    // shaderLibrary (device objects) is destroyed before vk, and inside vk
    // the device is declared before the instance while the dispatch loader
    // (dld) is declared first and outlives both.
    if (this->m_impl->vk.leakInstance)
        return;

    try {
        // Shader modules are device objects and must be destroyed first.
        this->m_impl->shaderLibrary.destroy();
        if (this->m_impl->vk.device)
            this->m_impl->vk.device.reset();
        if (this->m_impl->vk.instance)
            this->m_impl->vk.instance.reset();
    } catch (...) {
        if (!this->m_impl->logCallback)
            return;
        this->m_impl->logCallback("Failed to destroy Vulkan instance and device");
    }
}
