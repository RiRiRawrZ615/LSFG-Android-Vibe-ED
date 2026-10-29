/* SPDX-License-Identifier: CC-BY-NC-ND-4.0 */

#include "library.hpp"
#include "library/dll.hpp"
#include "lsfgvk.hpp"

#include "vkhelper.hpp"

#include "Mipmaps.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <vk>

/// All base shaders in the library.
const std::array<std::pair<std::string_view, uint32_t>, 2> BASE_LIBRARY{{
    { "mipmaps", 0 },
    { "generate", 1 }
}};

/// All non-base shaders in the library.
const std::array<std::pair<std::string_view, uint32_t>, 24> LIBRARY{{
    { "alpha0", 13 },
    { "alpha1", 14 },
    { "alpha2", 15 },
    { "alpha3", 16 },
    { "beta0", 22 },
    { "beta1", 23 },
    { "beta2", 24 },
    { "beta3", 25 },
    { "beta4", 26 },
    { "gamma0", 3 },
    { "gamma1", 4 },
    { "gamma2", 5 },
    { "gamma3", 6 },
    { "gamma4", 7 },
    { "delta0", 8 },
    { "delta1", 9 },
    { "delta2", 10 },
    { "delta3", 11 },
    { "delta4", 12 },
    { "epsilon0", 17 },
    { "epsilon1", 18 },
    { "epsilon2", 19 },
    { "epsilon3", 20 },
    { "epsilon4", 21 }
}};

using namespace lsfgvk;

ShaderLibrary::ShaderLibrary(
    const vk::detail::DispatchLoaderDynamic& dld,
    const vk::Device& device,
    bool halfPrecision,
    const std::filesystem::path& dll,
    std::function<void(const std::string&)>& logCallback
) {
    LOG() << "Loading shader library from " << dll.string();

    if (!std::filesystem::exists(dll)) {
        throw std::runtime_error("The specified shader DLL does not exist");
    }
    // Create shader modules for each shader in the library
    const auto resources = lsfgvk::parseDll(dll);
    for (const auto& [name, idx] : BASE_LIBRARY) {
        const uint32_t rid{idx};

        const auto& it = resources.find(rid == 0 ? 2147488584U : rid);
        if (it == resources.end())
            throw std::runtime_error(
                "Unable to find base shader '" + std::string(name) + "' in DLL"
            );

        LOG() << "  " << std::setw(2) << idx
            << ": name=" << name
            << ", rid=" << rid
            << ", size=" << (it->second.size() * 4) << " bytes";

        // Patch the generate shader to create two additional variants
        if (name == "generate") {
            std::vector<uint32_t> bytecode{it->second};

            bytecode.insert(bytecode.begin() + 8, { 0x00000031, 0x00020011 });
            for (size_t i = 5; i < bytecode.size(); ) {
                const uint32_t word{bytecode.at(i)};
                if ((word & 0xFFFF) == 25 && bytecode.at(i + 8) != 0) {
                    bytecode.at(i + 8) = 0x04;

                    this->m_baseShaders["generate0"] =
                        vkhelper::createShaderModule(dld, device, bytecode);

                    bytecode.at(i + 8) = 0x0B;

                    this->m_baseShaders["generate1"] =
                        vkhelper::createShaderModule(dld, device, bytecode);

                    bytecode.at(i + 8) = 0x02;

                    this->m_baseShaders["generate2"] =
                        vkhelper::createShaderModule(dld, device, bytecode);
                    break;
                }

                i += std::max((word >> 16) & 0xFFFF, 1U);
            }

            continue;
        }

        this->m_baseShaders[name] = vkhelper::createShaderModule(dld, device, it->second);
    }

    for (const auto& [name, idx] : LIBRARY) {
        const std::pair<uint32_t, uint32_t> rid{
            idx + (halfPrecision ? 48 : 0),
            idx + (halfPrecision ? 48 : 0) + 24
        };

        const auto& qit{resources.find(rid.first)};
        const auto& pit{resources.find(rid.second)};
        if (qit == resources.end() || pit == resources.end())
            throw std::runtime_error(
                "Unable to find shader '" + std::string(name) + "' in DLL"
            );

        LOG() << "  " << std::setw(2) << idx
            << ": name=" << std::setw(8) << name
            << ", [Q] "
                << "rid="<< std::setw(2) << rid.first
                << ", size="<< std::setw(5) << (qit->second.size() * 4) << " bytes"
            << ", [P] "
                << "rid="<< std::setw(2) << rid.second
                << ", size="<< std::setw(5) << (pit->second.size() * 4) << " bytes";

        this->m_qualityShaders[name] = vkhelper::createShaderModule(dld, device, qit->second);
        this->m_performanceShaders[name] = vkhelper::createShaderModule(dld, device, pit->second);
    }

    // Replace Mipmaps shader with HDR version if available
    std::vector<uint32_t> shaderData(Mipmaps_len / 4);
    std::copy_n(
        reinterpret_cast<const uint32_t*>(Mipmaps), // NOLINT (unsafe cast)
        Mipmaps_len / 4,
        shaderData.data()
    );
    this->m_baseShaders["mipmaps"] = vkhelper::createShaderModule(dld, device, shaderData);

    LOG() << "Shader library loaded successfully";
}
