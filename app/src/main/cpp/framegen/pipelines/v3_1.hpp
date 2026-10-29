/* SPDX-License-Identifier: CC-BY-NC-ND-4.0 */

#pragma once

#include "../signature.hpp"

namespace lsfgvk::v3_1 {

    ///
    /// Get the pipeline signature
    ///
    /// @param perf Performance mode
    /// @return Pipeline signature
    ///
    const PipelineSignature& getPipelineSignature(bool perf);

}
