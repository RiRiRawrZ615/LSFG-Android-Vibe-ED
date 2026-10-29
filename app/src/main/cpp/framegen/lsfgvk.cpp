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
#include <vk>

using namespace lsfgvk;

namespace {
    /// Create a Vulkan device
    std::pair<vk::UniqueDevice, vk::Queue> createDevice(
        vk::detail::DispatchLoaderDynamic& dld,
        const vk::PhysicalDevice& physdev,
        uint32_t qfi,
        bool fp16
    ) {
        constexpr std::array<const char*, 3> EXTENSIONS{
            vk::KHRSynchronization2ExtensionName,
            vk::KHRExternalMemoryFdExtensionName,
            vk::KHRExternalSemaphoreFdExtensionName
        };

        vk::PhysicalDeviceSynchronization2FeaturesKHR sync2Info{
            .synchronization2 = vk::True
        };
        const vk::PhysicalDeviceVulkan12Features vk12Info{
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
            .enabledExtensionCount = static_cast<uint32_t>(EXTENSIONS.size()),
            .ppEnabledExtensionNames = EXTENSIONS.data()
        };
        auto device{physdev.createDeviceUnique(deviceInfo, nullptr, dld)};
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

    auto [device, queue] = createDevice(*dld, physdev, qfi, fp16);

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
        .syncSemaphore = { vkhelper::createSemaphore(*vk.dld, *vk.device, true, true), 0 },
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

    // Increment iteration counter after previous frame is completed
    if (ctx.fence.second) {
        const auto result{vk.device->waitForFences(*ctx.fence.first, true, UINT64_MAX, *vk.dld)};
        if (result != vk::Result::eSuccess)
            throw std::runtime_error("Unable to wait for completion of previous iteration");
        vk.device->resetFences(*ctx.fence.first, *vk.dld);

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
        throw std::runtime_error("No more frames to acquire");
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
    if (!this->m_impl->vk.leakInstance)
        return;

    try {
        new std::unique_ptr<vk::detail::DispatchLoaderDynamic>(std::move(this->m_impl->vk.dld));
        new vk::UniqueDevice(std::move(this->m_impl->vk.device));
        new vk::UniqueInstance(std::move(this->m_impl->vk.instance));
    } catch (...) {
        if (!this->m_impl->logCallback)
            return;
        this->m_impl->logCallback("Failed to leak Vulkan instance and device");
    }
}
