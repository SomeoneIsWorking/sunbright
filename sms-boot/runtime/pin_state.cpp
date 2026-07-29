// pin_state.cpp — native side of the cross-engine state-pin harness. See pin_state.h.
#include "pin_state.h"

#include <sb_log.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

struct PinState {
  bool active = false;
  bool loaded = false;
  float camPos[3]{};
  float camUp[3]{};
  float camTarget[3]{};
  float camFovy = 0.f;
};

PinState g_pin;

// Minimal JSON scan for the fixed keys the fork emits (values are flat number
// arrays / scalars). Not a general parser — matches "key": [a, b, c] / "key": v.
bool find_vec3(const char* buf, const char* key, float out[3]) {
  const char* p = std::strstr(buf, key);
  if (!p) return false;
  p = std::strchr(p, '[');
  if (!p) return false;
  return std::sscanf(p, "[ %f , %f , %f", &out[0], &out[1], &out[2]) == 3;
}
bool find_scalar(const char* buf, const char* key, float* out) {
  const char* p = std::strstr(buf, key);
  if (!p) return false;
  p = std::strchr(p, ':');
  if (!p) return false;
  return std::sscanf(p + 1, " %f", out) == 1;
}

void load_once() {
  g_pin.loaded = true;
  const char* path = std::getenv("SB_PIN_STATE");
  if (!path || !*path) return;
  FILE* f = std::fopen(path, "rb");
  if (!f) {
    // LOGGER-EXEMPT: loud regardless of SB_LOG. An explicitly-requested pin that cannot load
    // is a BROKEN RUN, not a diagnostic — it must be visible with no channel enabled, so this
    // stays a direct stderr write rather than becoming a channel-gated log line.
    std::fprintf(stderr, "[sb-pin] SB_PIN_STATE='%s' not readable\n", path);
    return;
  }
  char buf[4096];
  size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
  std::fclose(f);
  buf[n] = '\0';
  bool ok = find_vec3(buf, "\"camPos\"", g_pin.camPos) &&
            find_vec3(buf, "\"camUp\"", g_pin.camUp) &&
            find_vec3(buf, "\"camTarget\"", g_pin.camTarget) &&
            find_scalar(buf, "\"camFovy\"", &g_pin.camFovy);
  g_pin.active = ok;
  std::fprintf(stderr,
               "[sb-pin] SB_PIN_STATE='%s' %s: pos=(%.1f,%.1f,%.1f) tgt=(%.1f,%.1f,%.1f) "
               "up=(%.1f,%.1f,%.1f) fovy=%.1f\n",
               path, ok ? "ACTIVE" : "PARSE-FAILED", g_pin.camPos[0], g_pin.camPos[1],
               g_pin.camPos[2], g_pin.camTarget[0], g_pin.camTarget[1], g_pin.camTarget[2],
               g_pin.camUp[0], g_pin.camUp[1], g_pin.camUp[2], g_pin.camFovy);
}

}  // namespace

bool sb_pin_get_camera(float pos[3], float up[3], float tgt[3], float* fovy) {
  if (!g_pin.loaded) load_once();
  if (!g_pin.active) return false;
  std::memcpy(pos, g_pin.camPos, 12);
  std::memcpy(up, g_pin.camUp, 12);
  std::memcpy(tgt, g_pin.camTarget, 12);
  *fovy = g_pin.camFovy;
  return true;
}
