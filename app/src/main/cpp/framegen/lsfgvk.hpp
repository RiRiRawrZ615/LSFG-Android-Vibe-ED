/* SPDX-License-Identifier: CC-BY-NC-ND-4.0 */

#pragma once

#ifndef LSFGVK_EXPOSE_VULKAN
#define LSFGVK_EXPOSE_VULKAN 1
#endif
#include "lsfgvk_public.hpp" // Public API: Instance, Context, ColorSpace, TransferFunction, BitDepth

#include "pipeline.hpp"
#include "library.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <ostream>
#include <sstream>
#include <utility>
#include <vk>

namespace lsfgvk::impl {

    /// Internal state of lsfg-vk
    struct Instance {
        /// Vulkan context
        struct Vulkan {
            /// Vulkan dispatch loader
            std::unique_ptr<vk::detail::DispatchLoaderDynamic> dld;
            /// Vulkan instance (1.2)
            vk::UniqueInstance instance;
            /// Vulkan physical device
            vk::PhysicalDevice physdev;
            /// Vulkan device with synchronization2 (extension), external memory & semaphore
            /// fd (extension) and timeline semaphores (core) enabled
            vk::UniqueDevice device;
            /// Compute queue
            vk::Queue queue;
            /// Compute queue family index
            uint32_t qfi;
            /// Whether fp16 is enabled and supported (shaderFloat16 is enabled)
            bool fp16;
            /// Whether this instance should be leaked
            bool leakInstance;
        } vk;
        /// Shader library
        lsfgvk::ShaderLibrary shaderLibrary;
        /// Log callback
        std::function<void(const std::string&)> logCallback;
    };

    /// Internal context for frame generation
    struct Context {
        /// Parent instance
        std::reference_wrapper<Instance> instance;
        /// Pipeline instance
        lsfgvk::Pipeline pipeline;
        /// Shared synchronization semaphores
        std::pair<vk::UniqueSemaphore, uint64_t> syncSemaphore;
        /// Internal synchronization semaphores
        std::pair<vk::UniqueSemaphore, uint64_t> internalSemaphores;
        /// Frames-in-flight fence
        std::pair<vk::UniqueFence, bool> fence;
        /// Is first iteration
        bool firstIteration{true};
        /// Currently dispatched iteration
        uint32_t iteration{0};
        /// Current dispatch total image count
        uint32_t total{0};
        /// Current dispatch image index
        uint32_t index{0};
    };

    /// Stream-like interface for logging
    class LogStream {
    public:
        /// Construct a log stream
        LogStream(std::function<void(const std::string&)>& log)
            : m_log(std::ref(log)) {}

        // Stream operator for various types
        template<typename T>
        LogStream& operator<<(const T& value) {
            this->m_stream << value; // NOLINT (treats const char* as array, not string)
            return *this;
        }

        // Stream operator for manipulators
        LogStream& operator<<(std::ostream& (*manip)(std::ostream&)) {
            this->m_stream << manip;
            return *this;
        }

        // Non-copyable, moveable
        LogStream(const LogStream&) = delete;
        LogStream& operator=(const LogStream&) = delete;
        LogStream(LogStream&&) noexcept = default;
        LogStream& operator=(LogStream&&) noexcept = default;
        ~LogStream() { this->m_log.get()(this->m_stream.str()); }
    private:
        std::reference_wrapper<std::function<void(const std::string&)>> m_log;
        std::ostringstream m_stream;
    };

#define LOG() \
    if (logCallback) \
        impl::LogStream(logCallback)

}
