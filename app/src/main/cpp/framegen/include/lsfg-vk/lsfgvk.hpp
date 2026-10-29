/* SPDX-License-Identifier: CC-BY-NC-ND-4.0 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#ifdef LSFGVK_EXPOSE_VULKAN
#include <vulkan/vulkan.h>
#endif // LSFGVK_EXPOSE_VULKAN

namespace lsfgvk {

    /// Forward declaration of implementation classes
    namespace impl {
        struct [[gnu::visibility("default")]] Instance;
        struct [[gnu::visibility("default")]] Context;
    }

    ///
    /// Main entrypoint of the library
    ///
    class [[gnu::visibility("default")]] Instance {
        friend class Context;
    public:
        ///
        /// Create a lsfg-vk instance
        ///
        /// The device identifier may be one of:
        /// - Device name (e.g. "NVIDIA GeForce RTX 5080")
        /// - Vendor ID + Device ID in lowercase hexadecimal (e.g. "10de:2c02")
        /// - PCI bus ID with padded zeroes (e.g. "0000:01:00.0")
        ///
        /// @param deviceId Device identifier (see above)
        /// @param lsfgvkDllPath Path to the lsfg-vk DLL file
        /// @param allowFP16 Whether to allow usage of fp16 shader variants
        /// @param leakInstance Whether to leak the Vulkan device & instance on destruction
        /// @param logCallback Optional callback for logging messages
        /// @throws std::runtime_error on failure
        ///
        Instance(
            const std::string& deviceId,
            const std::filesystem::path& lsfgvkDllPath,
            bool allowFP16,
            bool leakInstance = false,
            std::function<void(const std::string&)> logCallback = nullptr
        );

#ifdef LSFGVK_EXPOSE_VULKAN
        /// Get the underlying Vulkan instance handle
        /// @return Vulkan instance
        [[nodiscard]] VkInstance _instance() const;

        /// Get the underlying Vulkan device handle
        /// @return Vulkan device
        [[nodiscard]] VkDevice _device() const;
#endif // LSFGVK_EXPOSE_VULKAN

        // Non-copyable, non-movable
        Instance(const Instance&) = delete;
        Instance& operator=(const Instance&) = delete;
        Instance(Instance&&) = delete;
        Instance& operator=(Instance&&) = delete;
        ~Instance();
    private:
        std::unique_ptr<impl::Instance> m_impl;
    };

    ///
    /// File descriptors exported from a context, the user must close them after use.
    ///
    struct [[gnu::visibility("default")]] FileDescriptors {
        ///
        /// File descriptor for a Vulkan memory allocation containing
        /// a 2D array of RGBA8 / A2B10G10R10 / RGBA16 pixels with length 2 and optimal allocation.
        ///
        /// Starting at iteration 0, the next frame for which frames should be interpolated
        /// inbetween should be placed in image `iteration % 2`.
        ///
        int sourceFd;

        ///
        /// File descriptor for a Vulkan memory allocation containing a single
        /// RGBA8 / A2B10G10R10 / RGBA16 image into which each generated frame will be
        /// written to.
        ///
        int destinationFd;

        ///
        /// File descriptor for a timeline semaphore. When scheduling frames for generation,
        /// a specific value is waited for and signaled on return. It is up to the user to ensure
        /// the destination image is not overwritten before it is read.
        ///
        int syncFd;
    };


    ///
    /// Supported bit depths
    ///
    enum class [[gnu::visibility("default")]] BitDepth : uint8_t {
        /// 8-bit per cannel with R8G8B8A8 format.
        R8G8B8A8 = 0,
        /// 10-bit per channel with A2B10G10R10 format.
        A2B10G10R10 = 1,
        /// 16-bit per channel with R16G16B16A16 format.
        R16G16B16A16 = 2
    };

    ///
    /// Supported color spaces.
    ///
    enum class [[gnu::visibility("default")]] ColorSpace : uint8_t {
        /// The sRGB color space. Also use for Bt. 709, Display P3, AdobeRGB.
        SRGB = 0,
        /// The HDR10 / Bt. 2020 color space.
        HDR10 = 1,
        /// The extended sRGB color space.
        SCRGB = 2,
        /// The DCI-P3 color space (with theatrical white point).
        DCIP3 = 3
    };

    ///
    /// Supported transfer functions.
    ///
    enum class [[gnu::visibility("default")]] TransferFunction : uint8_t {
        /// sRGB / Gamma 2.2 transfer function.
        SRGB = 0,
        /// Linear transfer function.
        LINEAR = 1,
        /// Perceptual Quantizer (PQ) transfer function (from HDR10 ST2084).
        HDR10_PQ = 2,
        /// Hybrid Log-Gamma (HLG) transfer function.
        HDR10_HLG = 3,
        /// scRGBnl transfer function.
        SCRGBNL = 4,
        /// DCI-P3 / Gamma 2.6 transfer function.
        DCIP3 = 5
    };

    ///
    /// A context for generating frames
    ///
    class [[gnu::visibility("default")]] Context {
    public:
        ///
        /// Create a frame generation context
        ///
        /// @param instance Parent instance
        /// @param width Image width
        /// @param height Image height
        /// @param flowScale Flow estimation scale factor
        /// @param performanceMode Whether to enable performance mode
        /// @param depth Which bit-depth the input and output images should have
        /// @param colorSpace Color space of the input and output images
        /// @param transferFunction Transfer function of the input and output images
        /// @throws std::runtime_error on failure
        ///
        Context(
            const Instance& instance,
            uint32_t width,
            uint32_t height,
            float flowScale,
            bool performanceMode,
            BitDepth depth = BitDepth::R8G8B8A8,
            ColorSpace colorSpace = ColorSpace::SRGB,
            TransferFunction transferFunction = TransferFunction::SRGB
        );

        ///
        /// Export the internal resources
        ///
        /// @return File descriptors for internal resources
        /// @throws std::runtime_error on failure
        ///
        [[nodiscard]] FileDescriptors exportFds() const;

        ///
        /// Dispatch frame generation
        ///
        /// Let `so - 1` be the current value of the timeline semaphore, starting at 0.
        /// The user must signal `so` to start the generation of the next frame, after
        /// which lsfg-vk will signal `so + 1`. The user must ensure the previously
        /// generated frame is read before signaling the next one (at `so + 2` and so on).
        ///
        /// This method starts the pre-pass, for each generated image the main-pass must be
        /// scheduled with acquire(). It is recommended to call acquire() only after the
        /// signal operation has been recorded, else the context may stall the
        /// entire lsfg-vk Instance.
        ///
        /// @param total Total number of frames to generate
        /// @param unsignaled Do not wait for any timeline semaphores
        /// @throws std::runtime_error on failure
        ///
        void dispatch(uint32_t total, bool unsignaled = false);

        ///
        /// Acquire a generated image
        ///
        /// @throws std::runtime_error on failure
        ///
        void acquire(bool unsignaled = false);

        ///
        /// Wait for the dispatch to complete. This should only be
        /// called after the last acquire() and will block until the
        /// last frame is generated.
        ///
        /// @throws std::runtime_error on failure
        ///
        void idle();

        // Non-copyable, non-movable
        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;
        Context(Context&&) = delete;
        Context& operator=(Context&&) = delete;
        ~Context(); // This will call `idle()`.
    private:
        std::unique_ptr<impl::Context> m_impl;
    };

}
