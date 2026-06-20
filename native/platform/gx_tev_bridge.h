// gx_tev_bridge — convert the captured GX TEV state (GXState.tev, written by the
// GXSetTev* seam) into the renderer's NgxTevState, which the shipping tev_shader
// decodes into a GLSL combiner. This is the "GX-command tees -> GXState -> renderer"
// path: the GXSetTev* register bits ARE the NgxTevState.color_env/alpha_env layout,
// so the conversion is a direct field copy.
#pragma once
#include "gx_state.h"
#include "ngx_render_data.h"

namespace sb::platform::gx {

// Build the renderer's per-material TEV state from the captured GXState. numStages is
// taken from GXState.numTevStages (GXSetNumTevStages). Pure; no allocation.
inline NgxTevState ngx_tevstate_from_gx(const GXState& g) {
    NgxTevState st{};
    int ns = g.numTevStages; if (ns < 1) ns = 1; if (ns > 16) ns = 16;
    st.num_stages = (uint8_t)ns;
    for (int i = 0; i < 16; ++i) {
        st.stage[i].color_env  = g.tev.colorEnv[i];
        st.stage[i].alpha_env  = g.tev.alphaEnv[i];
        st.stage[i].kcsel      = g.tev.kcsel[i];
        st.stage[i].kasel      = g.tev.kasel[i];
        st.stage[i].texcoord   = g.tev.texcoord[i];
        st.stage[i].texmap     = g.tev.texmap[i];
        st.stage[i].color_chan = g.tev.colorChan[i];
    }
    for (int i = 0; i < 4; ++i) {
        st.swap_table[i] = g.tev.swapTable[i];
        for (int c = 0; c < 4; ++c) {
            st.tev_color[i][c] = g.tev.tevColor[i][c];
            st.kcolor[i][c]    = g.tev.kColor[i][c];
        }
    }
    st.ind.num_stages = g.tev.numIndStages;
    return st;
}

} // namespace sb::platform::gx
