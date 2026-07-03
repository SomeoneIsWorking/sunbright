// engine.cpp — sb::engine::mode() backing. See runtime/engine.h.

#include "../../runtime/engine.h"
#include "../platform/gx_fifo.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sb::engine {

namespace {
RenderMode s_mode = RenderMode::NATIVE_PC;
bool s_inited = false;
}

void init_from_env() {
    if (s_inited) return;
    s_inited = true;
    const char* v = std::getenv("SB_RENDER");
    if (!v || !v[0]) v = std::getenv("SUNBRIGHT_RENDER");
    if (v && v[0]) {
        if (!std::strcmp(v, "oracle") || !std::strcmp(v, "gx") || !std::strcmp(v, "dolphin"))
            s_mode = RenderMode::GX_ORACLE;
        else if (!std::strcmp(v, "native") || !std::strcmp(v, "pc") || !std::strcmp(v, "sdl3"))
            s_mode = RenderMode::NATIVE_PC;
        else
            std::fprintf(stderr, "[engine] unrecognized SB_RENDER='%s' (use native|oracle), keeping default\n", v);
    }
    std::printf("[engine] render mode = %s\n", mode_name());
    // GX seam FIFO recording (see native/platform/gx_fifo.h). Enabled iff
    // GX_ORACLE, so NATIVE_PC pays no cost.
    sb::gxfifo::init();
}

RenderMode mode() { return s_mode; }

const char* mode_name() {
    switch (s_mode) {
        case RenderMode::NATIVE_PC:  return "native";
        case RenderMode::GX_ORACLE:  return "oracle";
    }
    return "?";
}

} // namespace sb::engine
