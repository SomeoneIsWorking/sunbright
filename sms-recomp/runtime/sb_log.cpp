// sb_log.cpp — the SB_LOG channel registry for this runtime.
//
// Aurora gates a large part of its GX diagnostics on a WEAK `sb_log_enabled` that the hosting
// runtime is expected to provide (`pnzero`, `pn`, and friends in lib/gx/command_processor.cpp).
// sms-boot provides it; this runtime did not, so the weak symbol resolved to null and
// `sb_gx_log_on` returned false for EVERY channel. The diagnostics did not report being
// unavailable — they silently produced nothing, which reads exactly like "the condition never
// occurred". That cost a false negative: an SB_LOG=pnzero run reported zero zero-rotation matrix
// uploads while the channel was simply dead.
//
// Same semantics as sms-boot's registry so one SB_LOG spec means the same thing in both
// runtimes: comma-separated channel names, "all" enables everything, "list" announces channels
// as callsites first check them.

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

struct LogConfig {
    char spec[256]{};   // owned copy of SB_LOG with commas -> NULs
    bool all = false;
    bool list = false;
    bool parsed = false;
};

LogConfig g_cfg;

void parse_once() {
    if (g_cfg.parsed) return;
    g_cfg.parsed = true;
    const char* e = std::getenv("SB_LOG");
    if (e == nullptr || *e == '\0') return;
    std::snprintf(g_cfg.spec, sizeof(g_cfg.spec), "%s", e);
    for (char* p = g_cfg.spec; *p != '\0'; ++p)
        if (*p == ',') *p = '\0';
    const char* end = g_cfg.spec + sizeof(g_cfg.spec);
    for (const char* t = g_cfg.spec; t < end && *t != '\0';) {
        if (std::strcmp(t, "all") == 0) g_cfg.all = true;
        if (std::strcmp(t, "list") == 0) g_cfg.list = true;
        t += std::strlen(t) + 1;
    }
}

bool spec_has(const char* chan) {
    const char* end = g_cfg.spec + sizeof(g_cfg.spec);
    for (const char* t = g_cfg.spec; t < end && *t != '\0';) {
        if (std::strcmp(t, chan) == 0) return true;
        t += std::strlen(t) + 1;
    }
    return false;
}

}  // namespace

extern "C" int sb_log_enabled(const char* chan) {
    parse_once();
    if (g_cfg.list) std::fprintf(stderr, "[sb-log] channel available: %s\n", chan);
    if (g_cfg.all) return 1;
    return spec_has(chan) ? 1 : 0;
}

extern "C" void sb_logf(const char* chan, const char* fmt, ...) {
    std::fprintf(stderr, "[%s] ", chan);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}
