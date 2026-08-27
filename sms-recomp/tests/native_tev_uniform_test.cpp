#include "native_render.h"
#include "native_tev_uniform.h"

#include <cmath>
#include <cstdlib>

namespace {

void require(bool condition) {
    if (!condition)
        std::abort();
}

} // namespace

int main() {
    SbrTevState tev{};
    tev.numStages = 2;
    tev.stage[0].cA = 3;
    tev.stage[0].cB = 5;
    tev.stage[0].cC = 7;
    tev.stage[0].cD = 9;
    tev.stage[0].cBias = 1;
    tev.stage[0].cSub = 1;
    tev.stage[0].cClamp = 0;
    tev.stage[0].cScale = 2;
    tev.stage[0].aA = 1;
    tev.stage[0].aB = 2;
    tev.stage[0].aC = 3;
    tev.stage[0].aD = 4;
    tev.stage[0].cDest = 2;
    tev.stage[0].aDest = 3;
    tev.stage[0].texEnable = 1;
    tev.stage[0].swapRas = 2;
    tev.stage[0].swapTex = 3;
    tev.stage[0].kC = 0;
    tev.swapTable[2][0] = 3;
    tev.swapTable[2][1] = 2;
    tev.swapTable[2][2] = 1;
    tev.swapTable[2][3] = 0;
    tev.reg[2][3] = 0.625f;
    tev.alphaOp0 = 4;
    tev.alphaOp1 = 6;
    tev.alphaLogic = 2;
    tev.alphaRef0 = 31;
    tev.alphaRef1 = 191;

    SbrNativeTevUniform uniform{};
    sbr_native_pack_tev_uniform(tev, uniform);

    require(uniform.control[0] == 2);
    require(uniform.cSel[0][0] == 3 && uniform.cSel[0][1] == 5 && uniform.cSel[0][2] == 7 &&
            uniform.cSel[0][3] == 9);
    require(uniform.cOp[0][0] == 1 && uniform.cOp[0][1] == 1 && uniform.cOp[0][2] == 0 &&
            uniform.cOp[0][3] == 2);
    require(uniform.aSel[0][0] == 1 && uniform.aSel[0][1] == 2 && uniform.aSel[0][2] == 3 &&
            uniform.aSel[0][3] == 4);
    require(uniform.dest[0][0] == 2 && uniform.dest[0][1] == 3 && uniform.dest[0][2] == 1);
    require(((uniform.dest[0][3] >> 19) & 3) == 2);
    require(((uniform.dest[0][3] >> 21) & 3) == 3);
    require(uniform.swapTable[2][0] == 3 && uniform.swapTable[2][1] == 2 &&
            uniform.swapTable[2][2] == 1 && uniform.swapTable[2][3] == 0);
    require(std::fabs(uniform.konst[0][0] - 1.0f) < 0.0001f);
    require(std::fabs(uniform.regInit[2][3] - 0.625f) < 0.0001f);
    require(uniform.control[1] == 4 && uniform.control[2] == 6 && uniform.control[3] == 2);
    require(uniform.alphaRef[0] == 31.0f && uniform.alphaRef[1] == 191.0f);
    return 0;
}
