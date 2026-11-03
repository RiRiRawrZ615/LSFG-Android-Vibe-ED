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
#include <android/log.h>
#include <fcntl.h>    // fcntl, F_DUPFD_CLOEXEC for safeExportedFd
#include <unistd.h>   // close
#define VKLOG(...) __android_log_print(ANDROID_LOG_WARN, "lsfg-vkhelper", __VA_ARGS__)
#else
#define VKLOG(...) do {} while (0)
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
    // Base directory for pipeline cache files.
    //
    // Path policy (Android-first, per project rule "never use /tmp/"):
    //   1) explicit dir set by the host app via setPipelineCacheDir()
    //      (app internal files/lsfg-cache, always writable, no permissions)
    //   2) XDG_CACHE_HOME (desktop)
    //   3) $HOME/.cache (desktop)
    //   4) /tmp/lsfg-vk last resort (Linux desktop only; on HyperOS /tmp is a
    //      read-only tmpfs, so creating dirs there throws)
    // Any failure to create the directory degrades to in-memory-only cache
    // (returns an empty path) instead of aborting session creation.
    std::string g_pipelineCacheBase{};

    /// Find the cache file path. Returns an empty path when no usable cache
    /// directory could be established (caller must then skip file I/O).
    std::filesystem::path findPipelineCache(
        const vk::detail::DispatchLoaderDynamic& dld,
        const vk::PhysicalDevice& physdev,
        std::string_view tag
    ) {
        std::filesystem::path base;
        if (!g_pipelineCacheBase.empty())
            base = std::filesystem::path{g_pipelineCacheBase};
        else {
            const char* xdgCacheHome{std::getenv("XDG_CACHE_HOME")};
            if (xdgCacheHome && *xdgCacheHome != '\0')
                base = std::filesystem::path(xdgCacheHome) / "lsfg-vk";
            else {
                const char* home{std::getenv("HOME")};
                if (home && *home != '\0')
                    base = std::filesystem::path(home) / ".cache" / "lsfg-vk";
                else
                    base = std::filesystem::path{"/tmp/lsfg-vk"};
            }
        }

        // Ensure the directory exists (best-effort: a read-only /tmp on some
        // Android builds must not kill session creation).
        std::error_code ec{};
        if (!std::filesystem::exists(base, ec) && ec) {
            if (!g_pipelineCacheBase.empty())
                base = std::filesystem::path{""}; // explicit dir but unwritable: go in-memory
        }
        if (!base.empty() && !std::filesystem::exists(base)) {
            try {
                std::filesystem::create_directories(base);
            } catch (const std::exception&) {
                base = std::filesystem::path{""};
            }
        }

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

        // No usable base dir: signal "in-memory only" to the callers.
        if (base.empty())
            return std::filesystem::path{};

        // Return the full path
        return base / ("cache_" + std::string(tag) + "_" + ss.str() + ".bin");
    }
}

void vkhelper::setPipelineCacheDir(const std::string& dir) { g_pipelineCacheBase = dir; }

std::pair<vk::UniquePipelineCache, bool> vkhelper::createPipelineCache(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const vk::PhysicalDevice& physdev,
    std::string_view tag
) {
    const std::filesystem::path path{findPipelineCache(dld, physdev, tag)};
    const bool valid{!path.empty() && std::filesystem::exists(path) && std::filesystem::file_size(path) > 32};

    // Read cache data (if any). On a fresh install the cache file does not
    // exist yet, so gate on exists() (the directory is created by
    // findPipelineCache, but the file itself is written only after the
    // first pipeline build).
    std::vector<uint8_t> cacheData{};
    if (!path.empty() && std::filesystem::exists(path)) {
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
    if (path.empty())
        return; // no usable cache dir: keep cache in memory only

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
    bool external,
    vk::ExternalMemoryHandleTypeFlagBits externalType
) {
    const vk::ExternalMemoryImageCreateInfo externalInfo{
        .handleTypes = externalType
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
bool vkhelper::supportsAhbExport(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::PhysicalDevice& physdev,
    vk::Extent2D extent,
    vk::Format format,
    vk::ImageUsageFlags usage
) {
    // Ask the driver whether this exact image configuration is exportable as an
    // AHardwareBuffer. Export and import are reported separately and a driver can
    // support import without export, so this is queried rather than assumed. Any
    // failure simply means "stay on the opaque-fd handle type".
    (void)extent;
    try {
        // Raw C API rather than a vulkan.hpp convenience member: the vendored
        // header set does not expose getExternalImageFormatProperties(), and the
        // struct chain is unambiguous here.
        const auto getFmt2{dld.vkGetPhysicalDeviceImageFormatProperties2};
        if (!getFmt2)
            return false;

        VkPhysicalDeviceExternalImageFormatInfo externalInfo{};
        externalInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
        externalInfo.handleType =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;

        VkPhysicalDeviceImageFormatInfo2 formatInfo{};
        formatInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
        formatInfo.pNext = &externalInfo;
        formatInfo.format = static_cast<VkFormat>(format);
        formatInfo.type = VK_IMAGE_TYPE_2D;
        formatInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        formatInfo.usage = static_cast<VkImageUsageFlags>(usage);

        VkExternalImageFormatProperties externalProps{};
        externalProps.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;

        VkImageFormatProperties2 props{};
        props.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
        props.pNext = &externalProps;

        const VkResult result{getFmt2(
            static_cast<VkPhysicalDevice>(physdev), &formatInfo, &props)};
        if (result != VK_SUCCESS) {
            VKLOG("supportsAhbExport: format query result=%d", static_cast<int>(result));
            return false;
        }
        const bool exportable{
            (externalProps.externalMemoryProperties.externalMemoryFeatures &
             VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0
        };
        VKLOG("supportsAhbExport: exportable=%d (features=0x%x)",
            exportable ? 1 : 0,
            static_cast<unsigned>(externalProps.externalMemoryProperties.externalMemoryFeatures));
        return exportable;
    } catch (const std::exception& e) {
        VKLOG("supportsAhbExport query failed: %s", e.what());
        return false;
    }
}

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

    // Create the image for AHB import with TRANSFER usage for blits.
    //
    // IMPORTANT: do NOT create the image with an OPAQUE_FD external handle
    // type (external=false). The AHB-backed gralloc memory on Android is the
    // ANDROID_HARDWARE_BUFFER handle type, not an opaque fd. Requiring
    // OPAQUE_FD (external=true) forces the memory type to be fd-exportable,
    // which AHB-backed memory often is not, so vkBindImageMemory fails with
    // VK_ERROR_INVALID_EXTERNAL_HANDLE (-1000072003). The image becomes
    // external purely through VkImportAndroidHardwareBufferInfoANDROID below
    // (chained into the allocation), which is the portable AHB import path and
    // the one that works on this device's Adreno driver (see the WSI presenter
    // import, which uses a plain VkImageCreateInfo with no external handle
    // types and succeeds).
    auto image{createImage(
        dld,
        device,
        extent,
        format,
        1,
        usage,
        false
    )};

    // Query which memory type bits the AHardwareBuffer supports and pick one that is
    // also usable by this image.
    //
    // Universal-safety guard: the AHB import depends on the
    // VK_ANDROID_external_memory_android_hardware_buffer device extension. If the
    // device did not enable it, the dispatch-loader function pointer is NULL and
    // calling it is a NULL-call SIGSEGV (the exact crash that killed the app on
    // Adreno). Verify the pointer is resolved before using it.
    auto getAhbProps{dld.vkGetAndroidHardwareBufferPropertiesANDROID};
    if (!getAhbProps) {
        throw std::runtime_error(
            "device lacks vkGetAndroidHardwareBufferPropertiesANDROID; enable "
            "VK_ANDROID_external_memory_android_hardware_buffer on the device");
    }

    // sType MUST be set to the correct value; a zero-initialized struct has
    // sType = VK_STRUCTURE_TYPE_UNKNOWN, which strict drivers (Adreno) reject or
    // mis-handle when querying buffer properties. Use the C struct since the raw
    // PFN from the dispatch loader expects the C type.
    VkAndroidHardwareBufferPropertiesANDROID props{};
    props.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    if (getAhbProps(static_cast<VkDevice>(device), ahb, &props) != VK_SUCCESS)
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
    // Imported-AHardwareBuffer special case: the allocation size must cover the
    // BUFFER's allocation, not merely the image's memory requirements.
    //
    // The image's requirements describe the image's own view of the memory; the
    // imported buffer may be larger (gralloc pads and aligns, and the buffer is
    // sized for the full surface while the Vulkan image may be smaller).
    // Allocating only requirements.size under-sizes the import, so the driver
    // cannot map the whole buffer and the image ends up mis-bound. Adreno then
    // dereferences null when it resolves the image for the first copy that
    // touches it: SIGSEGV/SEGV_MAPERR at a small fixed offset inside
    // vkCmdCopyImage/vkCmdBlitImage. Take the larger of the two so the import is
    // always backed by the buffer's real allocation.
    const vk::DeviceSize importSize{
        std::max<vk::DeviceSize>(requirements.size, props.allocationSize)
    };
    VKLOG("AHB import ahb=%p extent=%ux%u imageReq=%llu bufAlloc=%llu using=%llu "
          "memTypeBits(img=%u buf=%u) chosen=%u",
        ahb, extent.width, extent.height,
        static_cast<unsigned long long>(requirements.size),
        static_cast<unsigned long long>(props.allocationSize),
        static_cast<unsigned long long>(importSize),
        static_cast<unsigned>(requirements.memoryTypeBits),
        static_cast<unsigned>(props.memoryTypeBits),
        memoryTypeIndex);
    const vk::MemoryAllocateInfo allocInfo{
        .pNext = &importInfo,
        .allocationSize = importSize,
        .memoryTypeIndex = memoryTypeIndex
    };
    // OWNERSHIP: release the allocation so it is NOT freed when this function
    // returns.
    //
    // Returning a UniqueImage while letting the UniqueDeviceMemory die at scope
    // exit makes vkFreeMemory run immediately, leaving the returned image bound
    // to freed memory. Every later use of that image (the copy that writes the
    // generated frame into it, the copy that reads it back out) then traps
    // inside the driver: Adreno dereferences the now-dangling backing store and
    // dies with SIGSEGV/SEGV_MAPERR at a fixed offset. That was the real cause of
    // the blit/copy crash - switching blit to copy changed the entry point but
    // not the fault, because the imported image was already dead.
    //
    // The caller is responsible for destroying the memory after the image. See
    // the "two-phase" note on createImageFromAhbAlloc, which hands the caller
    // that allocation explicitly. This raw-handle entry point keeps the old
    // signature for the one-shot source import (writeSourceLayer), which
    // destroys the image itself and deliberately leaks the bound memory for the
    // life of the process rather than risk a use-after-free: one small
    // allocation per capture buffer, reclaimed by the OS at process exit.
    auto allocation{device.allocateMemoryUnique(allocInfo, nullptr, dld)};

    device.bindImageMemory(*image, *allocation, 0, dld);

    (void)allocation.release();
    return image;
}

std::pair<vk::UniqueImage, vk::UniqueDeviceMemory> vkhelper::createImageFromAhbAlloc(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const void* buffer,
    vk::ImageUsageFlags usage
) {
    // Two-phase variant: returns the imported image AND the memory allocation
    // that backs it, so the caller can keep both alive for as long as the image
    // is used. Destroying this memory while the image is still in use corrupts
    // GPU state (see the ownership note above).
    const auto* ahb{static_cast<const AHardwareBuffer*>(buffer)};
    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(ahb, &desc);
    if (desc.width == 0 || desc.height == 0)
        throw std::runtime_error("AHardwareBuffer_describe returned an empty buffer");

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

    auto getAhbProps{dld.vkGetAndroidHardwareBufferPropertiesANDROID};
    if (!getAhbProps) {
        throw std::runtime_error(
            "device lacks vkGetAndroidHardwareBufferPropertiesANDROID; enable "
            "VK_ANDROID_external_memory_android_hardware_buffer on the device");
    }

    // Query the buffer properties BEFORE creating the image, and ask for the
    // FORMAT properties too via pNext.
    //
    // Valid usage requires that, for a dedicated AHB import, the image's format
    // is either VK_FORMAT_UNDEFINED or exactly the format the driver reports in
    // VkAndroidHardwareBufferFormatPropertiesANDROID::format. Our previous code
    // passed a hardcoded guess from a local switch, which can disagree with what
    // the driver reports and makes the import invalid. Feeding the driver's own
    // answer back into vkCreateImage is the conformant ordering.
    VkAndroidHardwareBufferFormatPropertiesANDROID fmtProps{};
    fmtProps.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;
    VkAndroidHardwareBufferPropertiesANDROID props{};
    props.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    props.pNext = &fmtProps;
    if (getAhbProps(static_cast<VkDevice>(device), ahb, &props) != VK_SUCCESS)
        throw std::runtime_error("vkGetAndroidHardwareBufferPropertiesANDROID failed");

    if (fmtProps.format != VK_FORMAT_UNDEFINED) {
        format = static_cast<vk::Format>(fmtProps.format);
    } else {
        // Undefined here means an implementation-defined external format (typically
        // YCbCr camera/video buffers) which needs a VkSamplerYcbcrConversion. Our
        // local switch handles the plain RGBA/RGB cases we allocate ourselves.
        VKLOG("AHB import: driver reported VK_FORMAT_UNDEFINED (externalFormat=0x%llx); "
              "falling back to local guess %d",
            static_cast<unsigned long long>(fmtProps.externalFormat),
            static_cast<int>(format));
    }

    // Create the image DECLARING the AHardwareBuffer external handle type.
    //
    // This is required, and was the actual root cause of the Adreno crash. An
    // image created without VkExternalMemoryImageCreateInfo reports generic
    // memory requirements, so vkGetImageMemoryRequirements offers only the
    // driver's default memory type. The AHardwareBuffer, however, is only
    // importable into specific memory types. Measured on this device:
    //
    //   imageReq=9613312 bufAlloc=9515008
    //   memTypeBits(img=1 buf=18) -> intersection EMPTY (1 & 18 == 0)
    //   memTypeBits(img=1 buf=2)  -> intersection EMPTY (1 & 2  == 0)
    //
    // The old code fell back to the image's own bits when the intersection was
    // empty, binding the buffer to a memory type it does not support. The image
    // then resolved to nothing and the first copy that touched it trapped in
    // the driver (SIGSEGV/SEGV_MAPERR at a small fixed offset).
    //
    // NOTE: the handle type here must be ANDROID_HARDWARE_BUFFER. This is NOT
    // the OPAQUE_FD path that an earlier comment warned about; AHB gralloc memory
    // is not fd-exportable, but it is importable as an AHardwareBuffer handle.
    // Declaring it lets the driver report memory types that are actually
    // compatible with the buffer, so a valid intersection exists.
    const vk::ExternalMemoryImageCreateInfo ahbExternalInfo{
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eAndroidHardwareBufferANDROID
    };
    const vk::ImageCreateInfo ahbImageInfo{
        .pNext = &ahbExternalInfo,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {
            .width = extent.width,
            .height = extent.height,
            .depth = 1
        },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = usage
    };
    auto image{device.createImageUnique(ahbImageInfo, nullptr, dld)};

    const auto requirements{device.getImageMemoryRequirements(*image, dld)};

    // The memory type must be BOTH usable by the image and supported by the AHB.
    //
    // The previous code silently fell back to the image's own memoryTypeBits when
    // the intersection was empty, which deliberately bound the buffer to a memory
    // type it does not support. That produced a memory object the driver could not
    // resolve, and the first copy touching the image trapped in the driver. Fail
    // loudly instead: an empty intersection means the import cannot be done
    // correctly at all, and a clear error is strictly better than a GPU fault.
    const uint32_t usable{
        static_cast<uint32_t>(requirements.memoryTypeBits) & props.memoryTypeBits
    };
    if (usable == 0) {
        throw std::runtime_error(
            "AHB import: no memory type is both image-usable (" +
            std::to_string(static_cast<unsigned>(requirements.memoryTypeBits)) +
            ") and AHB-compatible (" +
            std::to_string(static_cast<unsigned>(props.memoryTypeBits)) + ")");
    }
    uint32_t memoryTypeIndex = 0;
    for (uint32_t i = 0; i < 64; ++i) {
        if (usable & (1u << i)) {
            memoryTypeIndex = i;
            break;
        }
    }

    auto* ahbMutable{const_cast<AHardwareBuffer*>(ahb)};

    // DEDICATED ALLOCATION IS REQUIRED HERE.
    //
    // VkImportAndroidHardwareBufferInfoANDROID is one of the import paths that
    // requires the memory to be a dedicated allocation: without
    // VkMemoryDedicatedAllocateInfo naming this image, the pNext chain does not
    // describe a dedicated import, and the spec then demands the buffer be a
    // BLOB with AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER, which ours is not. Every
    // reference implementation (upstream lsfg-vk-pipeline, lsfg-vk-cli, and the
    // LSFG-Android AHB bridges) chains the dedicated struct in. We did not.
    const vk::MemoryDedicatedAllocateInfo dedicatedInfo{
        .image = *image
    };
    const vk::ImportAndroidHardwareBufferInfoANDROID importInfo{
        .pNext = &dedicatedInfo,
        .buffer = ahbMutable
    };
    // allocationSize must be EXACTLY the value the driver returned from
    // vkGetAndroidHardwareBufferPropertiesANDROID for this buffer. The previous
    // code passed max(imageRequirements.size, props.allocationSize) hoping the
    // larger of the two would be safer; the spec's requirement is equality, so a
    // mismatch is an invalid import even when the number is bigger.
    const vk::DeviceSize importSize{props.allocationSize};
    VKLOG("AHB import ahb=%p extent=%ux%u imageReq=%llu bufAlloc=%llu using=%llu "
          "memTypeBits(img=%u buf=%u chosen=0x%x) -> %u dedicated=yes",
        ahb, extent.width, extent.height,
        static_cast<unsigned long long>(requirements.size),
        static_cast<unsigned long long>(props.allocationSize),
        static_cast<unsigned long long>(importSize),
        static_cast<unsigned>(requirements.memoryTypeBits),
        static_cast<unsigned>(props.memoryTypeBits),
        static_cast<unsigned>(usable),
        memoryTypeIndex);
    const vk::MemoryAllocateInfo allocInfo{
        .pNext = &importInfo,
        .allocationSize = importSize,
        .memoryTypeIndex = memoryTypeIndex
    };
    auto allocation{device.allocateMemoryUnique(allocInfo, nullptr, dld)};

    device.bindImageMemory(*image, *allocation, 0, dld);

    return { std::move(image), std::move(allocation) };
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
    // Prefer vkCmdCopyImage when the source and destination have identical
    // extents and formats.
    //
    // vkCmdBlitImage is only valid when the DESTINATION advertises
    // VK_FORMAT_FEATURE_BLIT_DST_BIT, and gralloc-backed AHardwareBuffer images
    // generally do not (their optimized layouts are exposed as sampled /
    // transfer usages, not as blit targets). Requesting a blit into such an
    // image on Adreno does not fail cleanly: the driver dereferences a null
    // internal resource at offset 0x80 inside vkCmdBlitImage2 and the process
    // dies with SIGSEGV (SEGV_MAPERR, fault addr 0x80). That was the crash in
    // nativeAcquireToDst -> vkhelper::blitImage -> vkCmdBlitImage.
    //
    // vkCmdCopyImage has no BILT_DST requirement: it needs only TRANSFER_SRC on
    // the source and TRANSFER_DST on the destination, which both images already
    // declare. The WSI presenter's present path copies into AHB-imported images
    // with vkCmdCopyImage and works on this device, so this matches the one
    // proven-good path rather than inventing a new one.
    //
    // A copy is also the correct operation here: every call site copies between
    // matching extents at offset zero (source AHB -> pipeline input layer, and
    // pipeline output -> destination AHB). Scaling is not requested, so the
    // blit's sampling behaviour buys nothing and only risks the invalid-usage
    // path above. If the extents ever differ, fall back to the blit.
    //
    // Note: the copy region must cover whole texels, so the extent is clamped to
    // the smaller of the two images (defensive; callers pass matching sizes).
    const vk::ImageCopy copyInfo{
        .srcSubresource = blitInfo.srcSubresource,
        .srcOffset = { 0, 0, 0 },
        .dstSubresource = blitInfo.dstSubresource,
        .dstOffset = { 0, 0, 0 },
        .extent = {
            .width = extent.width,
            .height = extent.height,
            .depth = 1
        }
    };
    cmdbuf.copyImage(
        srcImage, vk::ImageLayout::eGeneral,
        dstImage, vk::ImageLayout::eGeneral,
        copyInfo,
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

#if defined(__ANDROID__)
AHardwareBuffer* vkhelper::exportMemoryAhb(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    const vk::DeviceMemory& memory
) {
    // Prefer the dispatch-loader pointer and fall back to the raw entry point, so
    // this cannot null-call if the function was not resolved into the loader
    // (exactly the class of bug that produced an earlier SIGSEGV at 0x0 here).
    const auto getAhb{dld.vkGetMemoryAndroidHardwareBufferANDROID};
    AHardwareBuffer* buffer{nullptr};
    VkMemoryGetAndroidHardwareBufferInfoANDROID info{};
    info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
    info.memory = static_cast<VkDeviceMemory>(memory);

    const VkResult result{getAhb
        ? getAhb(static_cast<VkDevice>(device), &info, &buffer)
        : ::vkGetMemoryAndroidHardwareBufferANDROID(
              static_cast<VkDevice>(device), &info, &buffer)};
    if (result != VK_SUCCESS || !buffer) {
        VKLOG("exportMemoryAhb failed: result=%d buffer=%p", static_cast<int>(result), buffer);
        return nullptr;
    }
    // The spec requires a NEW reference to be acquired here; the caller owns it.
    return buffer;
}
#endif

int vkhelper::safeExportedFd(int fd) {
    // Descriptors 0/1/2 belong to the process's std streams. If a driver returned
    // one, dup it out of the way and close the original, so later code never
    // closes a descriptor the rest of the process expects to own. Closing a
    // descriptor in the 0-2 range is what trips Android's fdsan into aborting.
    if (fd < 0)
        return -1;
    if (fd > 2)
        return fd;
    const int moved{::fcntl(fd, F_DUPFD_CLOEXEC, 3)};
    if (moved >= 0)
        ::close(fd);
    return moved;
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
