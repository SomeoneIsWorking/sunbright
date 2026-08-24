#include "native_tev_uniform.h"

#include "native_render.h"

#include <algorithm>
#include <cstdlib>

void sbr_native_pack_tev_uniform(const SbrTevState& tev, SbrNativeTevUniform& uniform) {
    const int stageCount = static_cast<int>(std::min<uint32_t>(tev.numStages, 16));
    uniform.control[0] = stageCount;
    for (int index = 0; index < 16; ++index) {
        const SbrTevStage& stage = tev.stage[index];
        uniform.cSel[index][0] = stage.cA;
        uniform.cSel[index][1] = stage.cB;
        uniform.cSel[index][2] = stage.cC;
        uniform.cSel[index][3] = stage.cD;
        uniform.cOp[index][0] = stage.cBias;
        uniform.cOp[index][1] = stage.cSub;
        uniform.cOp[index][2] = stage.cClamp;
        uniform.cOp[index][3] = stage.cScale;
        uniform.aSel[index][0] = stage.aA;
        uniform.aSel[index][1] = stage.aB;
        uniform.aSel[index][2] = stage.aC;
        uniform.aSel[index][3] = stage.aD;
        uniform.aOp[index][0] = stage.aBias;
        uniform.aOp[index][1] = stage.aSub;
        uniform.aOp[index][2] = stage.aClamp;
        uniform.aOp[index][3] = stage.aScale;
        uniform.dest[index][0] = stage.cDest;
        uniform.dest[index][1] = stage.aDest;
        uniform.dest[index][2] = stage.texEnable;

        // Texmap and texcoord are independent RAS1_TREF selectors. Named routing stays opt-in
        // while known GPU-copy-backed textures have no native producer; pinning every other unit
        // to zero is the diagnostic baseline documented in the renderer journal.
        static const uint32_t unitMask = [] {
            if (const char* mask = std::getenv("SBR_TEXMAP_UNITS"))
                return static_cast<uint32_t>(std::strtoul(mask, nullptr, 0));
            const char* named = std::getenv("SBR_TEXMAP_NAMED");
            return named != nullptr && named[0] != '\0' && named[0] != '0' ? 0xFFu : 0x1u;
        }();
        // Force routing asks whether a shader texture slot works independently of material state.
        static const int32_t forceUnit = [] {
            const char* value = std::getenv("SBR_TEXMAP_FORCE");
            return value != nullptr ? static_cast<int32_t>(std::strtol(value, nullptr, 0)) : -1;
        }();
        const int32_t map = static_cast<int32_t>(stage.texmap & 7);
        const int32_t unit =
            forceUnit >= 0 ? (forceUnit & 7) : (((unitMask >> map) & 1u) != 0 ? map : 0);
        uniform.dest[index][3] = unit | static_cast<int32_t>(stage.texcoord & 3) << 8 |
                                 static_cast<int32_t>(stage.rasChannel & 7) << 16;
        sbr_tev_konst(tev, static_cast<unsigned>(index), uniform.konst[index]);
    }

    for (int reg = 0; reg < 4; ++reg)
        for (int component = 0; component < 4; ++component)
            uniform.regInit[reg][component] = tev.reg[reg][component];

    // SBR_ALPHATEST=0 forces both comparisons to ALWAYS for the alpha-test ablation.
    static const bool alphaTest = [] {
        const char* value = std::getenv("SBR_ALPHATEST");
        return !(value != nullptr && value[0] == '0');
    }();
    uniform.control[1] = alphaTest ? tev.alphaOp0 : 7;
    uniform.control[2] = alphaTest ? tev.alphaOp1 : 7;
    uniform.control[3] = tev.alphaLogic;
    uniform.alphaRef[0] = static_cast<float>(tev.alphaRef0);
    uniform.alphaRef[1] = static_cast<float>(tev.alphaRef1);

    // 1 visualizes the raw texture sample, 2 its alpha, and 3 its generated coordinate.
    static const float visualization = [] {
        const char* value = std::getenv("SBR_TEV_VIZ");
        return value != nullptr ? static_cast<float>(std::strtol(value, nullptr, 10)) : 0.0f;
    }();
    uniform.alphaRef[2] = visualization;
}
