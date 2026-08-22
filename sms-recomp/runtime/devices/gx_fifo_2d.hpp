#pragma once

#include <cstdint>

#include "gx_fifo_vertex_layout.hpp"
#include "native_render.h"

struct GxFifo2DDrawContext {
    std::uint8_t* ram;
    const std::uint32_t* arrayBase;
    const std::uint32_t* arrayStride;
    const float* positionMatrices;
    const bool* positionMatrixRowsSet;
    const float* projection;
    const SbrTexture* textures;
    const SbrTevState* tev;
    const SbrXfState* xf;
    const SbrDepthState* depth;
    std::uint32_t streamPosition;
    std::uint32_t unit0Image3;
    long drawsSinceCapture;
    bool projectionIsOrthographic;
};

void gxFifo2DHandleDraw(const GxFifo2DDrawContext& context, std::uint32_t opcode,
                        const std::uint8_t* vertices, std::uint32_t vertexCount,
                        const GxFifoVat& vat);
void sbr_gxfifo_report_2d_gate();
