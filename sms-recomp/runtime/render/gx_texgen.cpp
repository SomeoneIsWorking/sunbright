#include "gx_texgen.h"

#include <lucent/log.h>

#include <algorithm>

void sbr_texgen(const SbrXfState& xf, unsigned generator, const SbrGeomVert& vertex,
                const float rasterColor[4], float out[2]) {
    const SbrTexGen& texgen = xf.texGen[generator];

    if (texgen.type == 2 || texgen.type == 3) {
        if (texgen.type == 3) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                lucent::error("nrender",
                              "texgen {} sources COLOR1, but only colour channel 0 is "
                              "evaluated — its coordinate is channel 0's",
                              generator);
            }
        }
        out[0] = std::clamp(rasterColor[0], 0.0f, 1.0f);
        out[1] = std::clamp(rasterColor[1], 0.0f, 1.0f);
        return;
    }
    if (texgen.type == 1) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            lucent::error("nrender",
                          "texgen {} is EMBOSS (bump mapping) — not implemented; its "
                          "coordinate is the un-offset source",
                          generator);
        }
    }

    float input[4] = {0, 0, 1, 1};
    switch (texgen.sourceRow) {
    case 0:
        input[0] = vertex.x;
        input[1] = vertex.y;
        input[2] = vertex.z;
        break;
    case 1:
        input[0] = vertex.nx;
        input[1] = vertex.ny;
        input[2] = vertex.nz;
        break;
    case 2:
        input[0] = rasterColor[0];
        input[1] = rasterColor[1];
        break;
    default:
        if (texgen.sourceRow >= 5 && texgen.sourceRow - 5 < 4) {
            input[0] = vertex.uv[texgen.sourceRow - 5][0];
            input[1] = vertex.uv[texgen.sourceRow - 5][1];
        }
        break;
    }
    if (texgen.inputForm == 0)
        input[2] = 1.0f;

    if (texgen.mtxSlot >= 10) {
        out[0] = input[0];
        out[1] = input[1];
        return;
    }
    if (!((xf.texMtxWritten >> texgen.mtxSlot) & 1)) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            lucent::error("nrender",
                          "texgen {} selects texture matrix {}, which no direct XF load "
                          "has written (GX also loads these by index — that path is not "
                          "parsed); using the raw coordinate",
                          generator, texgen.mtxSlot);
        }
        out[0] = input[0];
        out[1] = input[1];
        return;
    }

    const float* matrix = xf.texMtx[texgen.mtxSlot];
    const float s =
        matrix[0] * input[0] + matrix[1] * input[1] + matrix[2] * input[2] + matrix[3] * input[3];
    const float t =
        matrix[4] * input[0] + matrix[5] * input[1] + matrix[6] * input[2] + matrix[7] * input[3];
    if (texgen.projection == 1) {
        const float q = matrix[8] * input[0] + matrix[9] * input[1] + matrix[10] * input[2] +
                        matrix[11] * input[3];
        const float inverse = (q > 1e-6f || q < -1e-6f) ? 1.0f / q : 0.0f;
        out[0] = s * inverse;
        out[1] = t * inverse;
    } else {
        out[0] = s;
        out[1] = t;
    }
}
