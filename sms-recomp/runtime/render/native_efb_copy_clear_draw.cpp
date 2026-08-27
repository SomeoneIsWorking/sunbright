#include "native_efb_copy_clear_draw.h"

bool sbr_native_efb_copy_clear_draw(const NativeEfbCopyPlan& plan,
                                    NativeEfbCopyClearDraw& draw) noexcept {
    if (!plan.has_clear())
        return false;

    const NativeEfbCopyClear& clear = plan.clear;
    auto vertex = [&clear](float x, float y) {
        SbrVertex result{};
        result.x = x;
        result.y = y;
        result.z = clear.depth;
        result.w = 1.0f;
        result.r = result.r1 = clear.color[0];
        result.g = result.g1 = clear.color[1];
        result.b = result.b1 = clear.color[2];
        result.a = result.a1 = clear.color[3];
        return result;
    };
    draw.vertices[0] = vertex(-1.0f, -1.0f);
    draw.vertices[1] = vertex(3.0f, -1.0f);
    draw.vertices[2] = vertex(-1.0f, 3.0f);
    draw.state.test = clear.depthUpdate ? 1 : 0;
    draw.state.func = 7; // GX_ALWAYS
    draw.state.write = clear.depthUpdate ? 1 : 0;
    draw.state.colorUpdate = clear.colorUpdate ? 1 : 0;
    draw.state.alphaUpdate = clear.alphaUpdate ? 1 : 0;
    draw.state.scissor[0] = static_cast<int16_t>(plan.source.x);
    draw.state.scissor[1] = static_cast<int16_t>(plan.source.y);
    draw.state.scissor[2] = static_cast<int16_t>(plan.source.width);
    draw.state.scissor[3] = static_cast<int16_t>(plan.source.height);

    draw.tev.numStages = 1;
    draw.tev.stage[0].cA = draw.tev.stage[0].cB = draw.tev.stage[0].cC = 15; // GX_CC_ZERO
    draw.tev.stage[0].cD = 10;                                               // GX_CC_RASC
    draw.tev.stage[0].aA = draw.tev.stage[0].aB = draw.tev.stage[0].aC = 7;  // GX_CA_ZERO
    draw.tev.stage[0].aD = 5;                                                // GX_CA_RASA
    draw.tev.stage[0].cClamp = draw.tev.stage[0].aClamp = 1;
    return true;
}
