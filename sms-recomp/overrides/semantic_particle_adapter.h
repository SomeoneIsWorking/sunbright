#pragma once

#include <cstdint>

namespace sb::recomp {

// Publishes one exact standard JPA billboard from guest memory. The recompiled draw body remains
// callable by its override wrapper, so this is an additive semantic path while Aurora remains the
// content oracle.
[[nodiscard]] bool submit_guest_particle_billboard(std::uint32_t drawContext,
                                                   std::uint32_t particle,
                                                   std::uint32_t smallDataBase) noexcept;

void report_semantic_particle_stats() noexcept;

} // namespace sb::recomp
