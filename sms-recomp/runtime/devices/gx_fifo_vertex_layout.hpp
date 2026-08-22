#pragma once

#include <cstdint>

struct GxFifoVat {
    std::uint32_t vcd_lo = 0;
    std::uint32_t vcd_hi = 0;
    std::uint32_t fmt0 = 0;
    std::uint32_t fmt1 = 0;
    std::uint32_t fmt2 = 0;
};

[[nodiscard]] std::uint32_t gxFifoVertexSize(const GxFifoVat& vat);
