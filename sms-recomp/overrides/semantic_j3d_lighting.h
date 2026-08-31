#pragma once

#include <cstdint>

namespace sb::recomp {

struct SemanticJ3dLightingStats {
    std::uint64_t attempts = 0;
    std::uint64_t published = 0;
    std::uint64_t viewFailures = 0;
    std::uint64_t primaryPositionFailures = 0;
    std::uint64_t managerFailures = 0;
    std::uint64_t effectFailures = 0;
};

[[nodiscard]] SemanticJ3dLightingStats semantic_j3d_lighting_stats() noexcept;

} // namespace sb::recomp
