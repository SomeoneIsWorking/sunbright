// engine.cpp — sb::engine::mode() backing. See runtime/engine.h.

#include "../engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sb::engine {

namespace {
#if defined(SB_RENDER_GC_FAITHFUL)
RenderMode s_mode = RenderMode::GC_FAITHFUL;
#else
RenderMode s_mode = RenderMode::PC_NATIVE;
#endif
bool s_inited = false;
}

void init_from_env() {
    if (s_inited) return;
    s_inited = true;
    // Compile-time path is authoritative. SB_RENDER at runtime is a log echo only —
    // if it disagrees with the compiled path, we shout and ignore it.
    const char* v = std::getenv("SB_RENDER");
    if (!v || !v[0]) v = std::getenv("SUNBRIGHT_RENDER");
    if (v && v[0] && std::strcmp(v, mode_name()) != 0) {
        std::fprintf(stderr,
            "[engine] SB_RENDER='%s' ignored: this build is %s (rebuild with -DSB_RENDER=%s to switch)\n",
            v, mode_name(), (s_mode == RenderMode::PC_NATIVE ? "gc" : "pc"));
    }
    std::printf("[engine] render path = %s\n", mode_name());
}

RenderMode mode() { return s_mode; }

const char* mode_name() {
    switch (s_mode) {
        case RenderMode::PC_NATIVE:   return "pc";
        case RenderMode::GC_FAITHFUL: return "gc";
    }
    return "?";
}

} // namespace sb::engine
