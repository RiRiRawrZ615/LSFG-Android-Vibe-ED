/* SPDX-License-Identifier: CC-BY-NC-ND-4.0 */

#include "vkhelper.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ios>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <vk>

#if defined(__ANDROID__)
#include <android/hardware_buffer.h>
#endif

/* Device initialization */

vk::UniqueInstance vkhelper::createInstance(
    vk::detail::DispatchLoaderDynamic& dld,
    const std::string& name,
    uint32_t version
) {
    dld.init();

    const vk::ApplicationInfo appInfo{
        .pApplicationName = name.c_str(),
        .applicationVersion = vk::makeVersion(2, 0, 0),
        .pEngineName = name.c_str(),
        .engineVersion = vk::makeVersion(2, 0, 0),
        .apiVersion = version
    };
    const vk::InstanceCreateInfo instanceInfo{
        .pApplicationInfo = &appInfo
    };
    auto instance{vk::createInstanceUnique(instanceInfo, nullptr, dld)};
    dld.init(*instance);

    return instance;
}

vk::PhysicalDevice vkhelper::findPhysicalDevice(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Instance& instance,
    const std::string& id
) {
    for (const auto& physdev : instance.enumeratePhysicalDevices(dld)) {
        // Check for VK_EXT_pci_bus_info
        bool supportsPCIEXT{false};
        for (const auto& ext : physdev.enumerateDeviceExtensionProperties(nullptr, dld)) {
            if (std::string(ext.extensionName) != vk::EXTPciBusInfoExtensionName)
                continue;

            supportsPCIEXT = true;
            break;
        }

        // Fetch properties
        vk::PhysicalDevicePCIBusInfoPropertiesEXT busInfo{};
        vk::PhysicalDeviceProperties2 info{
            .pNext = supportsPCIEXT ? &busInfo : nullptr
        };
        physdev.getProperties2(&info, dld);

        auto& props{info.properties};

        // Check first if id is not given
        if (id.empty())
            return physdev;

        // Compare device name
        props.deviceName.back() = '\0'; // Ensure null-termination
        if (id == std::string(props.deviceName))
            return physdev;

        // Compare Vendor ID + Device ID
        std::ostringstream gpuss;
        gpuss << std::hex << std::setfill('0')
            << std::setw(4) << props.vendorID << ":"
            << std::setw(4) << props.deviceID;
        if (id == gpuss.str())
            return physdev;

        // Compare PCI bus ID
        if (!supportsPCIEXT)
            continue;

        std::ostringstream pciss;
        pciss << std::hex << std::setfill('0')
            << std::setw(4) << busInfo.pciDomain << ":"
            << std::setw(2) << busInfo.pciBus << ":"
            << std::setw(2) << busInfo.pciDevice << "."
            << std::setw(1) << busInfo.pciFunction;
        if (id == pciss.str())
            return physdev;
    }

    throw std::runtime_error("No physical device matching '" + id + "' found");
}

uint32_t vkhelper::findQueueFamilyIndex(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::PhysicalDevice& physdev,
    vk::QueueFlags flags,
    bool requireDedicated
) {
    uint32_t idx{0};
    for (const auto& family : physdev.getQueueFamilyProperties(dld)) {
        if (requireDedicated && (family.queueFlags & vk::QueueFlagBits::eGraphics))
            continue;
        if (family.queueFlags & flags)
            return idx;
        idx++;
    }

    throw std::runtime_error("No compute-capable queue family found");
}

bool vkhelper::checkHalfPrecisionSupport(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::PhysicalDevice& physdev
) {
    vk::PhysicalDeviceVulkan12Features featuresVulkan12{};
    vk::PhysicalDeviceFeatures2 features{
        .pNext = &featuresVulkan12
    };
    physdev.getFeatures2(&features, dld);
    return featuresVulkan12.shaderFloat16;
}

/* Shader modules & pipelines */

vk::UniqueShaderModule vkhelper::createShaderModule(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const std::span<const uint32_t>& code
) {
    const vk::ShaderModuleCreateInfo shaderInfo{
        .codeSize = code.size() * sizeof(uint32_t),
        .pCode = code.data()
    };
    return device.createShaderModuleUnique(shaderInfo, nullptr, dld);
}

namespace {
    /// Find the cache file path
    std::filesystem::path findPipelineCache(
        const vk::detail::DispatchLoaderDynamic& dld,
        const vk::PhysicalDevice& physdev,
        std::string_view tag
    ) {
        // First find the base path
        std::filesystem::path path{"/tmp/lsfg-vk"};

        const char* home{std::getenv("HOME")};
        if (home && *home != '\0')
            path = std::filesystem::path(home) / ".cache" / "lsfg-vk";

        const char* xdgCacheHome{std::getenv("XDG_CACHE_HOME")};
        if (xdgCacheHome && *xdgCacheHome != '\0')
            path = std::filesystem::path(xdgCacheHome) / "lsfg-vk";

        // Ensure the directory exists
        if (!std::filesystem::exists(path))
            std::filesystem::create_directories(path);

        // Calculate the physical device UUID
        const auto info{physdev.getProperties(dld)};

        std::ostringstream ss;
        ss << std::hex << std::setfill('0');
        for (size_t i = 0; i < 16; i++) {
            ss << std::setw(2) << static_cast<uint32_t>(info.pipelineCacheUUID.at(i));
            if (i == 3 || i == 5 || i == 7 || i == 9) {
                ss << "-";
            }
        }

        // Return the full path
        return path / ("cache_" + std::string(tag) + "_" + ss.str() + ".bin");
    }
}

std::pair<vk::UniquePipelineCache, bool> vkhelper::createPipelineCache(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const vk::PhysicalDevice& physdev,
    std::string_view tag
) {
    const std::filesystem::path path{findPipelineCache(dld, physdev, tag)};
    const bool valid{std::filesystem::exists(path) && std::filesystem::file_size(path) > 32};

    // Read cache data (if any)
    std::vector<uint8_t> cacheData{};
    if (std::filesystem::exists(path)) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open())
            throw std::runtime_error("Unable to open pipeline cache file for reading");

        const auto size{static_cast<std::streamsize>(file.tellg())};
        cacheData = std::vector<uint8_t>(static_cast<size_t>(size));

        file.seekg(0, std::ios::beg);
        if (!file.read(reinterpret_cast<char*>(cacheData.data()), size)) // NOLINT (unsafe cast)
            throw std::runtime_error("Unable to read pipeline cache file");
    }

    // Build pipeline cache
    const vk::PipelineCacheCreateInfo pipelineCacheInfo{
        .initialDataSize = cacheData.size(),
        .pInitialData = cacheData.data()
    };
    return { device.createPipelineCacheUnique(pipelineCacheInfo, nullptr, dld), valid };
}

void vkhelper::persistPipelineCache(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const vk::PhysicalDevice& physdev,
    const vk::PipelineCache& cache,
    std::string_view tag
) {
    const std::filesystem::path path{findPipelineCache(dld, physdev, tag)};

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
        throw std::runtime_error("Unable to open pipeline cache file for writing");

    const std::vector<uint8_t> cacheData{
        device.getPipelineCacheData(cache, dld)
    };
    file.write(
        reinterpret_cast<const char*>(cacheData.data()), // NOLINT (unsafe cast)
        static_cast<std::streamsize>(cacheData.size())
    );

    file.flush();
    file.close();
}

std::pair<vk::UniqueDescriptorSetLayout, vk::UniquePipelineLayout> vkhelper::createLayout(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const std::vector<vk::DescriptorSetLayoutBinding>& bindings,
    size_t pushConstantSize
) {
    const vk::DescriptorSetLayoutCreateInfo layoutInfo{
        .flags = vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool,
        .bindingCount = static_cast<uint32_t>(bindings.size()),
        .pBindings = bindings.data()
    };
    auto descriptorSetLayout{device.createDescriptorSetLayoutUnique(layoutInfo, nullptr, dld)};

    const vk::PushConstantRange pushConstantRange{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .size = static_cast<uint32_t>(pushConstantSize)
    };
    const vk::PipelineLayoutCreateInfo pipelineLayoutInfo{
        .setLayoutCount = 1,
        .pSetLayouts = &*descriptorSetLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushConstantRange
    };
    auto pipelineLayout{device.createPipelineLayoutUnique(pipelineLayoutInfo, nullptr, dld)};

    return { std::move(descriptorSetLayout), std::move(pipelineLayout) };
}

/* Resources */

vk::UniqueImage vkhelper::createImage(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    vk::Extent2D extent,
    vk::Format format,
    uint32_t layers,
    vk::ImageUsageFlags usage,
    bool external
) {
    const vk::ExternalMemoryImageCreateInfo externalInfo{
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd
    };
    const vk::ImageCreateInfo imageInfo{
        .pNext = external ? &externalInfo : nullptr,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {
            .width = extent.width,
            .height = extent.height,
            .depth = 1
        },
        .mipLevels = 1,
        .arrayLayers = layers,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = usage
    };
    return device.createImageUnique(imageInfo, nullptr, dld);
}

#if defined(__ANDROID__)
vk::UniqueImage vkhelper::createImageFromAhb(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const void* buffer,
    vk::ImageUsageFlags usage
) {
    const auto* ahb{static_cast<const AHardwareBuffer*>(buffer)};
    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(ahb, &desc);
    if (desc.width == 0 || desc.height == 0)
        throw std::runtime_error("AHardwareBuffer_describe returned an empty buffer");

    // Map the AHardwareBuffer pixel format to a Vulkan format.
    vk::Format format = vk::Format::eR8G8B8A8Unorm;
    switch (desc.format) {
        case AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM:
            format = vk::Format::eR8G8B8A8Unorm;
            break;
        case AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM:
            format = vk::Format::eA2B10G10R10UnormPack32;
            break;
        case AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT:
            format = vk::Format::eR16G16B16A16Sfloat;
            break;
        case AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM:
            format = vk::Format::eR8G8B8Unorm;
            break;
        default:
            break;
    }

    const vk::Extent2D extent{
        static_cast<uint32_t>(desc.width),
        static_cast<uint32_t>(desc.height)
    };

    // Create an external, importable image with TRANSFER usage for blits.
    auto image{createImage(
        dld,
        device,
        extent,
        format,
        1,
        usage,
        true
    )};

    // Query which memory type bits the AHardwareBuffer supports and pick one that is
    // also usable by this image.
    vk::AndroidHardwareBufferPropertiesANDROID props{};
    if (device.getAndroidHardwareBufferPropertiesANDROID(ahb, &props, dld) != vk::Result::eSuccess)
        throw std::runtime_error("vkGetAndroidHardwareBufferPropertiesANDROID failed");

    const auto requirements{device.getImageMemoryRequirements(*image, dld)};
    // The AHardwareBuffer import must use a memory type that is both usable by the image
    // and supported by the buffer. Prefer the lowest such index.
    const uint32_t usable{static_cast<uint32_t>(requirements.memoryTypeBits) & props.memoryTypeBits};
    const uint32_t typeBits{usable ? usable : static_cast<uint32_t>(requirements.memoryTypeBits)};
    uint32_t memoryTypeIndex = 0;
    for (uint32_t i = 0; i < 64; ++i) {
        if (typeBits & (1u << i)) {
            memoryTypeIndex = i;
            break;
        }
    }

    auto* ahbMutable{const_cast<AHardwareBuffer*>(ahb)};
    const vk::ImportAndroidHardwareBufferInfoANDROID importInfo{
        .buffer = ahbMutable
    };
    const vk::MemoryAllocateInfo allocInfo{
        .pNext = &importInfo,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memoryTypeIndex
    };
    auto allocation{device.allocateMemoryUnique(allocInfo, nullptr, dld)};

    device.bindImageMemory(*image, *allocation, 0, dld);

    return image;
}
#endif

vk::UniqueSampler vkhelper::createSampler(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    vk::SamplerAddressMode mode,
    vk::CompareOp compare,
    bool white
) {
    const vk::SamplerCreateInfo samplerInfo{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eLinear,
        .addressModeU = mode,
        .addressModeV = mode,
        .addressModeW = mode,
        .compareOp = compare,
        .maxLod = vk::LodClampNone,
        .borderColor = white ?
            vk::BorderColor::eFloatOpaqueWhite : vk::BorderColor::eFloatTransparentBlack
    };
    return device.createSamplerUnique(samplerInfo, nullptr, dld);
}

std::pair<vk::UniqueBuffer, vk::UniqueDeviceMemory> vkhelper::createBuffer(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const vk::PhysicalDevice& physdev,
    vk::BufferUsageFlags usage,
    const void* data,
    size_t size
) {
    // Create buffer
    const vk::BufferCreateInfo bufferInfo{
        .size = size,
        .usage = usage,
        .sharingMode = vk::SharingMode::eExclusive
    };
    auto buffer{device.createBufferUnique(bufferInfo, nullptr, dld)};

    // Allocate memory
    const auto requirements{device.getBufferMemoryRequirements(*buffer, dld)};

    auto memory{vkhelper::allocateMemory(
        dld,
        device,
        physdev,
        requirements.size,
        requirements.memoryTypeBits,
        true
    )};

    // Bind memory
    device.bindBufferMemory(*buffer, *memory, 0, dld);

    // Copy data
    if (data) {
        void* mapped{device.mapMemory(*memory, 0, size, {}, dld)};
        std::copy_n(
            static_cast<const uint8_t*>(data),
            size,
            static_cast<uint8_t*>(mapped)
        );
        device.unmapMemory(*memory, dld);
    }

    return {
        std::move(buffer),
        std::move(memory)
    };
}

/* Memory allocations */

vk::UniqueDeviceMemory vkhelper::allocateMemory(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const vk::PhysicalDevice& physdev,
    vk::DeviceSize size,
    std::bitset<32> types,
    bool hostVisible,
    const void* next
) {
    // Find a suitable memory type index
    const auto memProps{physdev.getMemoryProperties(dld)};

    std::optional<uint32_t> idealTypeIdx{};
    std::optional<uint32_t> fallbackTypeIdx{};
    for (size_t i = 0; i < memProps.memoryTypeCount; i++) {
        if (!types.test(i))
            continue;
        const auto& memType{memProps.memoryTypes.at(i)};

        // Require host visibility when requested
        const bool isHostVisible{
            memType.propertyFlags & vk::MemoryPropertyFlagBits::eHostVisible &&
            memType.propertyFlags & vk::MemoryPropertyFlagBits::eHostCoherent
        };
        if (hostVisible && !isHostVisible)
            continue;

        // Always prefer device local memory
        const bool isDeviceLocal{
            memType.propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal
        };
        if (isDeviceLocal && !idealTypeIdx.has_value()) {
            idealTypeIdx = i;
        }

        // Fallback to host-visible memory if no device-local memory is available
        if (!isDeviceLocal && !fallbackTypeIdx.has_value()) {
            fallbackTypeIdx = i;
        }
    }

    // Allocate memory
    try {
        const vk::MemoryAllocateInfo allocInfo{
            .pNext = next,
            .allocationSize = size,
            .memoryTypeIndex = *idealTypeIdx // Throws when not present
        };
        return device.allocateMemoryUnique(allocInfo, nullptr, dld);
    } catch (...) {
        if (!fallbackTypeIdx)
            throw "No suitable memory type found for allocation";
        const vk::MemoryAllocateInfo allocInfo{
            .pNext = next,
            .allocationSize = size,
            .memoryTypeIndex = *fallbackTypeIdx
        };
        return device.allocateMemoryUnique(allocInfo, nullptr, dld);
    }
}

/* Descriptors */

std::pair<vk::UniqueDescriptorPool, vk::DescriptorSet> vkhelper::createDescriptorSet(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const vk::DescriptorSetLayout& layout,
    uint32_t samplers, uint32_t buffers,
    uint32_t sampledImages, uint32_t storageImages
) {
    const std::array<vk::DescriptorPoolSize, 4> poolSizes{{
        { .type = vk::DescriptorType::eSampler,
          .descriptorCount = samplers },
        { .type = vk::DescriptorType::eSampledImage,
          .descriptorCount = sampledImages },
        { .type = vk::DescriptorType::eStorageImage,
          .descriptorCount = storageImages },
        { .type = vk::DescriptorType::eUniformBuffer,
          .descriptorCount = buffers }
    }};
    auto pool{device.createDescriptorPoolUnique({
        .flags = vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind,
        .maxSets = 1,
        .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
        .pPoolSizes = poolSizes.data()
    }, nullptr, dld)};

    const auto set{device.allocateDescriptorSets({
        .descriptorPool = *pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &layout
    }, dld).at(0)};

    return{
        std::move(pool),
        set
    };
}

vk::UniqueImageView vkhelper::createImageView(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const vk::Image& image,
    vk::Format format,
    uint32_t layers
) {
    const vk::ImageViewCreateInfo viewInfo{
        .image = image,
        .viewType = layers == 1 ? vk::ImageViewType::e2D : vk::ImageViewType::e2DArray,
        .format = format,
        .subresourceRange = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = layers
        }
    };
    return device.createImageViewUnique(viewInfo, nullptr, dld);
}

/* Command buffers */

vk::UniqueCommandPool vkhelper::createCommandPool(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    uint32_t qfi,
    bool resettable
) {
    const vk::CommandPoolCreateInfo cmdpoolInfo{
        .flags = resettable ? vk::CommandPoolCreateFlagBits::eResetCommandBuffer
            : vk::CommandPoolCreateFlagBits{0},
        .queueFamilyIndex = qfi,
    };
    return device.createCommandPoolUnique(cmdpoolInfo, nullptr, dld);
}

vk::UniqueCommandBuffer vkhelper::createCommandBuffer(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const vk::CommandPool& cmdpool
) {
    const vk::CommandBufferAllocateInfo cmdbufInfo{
        .commandPool = cmdpool,
        .commandBufferCount = 1
    };
    return { std::move(device.allocateCommandBuffersUnique(cmdbufInfo, dld).front()) };
}

void vkhelper::blitImage(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::CommandBuffer& cmdbuf,
    const vk::Image& srcImage,
    uint32_t srcLayer,
    const vk::Image& dstImage,
    uint32_t dstLayer,
    vk::Extent2D extent
) {
    const vk::ImageBlit blitInfo{
        .srcSubresource = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseArrayLayer = srcLayer,
            .layerCount = 1,
        },
        .srcOffsets = {{
            vk::Offset3D { 0, 0, 0 },
            { static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1 }
        }},
        .dstSubresource = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseArrayLayer = dstLayer,
            .layerCount = 1,
        },
        .dstOffsets = {{
            vk::Offset3D { 0, 0, 0 },
            { static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1 }
        }}
    };
    cmdbuf.blitImage(
        srcImage, vk::ImageLayout::eGeneral,
        dstImage, vk::ImageLayout::eGeneral,
        blitInfo,
        vk::Filter::eNearest,
        dld
    );
}

/* Synchronization */

vk::UniqueSemaphore vkhelper::createSemaphore(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    bool timeline,
    bool external
) {
    const vk::ExportSemaphoreCreateInfo exportInfo{
        .handleTypes = vk::ExternalSemaphoreHandleTypeFlagBits::eOpaqueFd
    };
    const vk::SemaphoreTypeCreateInfo typeInfo{
        .pNext = external ? &exportInfo : nullptr,
        .semaphoreType = vk::SemaphoreType::eTimeline,
    };
    const vk::SemaphoreCreateInfo createInfo{
        .pNext = timeline ? static_cast<const void*>(&typeInfo)
            : (external ? &exportInfo : nullptr) // NOLINT (ternary)
    };
    return device.createSemaphoreUnique(createInfo, nullptr, dld);
}

vk::UniqueFence vkhelper::createFence(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device
) {
    return device.createFenceUnique({}, nullptr, dld);
}

/* External memory */

int vkhelper::exportMemoryFd(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const vk::DeviceMemory& memory
) {
    const vk::MemoryGetFdInfoKHR fdInfo{
        .memory = memory,
        .handleType = vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd
    };
    return device.getMemoryFdKHR(fdInfo, dld);
}

int vkhelper::exportSemaphoreFd(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const vk::Semaphore& semaphore
) {
    const vk::SemaphoreGetFdInfoKHR fdInfo{
        .semaphore = semaphore,
        .handleType = vk::ExternalSemaphoreHandleTypeFlagBits::eOpaqueFd
    };
    return device.getSemaphoreFdKHR(fdInfo, dld);
}

void vkhelper::importSemaphoreFd(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const vk::Semaphore& semaphore,
    int fd
) {
    const vk::ImportSemaphoreFdInfoKHR importInfo{
        .semaphore = semaphore,
        .handleType = vk::ExternalSemaphoreHandleTypeFlagBits::eOpaqueFd,
        .fd = fd
    };
    device.importSemaphoreFdKHR(importInfo, dld);
}
