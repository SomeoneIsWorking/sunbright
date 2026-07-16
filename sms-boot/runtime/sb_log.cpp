// sb_log.cpp — implementation of the sb_log.h diagnostic channel registry.
#include <sb_log.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// Parsed once from SB_LOG. Comma-separated channel names; "all" enables
// everything; "list" prints channel names as they register.
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
  if (!e || !*e) return;
  std::snprintf(g_cfg.spec, sizeof(g_cfg.spec), "%s", e);
  // split on commas in place; scan tokens for the two keywords
  for (char* p = g_cfg.spec; *p; ++p)
    if (*p == ',') *p = '\0';
  const char* end = g_cfg.spec + sizeof(g_cfg.spec);
  for (const char* t = g_cfg.spec; t < end && *t;) {
    if (std::strcmp(t, "all") == 0) g_cfg.all = true;
    if (std::strcmp(t, "list") == 0) g_cfg.list = true;
    t += std::strlen(t) + 1;
  }
}

bool spec_has(const char* chan) {
  const char* end = g_cfg.spec + sizeof(g_cfg.spec);
  for (const char* t = g_cfg.spec; t < end && *t;) {
    if (std::strcmp(t, chan) == 0) return true;
    t += std::strlen(t) + 1;
  }
  return false;
}

}  // namespace

extern "C" int sb_log_enabled(const char* chan) {
  parse_once();
  if (g_cfg.list) {
    // Registry discovery: announce each channel once as callsites first check it.
    // (Duplicate announcements only if multiple callsites share a channel — fine.)
    std::fprintf(stderr, "[sb-log] channel available: %s\n", chan);
  }
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
