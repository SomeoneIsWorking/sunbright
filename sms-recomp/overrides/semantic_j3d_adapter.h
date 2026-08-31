#pragma once

#include <intrinsics.h>

#include <cstdint>
#include <span>
#include <string>

struct GuestJ3dMatrixBinding {
    u32 matrixObject = 0;
    std::uint16_t sourceSlot = 0;
    std::uint16_t matrixIndex = 0;
};

void submit_semantic_j3d_shape(u32 shape, std::span<const GuestJ3dMatrixBinding> matrices);
[[nodiscard]] std::string semantic_j3d_stats_text();
