#include "gx_fifo_vertex_layout.hpp"

namespace {

std::uint32_t componentBytes(std::uint32_t format) {
    switch (format) {
    case 0:
    case 1:
        return 1;
    case 2:
    case 3:
        return 2;
    case 4:
        return 4;
    default:
        return 0;
    }
}

std::uint32_t attributeBytes(std::uint32_t mode, std::uint32_t directBytes) {
    switch (mode) {
    case 0:
        return 0;
    case 1:
        return directBytes;
    case 2:
        return 1;
    case 3:
        return 2;
    default:
        return 0;
    }
}

std::uint32_t colourBytes(std::uint32_t format) {
    constexpr std::uint8_t bytes[] = {2, 3, 4, 2, 3, 4};
    return format < sizeof(bytes) ? bytes[format] : 0;
}

std::uint32_t texcoordBytes(const GxFifoVat& vat, unsigned index) {
    std::uint32_t elements = 0;
    std::uint32_t format = 0;
    if (index == 0) {
        elements = (vat.fmt0 >> 21) & 1;
        format = (vat.fmt0 >> 22) & 7;
    } else if (index <= 4) {
        const std::uint32_t shift = (index - 1) * 9;
        elements = (vat.fmt1 >> shift) & 1;
        format = (vat.fmt1 >> (shift + 1)) & 7;
    } else {
        // VAT_C begins with TEX4's five-bit fractional shift, so TEX5 starts at bit 5.
        const std::uint32_t shift = 5 + (index - 5) * 9;
        elements = (vat.fmt2 >> shift) & 1;
        format = (vat.fmt2 >> (shift + 1)) & 7;
    }
    return (elements != 0 ? 2u : 1u) * componentBytes(format);
}

} // namespace

std::uint32_t gxFifoVertexSize(const GxFifoVat& vat) {
    std::uint32_t bytes = (vat.vcd_lo & 1) != 0 ? 1 : 0;
    for (unsigned texMatrix = 0; texMatrix < 8; ++texMatrix) {
        bytes += (vat.vcd_lo & (1u << (1 + texMatrix))) != 0 ? 1 : 0;
    }

    const std::uint32_t positionMode = (vat.vcd_lo >> 9) & 3;
    const std::uint32_t normalMode = (vat.vcd_lo >> 11) & 3;
    const std::uint32_t colour0Mode = (vat.vcd_lo >> 13) & 3;
    const std::uint32_t colour1Mode = (vat.vcd_lo >> 15) & 3;
    const std::uint32_t positionComponents = (vat.fmt0 & 1) != 0 ? 3 : 2;
    bytes += attributeBytes(positionMode, positionComponents * componentBytes((vat.fmt0 >> 1) & 7));

    const bool nbt3 = ((vat.fmt0 >> 31) & 1) != 0;
    const bool nbt = nbt3 || ((vat.fmt0 >> 9) & 1) != 0;
    const std::uint32_t normalComponents = nbt ? 9 : 3;
    if (normalMode == 1) {
        bytes += normalComponents * componentBytes((vat.fmt0 >> 10) & 7);
    } else if (normalMode == 2) {
        bytes += nbt3 ? 3 : 1;
    } else if (normalMode == 3) {
        bytes += nbt3 ? 6 : 2;
    }

    bytes += attributeBytes(colour0Mode, colourBytes((vat.fmt0 >> 14) & 7));
    bytes += attributeBytes(colour1Mode, colourBytes((vat.fmt0 >> 18) & 7));
    for (unsigned texcoord = 0; texcoord < 8; ++texcoord) {
        bytes += attributeBytes((vat.vcd_hi >> (texcoord * 2)) & 3, texcoordBytes(vat, texcoord));
    }
    return bytes;
}
