/* SPDX-License-Identifier: CC-BY-NC-ND-4.0 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace lsfgvk {

    ///
    /// Parse all resources from a DLL file
    ///
    /// @returns Map of resource ID to data
    /// @throws std::runtime_error if the file is invalid or cannot be read
    ///
    std::unordered_map<uint32_t, std::vector<uint32_t>> parseDll(
        const std::filesystem::path& dll
    );

}
