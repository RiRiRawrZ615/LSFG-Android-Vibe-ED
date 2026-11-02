/* SPDX-License-Identifier: CC-BY-NC-ND-4.0 */

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vk>

namespace lsfgvk {

    ///
    /// The lsfg-vk shader library
    ///
    class ShaderLibrary {
    public:
        ///
        /// Create the shader library
        ///
        /// @param halfPrecision Whether to load the half-precision shader variants
        /// @param dll Path to the shader DLL file
        /// @throws std::runtime_error on failure
        ///
        explicit ShaderLibrary(
            const vk::detail::DispatchLoaderDynamic& dld,
            const vk::Device& device,
            bool halfPrecision,
            const std::filesystem::path& dll,
            std::function<void(const std::string&)>& logCallback
        );

        ///
        /// Get a base shader by name
        ///
        /// @return A reference to the shader
        /// @throws std::out_of_range if the shader is not found
        ///
        [[nodiscard]] const auto& baseShader(std::string_view name) const {
            return this->m_baseShaders.at(name);
        }

        ///
        /// Get a shader by name
        ///
        /// @param perf Whether to get the performance variant of the shader
        /// @return A reference to the shader
        /// @throws std::out_of_range if the shader is not found
        ///
        [[nodiscard]] const auto& shader(std::string_view name, bool perf) const {
            const auto it{this->m_baseShaders.find(name)};
            if (it != this->m_baseShaders.end())
                return it->second;

            return perf ? this->m_performanceShaders.at(name) : this->m_qualityShaders.at(name);
        }

        ///
        /// Destroy all shader modules. Must be called before the Vulkan device
        /// they belong to is destroyed (the implicit destructor would otherwise
        /// run after the device is gone). Safe to call twice.
        ///
        void destroy() {
            this->m_baseShaders.clear();
            this->m_qualityShaders.clear();
            this->m_performanceShaders.clear();
        }

    private:
        std::unordered_map<std::string_view, vk::UniqueShaderModule> m_baseShaders;
        std::unordered_map<std::string_view, vk::UniqueShaderModule> m_qualityShaders;
        std::unordered_map<std::string_view, vk::UniqueShaderModule> m_performanceShaders;
    };

}
