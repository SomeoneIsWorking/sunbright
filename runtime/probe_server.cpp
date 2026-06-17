#include "probe_server.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <string>
#include <thread>
#include <chrono>
#include <mutex>

#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#ifdef HAVE_DOLPHIN_CORE
#  include "Common/Config/Config.h"
#  include "Core/Config/MainSettings.h"
#  include "Core/Config/GraphicsSettings.h"
#  include "Core/System.h"
#  include "Core/Core.h"
#  include "Core/CoreTiming.h"
#  include "Core/HW/SystemTimers.h"
#  include "VideoCommon/PerformanceMetrics.h"
#endif

ProbeCounters g_probe;
bool g_probe_enabled = false;

#ifdef HAVE_DOLPHIN_CORE
#  include <vector>
#  include <algorithm>
#  include <fstream>
#  include <atomic>
#ifdef HAVE_DOLPHIN_CORE
#include "Core/System.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/DSP.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/FrameDumper.h"
#include "VideoCommon/Statistics.h"   // g_stats: Dolphin's per-frame GX draw/prim counts
#include <sys/stat.h>
#endif
extern u32 mem_r32(u32 ea);
extern u16 mem_r16(u32 ea);
extern void sunbright_repl_inject(const char* line);   // main_sdl.cpp — /pad scripted input
extern unsigned long g_nintr_counts[32];               // dolphin_hook.cpp — /nintr counters
extern void mem_w32(u32, u32);                         // memory_bridge — /w diagnostic poke
extern void mem_w8(u32, u8);
extern unsigned long g_ds_token_dispatches, g_ds_callbacks, g_ds_sleeps, g_ds_wakes;
unsigned long long watchdog_vi_fields();

// ── REPL-readable trace ring ──────────────────────────────────────────────────
// A thin observer (e.g. a SUNBRIGHT_OVERRIDE) calls sb_trace(tag,a,b,c,d) to record an event;
// the REPL `/tracelog` endpoint dumps it. Keeps execution-trace data in the REPL instead of
// env-gated stderr logs. Lock-free-ish: a single producer (the guest thread holding the CPU
// token) writes; the probe thread reads a snapshot. Good enough for diagnosis.
struct TraceRec { char tag[16]; uint32_t a, b, c, d; uint64_t seq; };
constexpr int SB_TRACE_N = 8192;   // ~40 B/entry; big enough to hold the seconds AROUND a fault
                                   // at audio-frame event rates (the 512 ring aged out the
                                   // dead-audio death window before a poller could react)
TraceRec           g_trace[SB_TRACE_N];
std::atomic<uint64_t> g_trace_seq{0};

extern "C" void sb_trace(const char* tag, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    uint64_t s = g_trace_seq.fetch_add(1, std::memory_order_relaxed);
    TraceRec& r = g_trace[s % SB_TRACE_N];
    int i = 0; for (; tag[i] && i < 15; i++) r.tag[i] = tag[i]; r.tag[i] = 0;
    r.a = a; r.b = b; r.c = c; r.d = d; r.seq = s;
}
#endif

extern "C" int njas_probe(char* out, int cap);
int interp60_probe(char* out, int cap, const char* query);   // runtime/interp60.h
extern "C" int native_vi_probe(char* out, int cap, const char* query);  // native_vi2.cpp
void gxs_ngx_parity_stats(unsigned long*, unsigned long*, unsigned long*, const char**);  // gx_stream.h
int sb_tex_selftest(char*, int);                             // runtime/render/tex_decode_selftest.cpp
int sb_vk_quad_selftest(char*, int);                         // runtime/render/vk_quad.cpp
int sb_j2d_dump(char*, int);                                 // runtime/render/j2d_walk.cpp
int sb_j2d_screens_dump(char*, int);                         // runtime/render/j2d_walk.cpp (/j2dscreens)
int sb_tex_at_dump(char*, int, uint32_t, int, int, int);     // runtime/render/j2d_walk.cpp (/texat)
int sb_j2d_render(char*, int);                               // runtime/render/j2d_render.cpp
int sb_ngx_vertex_selftest(char*, int);                      // runtime/ngx/ngx_vertex.cpp
int sb_ngx_mesh_selftest(char*, int);                        // runtime/ngx/ngx_mesh.cpp
int sb_ngx_shape_dump(char*, int);                           // runtime/overrides/ngx_j3d_shape.cpp
int sb_ngx_shapes_dump(char*, int);                          // runtime/overrides/ngx_j3d_shape.cpp (/ngxshapes)
int sb_ngx_gxstate_dump(char*, int);                         // runtime/overrides/ngx_j3d_shape.cpp (/gxstate)
extern "C" void sb_ngx_set_gxstate_ti(int);                  // runtime/overrides/ngx_j3d_shape.cpp (/gxstate?ti=)
int sb_ngx_order_dump(char*, int);                           // runtime/overrides/ngx_j3d_shape.cpp (/ngxorder)
extern "C" int sb_ngx_set_prefix(int);                       // runtime/render/ngx_present.cpp (/ngxprefix?n=)
int sb_ngx_efbcopies_dump(char*, int);                       // runtime/overrides/ngx_j3d_shape.cpp (/efbcopies)
int sb_ngx_pixel_batch(float, float, char*, int);            // runtime/overrides/ngx_j3d_shape.cpp
int sb_ngx_pixel_blend(float, float, char*, int);            // runtime/overrides/ngx_j3d_shape.cpp (/pixblend)
extern "C" int ngx_geom_diff_report(char* out, int cap);     // runtime/overrides/ngx_j3d_shape.cpp (/ngxgeomdiff)
extern "C" int ngx_proj_diff_report(char* out, int cap);     // runtime/overrides/ngx_j3d_shape.cpp (/ngxproj)
extern "C" int sb_ngx_set_onlyti(int);                       // runtime/render/ngx_present.cpp (/ngxonly)
extern "C" int sb_ngx_set_skipti(int);                       // runtime/render/ngx_present.cpp (/ngxskip)
extern "C" int sb_ngx_set_onlyepoch(int);                    // runtime/render/ngx_present.cpp (/ngxepoch)
extern "C" int sb_ngx_set_dropepoch(int);                    // runtime/render/ngx_present.cpp (/ngxepoch)
extern "C" int sb_ngx_set_rtfilter(int);                     // runtime/render/ngx_present.cpp (/ngxrtfilter)
extern "C" void sb_ngx_skipset_clear();                      // runtime/render/ngx_present.cpp (/ngxskipset)
extern "C" void sb_ngx_skipset_add(int);                     // runtime/render/ngx_present.cpp (/ngxskipset)
int sb_ngx_render(char*, int);                               // runtime/render/vk_mesh.cpp
int sb_ngx_present_test(char*, int);                         // runtime/render/vk_mesh.cpp
extern "C" void sb_ngx_present_stats(unsigned long*, unsigned long*, unsigned long*, int*, int*, int*, unsigned long*);  // ngx_present.cpp
int sb_tev_shader_selftest(char*, int);                      // runtime/render/tev_shader.cpp
void gxs_frametime_reset();                                   // runtime/gx_stream.h
void gxs_frametime_stats(unsigned long*, double*, double*, double*, double*);
void gxs_frametime_decode(double*, double*);
void gxs_frametime_ct(double*, double*);
extern "C" void sb_census_dump(const char* p);
namespace {

#ifdef HAVE_DOLPHIN_CORE
// ── REPL: interactive guest-state inspection (curl the endpoints; no rebuild/env-log cycle) ──
// Symbol map (reference/sms_gmse01_funcs.txt: "ADDR name" per line), loaded once for addr→name.
std::vector<std::pair<u32, std::string>>& symtab() {
    static std::vector<std::pair<u32, std::string>> t;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        const char* path = getenv("SUNBRIGHT_SYMBOLS");
        std::ifstream f(path ? path : "reference/sms_gmse01_funcs.txt");
        std::string line;
        while (std::getline(f, line)) {
            char* end = nullptr;
            unsigned long a = strtoul(line.c_str(), &end, 16);
            if (end == line.c_str() || !end) continue;
            while (*end == ' ' || *end == '\t') end++;
            if (a) t.emplace_back((u32)a, std::string(end));
        }
        std::sort(t.begin(), t.end());
        fprintf(stderr, "[probe/repl] loaded %zu symbols\n", t.size());
    }
    return t;
}
// Nearest function entry at or below `a` → "name+0xNN" (or raw hex if no map).
std::string sym(u32 a) {
    auto& t = symtab();
    if (t.empty() || a < t.front().first) { char b[16]; snprintf(b, sizeof b, "%08x", a); return b; }
    auto it = std::upper_bound(t.begin(), t.end(), a,
        [](u32 v, const std::pair<u32,std::string>& p){ return v < p.first; });
    --it;
    char b[256]; snprintf(b, sizeof b, "%s+0x%x", it->second.c_str(), a - it->first);
    return b;
}
u32 qarg(const char* path, const char* key, u32 def) {
    std::string pat = std::string(key) + "=";
    const char* p = strstr(path, pat.c_str());
    if (!p) return def;
    return (u32)strtoul(p + pat.size(), nullptr, 16);
}
// Decimal variant — for durations (ms=...); hex-parsing 90000 as 0x90000 ms once wedged the
// single-threaded server for 10 minutes and looked like "HTTP is dead".
u32 qarg_dec(const char* path, const char* key, u32 def) {
    std::string pat = std::string(key) + "=";
    const char* p = strstr(path, pat.c_str());
    if (!p) return def;
    return (u32)strtoul(p + pat.size(), nullptr, 10);
}

// 60fps midpoint-verification tool (interp_verify.cpp / fork Present.cpp). Namespace-scope so the
// extern "C" linkage is legal (block-scope extern "C" is ill-formed).
extern "C" void interp_verify_arm(int);
extern "C" int  interp_verify_report(char*, int);
extern "C" int  sb_capture_frames;
extern "C" volatile int g_sb_ngx_present;   // /abshot toggles the present source (Present.cpp)
extern "C" volatile int g_sb_ab_capture;    // /abshot2 arms same-present dual capture (Present.cpp)
extern "C" unsigned long sb_ngx_front_frame();  // /abshot2 liveness: published ngx snapshot frame id
extern "C" int sb_ngx_set_mtxsrc(int);          // /ngxmtxsrc live skinned-matrix source A/B
extern "C" int sb_ngx_set_dbg(int);         // /ngxdbg sets the native renderer debug mode
extern "C" void sb_ngx_set_nolight(int);    // /ngxdbg?nolight= toggles native lighting (capture)
extern "C" void sb_ngx_set_freeze(int);     // /ngxfreeze latches the published snapshot
extern "C" int  sb_ngx_get_freeze();
extern "C" int  sb_ngx_set_noblend(int);    // /ngxnoblend forces materials opaque
extern "C" int sb_xfmem_dump(char*, int);   // /xfdump prints live Dolphin xfmem (ngx_j3d_shape.cpp)
extern "C" int sb_xfmem_hist(char*, int);   // /xfhist prints distinct xfmem draw-tuples (ngx_j3d_shape.cpp)
extern "C" int sb_ngx_gen_shader(unsigned, char*, int);   // /skyshader dumps generated TEV GLSL (ngx_j3d_shape.cpp)

// REPL request handler. Returns the response body for any /repl path; empty string = not a REPL path.
std::string handle_repl(const char* path) {
    // 64 KB: /tracelog dumps up to 512 ring entries (~80 bytes each) — the old 8 KB cut the
    // tail off exactly where a deadlock's last events live.
    static thread_local char buf[65536]; int n = 0;
    auto app = [&](const char* fmt, auto... a){ if (n < (int)sizeof buf) n += snprintf(buf+n, sizeof buf-n, fmt, a...); };

    if (strncmp(path, "/r?", 3) == 0 || strncmp(path, "/r ", 3) == 0) {
        u32 a = qarg(path, "a", 0), cnt = qarg(path, "n", 8);
        if (cnt > 256) cnt = 256;
        for (u32 i = 0; i < cnt; i++) {
            if ((i & 3) == 0) app("%08x:", a + i*4);
            app(" %08x", mem_r32(a + i*4));
            if ((i & 3) == 3) app("\n");
        }
        if (cnt & 3) app("\n");
        return std::string(buf, n);
    }
    if (strncmp(path, "/r16?", 5) == 0) {   // 16-bit reads (CP/PE/VI MMIO regs have no 32-bit mapping)
        u32 a = qarg(path, "a", 0), cnt = qarg(path, "n", 8);
        if (cnt > 256) cnt = 256;
        for (u32 i = 0; i < cnt; i++) {
            if ((i & 7) == 0) app("%08x:", a + i*2);
            app(" %04x", (unsigned)mem_r16(a + i*2));
            if ((i & 7) == 7) app("\n");
        }
        if (cnt & 7) app("\n");
        return std::string(buf, n);
    }
    if (strncmp(path, "/gx", 3) == 0 && strncmp(path, "/gxstate", 8) != 0) {     // CP/Fifo internals (dual-core pacing diagnostics)
#ifdef HAVE_DOLPHIN_CORE
        auto& sys = Core::System::GetInstance();
        auto& cp  = sys.GetCommandProcessor();
        auto& ff  = cp.GetFifo();
        app("CPBase=%08x CPEnd=%08x CPHiWM=%08x CPLoWM=%08x\n",
            ff.CPBase.load(), ff.CPEnd.load(), ff.CPHiWatermark, ff.CPLoWatermark);
        app("wp=%08x rp=%08x dist=%08x bp=%08x\n",
            ff.CPWritePointer.load(), ff.CPReadPointer.load(),
            ff.CPReadWriteDistance.load(), ff.CPBreakpoint.load());
        app("bpEnable=%d bpInt=%d bpHit=%d hiWM=%d hiWMInt=%d loWM=%d loWMInt=%d gpRead=%d\n",
            (int)ff.bFF_BPEnable.load(), (int)ff.bFF_BPInt.load(), (int)ff.bFF_Breakpoint.load(),
            (int)ff.bFF_HiWatermark.load(), (int)ff.bFF_HiWatermarkInt.load(),
            (int)ff.bFF_LoWatermark.load(), (int)ff.bFF_LoWatermarkInt.load(),
            (int)ff.bFF_GPReadEnable.load());
        app("interrupt_waiting=%d pi_cause=%08x pi_mask=%08x\n",
            (int)cp.IsInterruptWaiting(),
            sys.GetProcessorInterface().GetCause(), sys.GetProcessorInterface().GetMask());
#endif
        return std::string(buf, n);
    }
    if (strncmp(path, "/tex", 4) == 0 && path[4] != 'a') {   // N1 native texture decoder parity vs Dolphin oracle
        char rep[8192];
        int fails = sb_tex_selftest(rep, sizeof rep);
        app("%s", rep);
        app("verdict=%s\n", fails == 0 ? "PARITY-OK" : (fails < 0 ? "NO-ORACLE" : "MISMATCH"));
        return std::string(buf, n);
    }
    if (strncmp(path, "/j2drender", 10) == 0) {  // N3 render the HUD natively (offscreen + PPM)
        char rep[8192];
        sb_j2d_render(rep, sizeof rep);
        app("%s", rep);
        return std::string(buf, n);
    }
    if (strncmp(path, "/texat", 6) == 0) {   // decode a guest texture (addr/fmt/w/h) → intensity grid
        char rep[4096];
        auto qv = [&](const char* k, long def) -> long {
            const char* q = strstr(path, k); if (!q) return def;
            return strtol(q + strlen(k), nullptr, 0);
        };
        sb_tex_at_dump(rep, sizeof rep, (uint32_t)qv("a=", 0), (int)qv("fmt=", 0),
                       (int)qv("w=", 0), (int)qv("h=", 0));
        app("%s", rep);
        return std::string(buf, n);
    }
    if (strncmp(path, "/j2dscreens", 11) == 0) {  // correlation: every recent J2DScreen root + window inventory
        char rep[16384];
        sb_j2d_screens_dump(rep, sizeof rep);
        app("%s", rep);
        return std::string(buf, n);
    }
    if (strncmp(path, "/j2d", 4) == 0) {     // N3 J2D pane-tree walk (live HUD draw data)
        char rep[16384];
        sb_j2d_dump(rep, sizeof rep);
        app("%s", rep);
        return std::string(buf, n);
    }
    if (strncmp(path, "/vkquad", 7) == 0) {  // N2 native Vulkan textured-quad offscreen render
        char rep[2048];
        sb_vk_quad_selftest(rep, sizeof rep);
        app("%s", rep);
        return std::string(buf, n);
    }
    if (strncmp(path, "/xfdump", 7) == 0) {  // live Dolphin xfmem lighting/colour (oracle = truth)
        char rep[2048]; int rn = sb_xfmem_dump(rep, sizeof rep); app("%.*s", rn, rep);
        return std::string(buf, n);
    }
    if (strncmp(path, "/xfhist", 7) == 0) {  // distinct xfmem tuples at J3DShape::draw (oracle = truth)
        char rep[4096]; int rn = sb_xfmem_hist(rep, sizeof rep); app("%.*s", rn, rep);
        return std::string(buf, n);
    }
    if (strncmp(path, "/skyshader", 10) == 0) {  // generated TEV GLSL for a material (default sky)
        unsigned ce = 0x09fae8; if (const char* p = strstr(path, "ce=")) ce = strtoul(p+3, nullptr, 16);
        char rep[8192]; int rn = sb_ngx_gen_shader(ce, rep, sizeof rep); app("%.*s", rn, rep);
        return std::string(buf, n);
    }
    if (strncmp(path, "/ngxmtxsrc", 10) == 0) {  // LIVE skinned-matrix source A/B (no rebuild)
        int m = 0; if (const char* p = strstr(path, "m=")) m = atoi(p + 2);
        app("ngx skinned matrix src = %d  (0=per-packet object-model, 1=g_posmtx, 2=modelview)\n",
            sb_ngx_set_mtxsrc(m));
        return std::string(buf, n);
    }
    if (strncmp(path, "/ngxproj", 8) == 0) {  // ngx projection vs Dolphin's ACTUAL projection
        char rep[2048]; int rn = ngx_proj_diff_report(rep, sizeof rep); app("%.*s", rn, rep);
        return std::string(buf, n);
    }
    if (strncmp(path, "/ngxgeomdiff", 12) == 0) {  // ngx-vs-Dolphin per-slot matrix differential
        char rep[16384]; int rn = ngx_geom_diff_report(rep, sizeof rep); app("%.*s", rn, rep);
        return std::string(buf, n);
    }
    if (strncmp(path, "/ngxdbg", 7) == 0) {
        // Flip the native renderer's debug mode on a LIVE scene (no relaunch). /ngxdbg?m=MODE
        // MODE: normal|tex|ras|cat|bid (or 0..4). Clears the pipeline cache → shaders regen.
        // (Must precede the broad "/ngx" parity handler below.)
        if (const char* p = strstr(path, "nolight=")) {
            int on = atoi(p + 8); sb_ngx_set_nolight(on);
            app("ngx nolight = %d\n", on);
            return std::string(buf, n);
        }
        int m = 0;
        if (const char* p = strstr(path, "m=")) {
            p += 2;
            if      (!strncmp(p, "normal", 6)) m = 0;
            else if (!strncmp(p, "tex", 3))    m = 1;
            else if (!strncmp(p, "ras", 3))    m = 2;
            else if (!strncmp(p, "cat", 3))    m = 3;
            else if (!strncmp(p, "bid", 3))    m = 4;
            else if (!strncmp(p, "tex1", 4))   m = 5;
            else if (!strncmp(p, "uv0", 3))    m = 6;
            else if (!strncmp(p, "uv1", 3))    m = 7;
            else m = atoi(p);
        }
        sb_ngx_set_dbg(m);
        static const char* names[5] = {"normal","tex","ras","cat","bid"};
        app("ngx debug mode = %d (%s)\n", m, (m>=0&&m<=4)?names[m]:"?");
        return std::string(buf, n);
    }
    if (strncmp(path, "/ngxvtx", 7) == 0) {  // N4 native GX vertex-attribute extractor self-test
        char rep[2048]; sb_ngx_vertex_selftest(rep, sizeof rep); app("%s", rep);
        return std::string(buf, n);
    }
    if (strncmp(path, "/ngxmesh", 8) == 0) {  // N4 native mesh assembly + triangulation self-test
        char rep[2048]; sb_ngx_mesh_selftest(rep, sizeof rep); app("%s", rep);
        return std::string(buf, n);
    }
    if (strncmp(path, "/tevshader", 10) == 0) {  // N5 TEV-state -> GLSL generator self-test
        static thread_local char rep[65536]; sb_tev_shader_selftest(rep, sizeof rep); app("%s", rep);
        return std::string(buf, n);
    }
    if (strncmp(path, "/drawstats", 10) == 0) {  // Dolphin's GX render-pass counts THIS FRAME (for A/B
        // vs the native ngx capture). Per-frame counters reset each frame; curl repeatedly + take the
        // max to read a full frame. num_draw_calls = GX draw operations Dolphin renders (all paths,
        // incl. non-J3DShape). Compare vs /ngxshape (which captures J3DShape only).
        const auto& f = g_stats.this_frame;
        app("gxstats(this_frame): draw_calls=%d prims=%d dl_prims=%d drawn_objects=%d "
            "triangles_drawn=%d triangles_in=%d vertices_loaded=%d efb_peeks=%d efb_pokes=%d "
            "dlists_called=%d shader_changes=%d\n",
            f.num_draw_calls, f.num_prims, f.num_dl_prims, f.num_drawn_objects,
            f.num_triangles_drawn, f.num_triangles_in, f.num_vertices_loaded,
            f.num_efb_peeks, f.num_efb_pokes, f.num_dlists_called, f.num_shader_changes);
        return std::string(buf, n);
    }
    if (strncmp(path, "/ngxshapes", 10) == 0) {  // per-shape NDC bbox (localize a misplaced shape)
        static thread_local char rep[16384]; sb_ngx_shapes_dump(rep, sizeof rep); app("%s", rep);
        return std::string(buf, n);
    }
    if (strncmp(path, "/gxstate", 8) == 0) {  // GX-cmd-stream vs ngx-object-model render-state diff
        if (const char* p = strstr(path, "ti=")) sb_ngx_set_gxstate_ti(atoi(p + 3));
        static thread_local char rep[8192]; sb_ngx_gxstate_dump(rep, sizeof rep); app("%s", rep);
        return std::string(buf, n);
    }
    if (strncmp(path, "/efbcopies", 10) == 0) {  // rolling EFB-copy log (display vs offscreen routing)
        static thread_local char rep[8192]; sb_ngx_efbcopies_dump(rep, sizeof rep); app("%s", rep);
        return std::string(buf, n);
    }
    if (strncmp(path, "/ngxshape", 9) == 0) {  // N4 live J3DShape native-mesh capture stats
        static thread_local char rep[16384]; sb_ngx_shape_dump(rep, sizeof rep); app("%s", rep);
        return std::string(buf, n);
    }
    if (strncmp(path, "/ngxorder", 9) == 0) {  // displayed batches in DRAW order (ti + vcount) — maps prefix N -> layer
        static thread_local char rep[16384]; sb_ngx_order_dump(rep, sizeof rep); app("%s", rep);
        return std::string(buf, n);
    }
    if (strncmp(path, "/ngxprefix", 10) == 0) {  // render only first N batches (draw order) then present
        int v = -1; if (const char* p = strstr(path, "n=")) v = atoi(p + 2);
        sb_ngx_set_prefix(v); app("ngx prefix_n = %d\n", v); return std::string(buf, n);
    }
    if (strncmp(path, "/ngxonly", 8) == 0) {   // runtime: render ONLY this tev_index (-2 env, -1 off)
        int t = -1; if (const char* p = strstr(path, "ti=")) t = atoi(p + 3);
        sb_ngx_set_onlyti(t); app("ngx only_ti = %d\n", t); return std::string(buf, n);
    }
    if (strncmp(path, "/ngxskip", 8) == 0 && path[8] != 's') {   // SKIP this tev_index (NOT /ngxskipset)
        int t = -1; if (const char* p = strstr(path, "ti=")) t = atoi(p + 3);
        sb_ngx_set_skipti(t); app("ngx skip_ti = %d\n", t); return std::string(buf, n);
    }
    if (strncmp(path, "/ngxskipset", 11) == 0) {  // skip a SET of tev_indices: ti=9,10,18 (empty = clear)
        sb_ngx_skipset_clear();
        if (const char* p = strstr(path, "ti=")) {
            p += 3; while (*p && *p != '&') { sb_ngx_skipset_add(atoi(p)); while (*p && *p!=',' && *p!='&') p++; if (*p==',') p++; }
        }
        app("ngx skipset updated\n"); return std::string(buf, n);
    }
    if (strncmp(path, "/ngxepoch", 9) == 0) {  // isolate an EFB-copy epoch: keep=N (only) or drop=N
        int keep = -1, drop = -1;
        if (const char* p = strstr(path, "keep=")) keep = atoi(p + 5);
        if (const char* p = strstr(path, "drop=")) drop = atoi(p + 5);
        sb_ngx_set_onlyepoch(keep); sb_ngx_set_dropepoch(drop);
        app("ngx epoch filter: keep=%d drop=%d\n", keep, drop); return std::string(buf, n);
    }
    if (strncmp(path, "/ngxrtfilter", 12) == 0) {  // render-target-aware present (ghost fix): on=0/1
        int v = 1; if (const char* p = strstr(path, "on=")) v = atoi(p + 3);
        sb_ngx_set_rtfilter(v); app("ngx rtfilter = %d (drop auxiliary offscreen epochs)\n", v);
        return std::string(buf, n);
    }
    if (strncmp(path, "/ngxfreeze", 10) == 0) { // FREEZE the published snapshot (deterministic A/B)
        int on = 1; if (const char* p = strstr(path, "on=")) on = atoi(p + 3);
        sb_ngx_set_freeze(on); app("ngx frozen = %d (snapshot latched; tweak modes/layers/blend on this frame)\n", sb_ngx_get_freeze());
        return std::string(buf, n);
    }
    if (strncmp(path, "/ngxnoblend", 11) == 0) {// force every material opaque (-1 per-mat, 0 opaque, 1 keep)
        int v = 0; if (const char* p = strstr(path, "on=")) v = atoi(p + 3);
        sb_ngx_set_noblend(v); app("ngx noblend = %d (0=force opaque, -1=per-material)\n", v);
        return std::string(buf, n);
    }
    if (strncmp(path, "/pixblend", 9) == 0) {  // CPU full-pipeline blend-stack replay (per-layer TEV+blend over the clear)
        float x = 0.f, y = -0.6f;
        if (const char* p = strstr(path, "x=")) x = strtof(p + 2, nullptr);
        if (const char* p = strstr(path, "y=")) y = strtof(p + 2, nullptr);
        static thread_local char rep[16384]; sb_ngx_pixel_blend(x, y, rep, sizeof rep); app("%s", rep);
        return std::string(buf, n);
    }
    if (strncmp(path, "/pixbatch", 9) == 0) {  // CPU pixel->batch raster probe (which captured batch covers a NDC point)
        float x = 0.f, y = 0.9f;   // default: a sky point (top-centre, GL NDC y up)
        if (const char* p = strstr(path, "x=")) x = strtof(p + 2, nullptr);
        if (const char* p = strstr(path, "y=")) y = strtof(p + 2, nullptr);
        static thread_local char rep[16384]; sb_ngx_pixel_batch(x, y, rep, sizeof rep); app("%s", rep);
        return std::string(buf, n);
    }
    if (strncmp(path, "/ngxpresentlive", 15) == 0) {  // N7 live present: renderer stats (SUNBRIGHT_NGX_PRESENT)
        unsigned long fr = 0, pi = 0, tx = 0, jq = 0; int w = 0, h = 0, ok = 0;
        sb_ngx_present_stats(&fr, &pi, &tx, &w, &h, &ok, &jq);
        app("ngx_present_live: init_ok=%d frames=%lu pipelines_built=%lu textures_decoded=%lu target=%dx%d hud_quads=%lu\n",
            ok, fr, pi, tx, w, h, jq);
        return std::string(buf, n);
    }
    if (strncmp(path, "/ngxpresent", 11) == 0) {  // N7 present primitive: render into an external target
        char rep[1024]; sb_ngx_present_test(rep, sizeof rep); app("%s", rep);
        return std::string(buf, n);
    }
    if (strncmp(path, "/ngxrender", 10) == 0) {  // N4 native Vulkan render of captured geometry
        char rep[1024]; sb_ngx_render(rep, sizeof rep); app("%s", rep);
        return std::string(buf, n);
    }
    if (strncmp(path, "/ngx", 4) == 0) {   // R1 native-GX-decoder parity vs oracle
        unsigned long frames = 0, mismatch = 0, first = 0; const char* what = "";
        gxs_ngx_parity_stats(&frames, &mismatch, &first, &what);
        app("ngx_parity frames=%lu mismatch=%lu first_frame=%lu first_what=%s\n"
            "verdict=%s\n",
            frames, mismatch, first, (what && *what) ? what : "-",
            (frames > 0 && mismatch == 0) ? "PARITY-OK" : (frames == 0 ? "NO-DATA" : "MISMATCH"));
        return std::string(buf, n);
    }
    if (strncmp(path, "/w?", 3) == 0) {
        // Diagnostic guest-memory poke: /w?a=HEX&v=HEX[&b=1 for byte] — hypothesis testing
        // (e.g. forcing a state byte to confirm a gate theory) without rebuild cycles.
        u32 a = qarg(path, "a", 0), v = qarg(path, "v", 0), byte = qarg(path, "b", 0);
        if (a >= 0x80000000u && a < 0x81800000u) {
            if (byte) mem_w8(a, (u8)v); else mem_w32(a, v);
            app("wrote %08x to %08x (%s)\n", v, a, byte ? "byte" : "word");
        } else app("refused: %08x not in RAM\n", a);
        return std::string(buf, n);
    }
    if (strncmp(path, "/aram", 5) == 0) {
        // ARAM content checker (instrument wave banks): /aram?a=<offset>&n=<bytes, hex> →
        // FNV-1a hash + nonzero count of the region. Compare oracle vs recomp uploads.
        u32 a = qarg(path, "a", 0), len = qarg(path, "n", 0x10000);
        if (len > 0x400000) len = 0x400000;
        const u8* p = Core::System::GetInstance().GetDSP().GetARAMPtr();
        if (!p) { app("no ARAM ptr\n"); return std::string(buf, n); }
        u32 h = 2166136261u; unsigned long nz = 0;
        for (u32 i = 0; i < len; i++) { const u8 b = p[a + i]; h = (h ^ b) * 16777619u; nz += b != 0; }
        app("aram a=%08x n=%x fnv=%08x nonzero=%lu\n", a, len, h, nz);
        return std::string(buf, n);
    }
    if (strncmp(path, "/shot", 5) == 0) {
        // Burst frame dump: /shot?on=1 enables Dolphin's image dump, /shot (or ?on=0)
        // disables it. Headless has no present path, so Core::SaveScreenShot never fires;
        // the dump path works headless. Short bursts replace SUNBRIGHT_DUMP for long
        // interactive drives — continuous PNG dumping falls behind the GPU and stalls VI
        // into a watchdog kill. Frames land in User/Dump/Frames/.
#ifdef HAVE_DOLPHIN_CORE
        const bool on = qarg(path, "on", 0) != 0;
        Config::SetCurrent(Config::MAIN_MOVIE_DUMP_FRAMES, on);
        Config::SetCurrent(Config::GFX_DUMP_FRAMES_AS_IMAGES, on);
        app("frame dump %s\n", on ? "ON" : "OFF");
#endif
        return std::string(buf, n);
    }
    if (strncmp(path, "/jas", 4) == 0) {
        // JASystem track-tree walk: master roots from the TrackMgr handle table
        // (ptr @r13-23296=0x8040E6C0, count @0x8040E6C8). Per track: BMS base/cursor/wait,
        // tempo (unk3B8), timing mode (unk3BD), active (unk3C4), tick accum/step (3AC/3B0),
        // port0 value (+0x68). Recurses children via unk2C4[16]. The silent-BGM scope.
        const u32 tbl = mem_r32(0x8040E6C0u), cnt = mem_r32(0x8040E6C8u);
        app("handle_table=%08x count=%u\n", tbl, cnt);
        // simple iterative DFS with explicit stack (depth, addr)
        struct Ent { u32 t; int d; };
        Ent stk[256]; int sp = 0;
        for (u32 h = 0; h < cnt && h < 8; h++) {
            u32 root = mem_r32(tbl + h * 4);
            if (root >= 0x80000000u && root < 0x81800000u) { stk[sp++] = { root, 0 }; }
        }
        int emitted = 0;
        while (sp > 0 && emitted < 120) {
            Ent e = stk[--sp];
            const u32 t = e.t;
            const u32 base = mem_r32(t), cur = mem_r32(t + 4), wait = mem_r32(t + 8);
            const u32 tempo = mem_r16(t + 0x3B8);
            const u32 b3bc = mem_r32(t + 0x3BC), b3c4 = mem_r32(t + 0x3C4);
            const u32 bd = (b3bc >> 16) & 0xff, act = (b3c4 >> 24) & 0xff;
            const u32 acc = mem_r32(t + 0x3AC), step = mem_r32(t + 0x3B0);
            const u32 p0 = mem_r16(t + 0x68);
            app("%*s%08x base=%08x cur=+%x wait=%d tempo=%u tmode=%u act=%u acc=%08x step=%08x port0=%04x\n",
                e.d * 2, "", t, base, cur - base, (int)wait, tempo, bd, act, acc, step, p0);
            emitted++;
            for (int i = 15; i >= 0 && sp < 250; i--) {
                u32 c = mem_r32(t + 0x2C4 + (u32)i * 4);
                if (c >= 0x80000000u && c < 0x81800000u && c != t) stk[sp++] = { c, e.d + 1 };
            }
        }
        return std::string(buf, n);
    }
    if (strncmp(path, "/census", 7) == 0) {
        // dump the dynamic call census (SUNBRIGHT_CALL_CENSUS=1) → call_census.tsv
        sb_census_dump("scratch/logs/call_census.tsv");
        app("census dumped to scratch/logs/call_census.tsv\n");
        return std::string(buf, n);
    }
    if (strncmp(path, "/frametime", 10) == 0) {
        // Inter-present wall-time jitter — the objective frame-delivery measure.
        // ?reset=1 zeroes the window first. Even 60fps = mean~16.7ms, low stddev.
        if (qarg(path, "reset", 0)) { gxs_frametime_reset(); app("reset\n"); return std::string(buf, n); }
        unsigned long fn2; double mean, sd, mn, mx;
        gxs_frametime_stats(&fn2, &mean, &sd, &mn, &mx);
        app("frametime: n=%lu mean=%.2fms stddev=%.2fms min=%.2fms max=%.2fms  fps=%.1f\n",
            fn2, mean, sd, mn, mx, mean > 0 ? 1000.0 / mean : 0.0);
        double dec_mean, dec_atmax; gxs_frametime_decode(&dec_mean, &dec_atmax);
        app("  GX decode (sync render on guest thread): mean=%.2fms/frame  at-slowest-frame=%.2fms\n",
            dec_mean, dec_atmax);
        double ct_mean, ct_atmax; gxs_frametime_ct(&ct_mean, &ct_atmax);
        app("  CoreTiming Advance+catchup: mean=%.2fms/frame  at-slowest-frame=%.2fms\n",
            ct_mean, ct_atmax);
        app("  -> if at-slowest-frame ~= max, the spikes are RENDER (own the GPU submission);\n"
            "     if decode is small vs max, the spikes are game logic / pacing.\n");
        return std::string(buf, n);
    }
    if (strncmp(path, "/interp60", 9) == 0) {
        // 60 fps interpolation data path: counters + the does-the-blend-reach-the-GPU
        // cross-check + live A/B controls (?alpha=<f> ?blend=<0|1> ?perturb=<0|1>).
        n += interp60_probe(buf + n, (int)sizeof(buf) - n, path);
        return std::string(buf, n);
    }
    if (strncmp(path, "/nativevi", 9) == 0) {
        // Native VI scan-out field-split accounting (pairs/single/reasserts, live top/bottom FBB).
        n += native_vi_probe(buf + n, (int)sizeof(buf) - n, path);
        return std::string(buf, n);
    }
    if (strncmp(path, "/njas", 5) == 0) {
        // Native JAS engine voice table (recomp side of the oracle A/B): srcHash joins
        // against /aram?a=<vpb base>&n=40 FNV on the oracle side. See tools/audio/vpb_compare.py.
        n += njas_probe(buf + n, (int)sizeof(buf) - n);
        return std::string(buf, n);
    }
    if (strncmp(path, "/vpb", 4) == 0) {
        // JAS DSP voice parameter blocks, read straight from guest RAM (CH_BUF global
        // 0x8040E5B8 → 64 × 0x180-byte DSPBuffer; layout = Dolphin Zelda VPB, BE u16s).
        // The ear-free voice probe: enabled/done flags + per-channel target/current volumes.
        const u32 base = mem_r32(0x8040E5B8u);
        app("CH_BUF=%08x\n", base);
        if (base >= 0x80000000u && base < 0x81800000u) {
            for (int v = 0; v < 64; v++) {
                const u32 b = base + (u32)v * 0x180u;
                const u16 en = mem_r16(b), done = mem_r16(b + 2);
                if (!en && !done) continue;
                app("v%02d en=%u done=%u ratio=%04x", v, en, done, mem_r16(b + 4));
                for (int c = 0; c < 6; c++) {
                    const u32 ch = b + 0x10u + (u32)c * 8u;   // channels[6]{id,tgt,cur,unk} u16s
                    const u16 id = mem_r16(ch);
                    if (id) app(" ch%04x=%d/%d", id, (s16)mem_r16(ch + 2), (s16)mem_r16(ch + 4));
                }
                if (mem_r16(b + 0x58u))                       // use_dolby_volume (u16 idx 0x2C)
                    app(" dolby=%d/%d", (s16)mem_r16(b + 0x54u), (s16)mem_r16(b + 0x56u));
                // wave source block (u16 idx 0x80+): type, loop flag, end_requested,
                // loop addr, base addr — the "notes die after one sample pass" probe.
                app(" src=%u loop=%u endreq=%u loopa=%04x%04x base=%04x%04x",
                    mem_r16(b + 0x100u), mem_r16(b + 0x102u), mem_r16(b + 0x10Au),
                    mem_r16(b + 0x110u), mem_r16(b + 0x112u),
                    mem_r16(b + 0x118u), mem_r16(b + 0x11Au));
                app(" pos=%u:%u\n", mem_r16(b + 0x68u), mem_r16(b + 0x6Au));
            }
        }
        return std::string(buf, n);
    }
    if (strncmp(path, "/nintr", 6) == 0) {     // native interrupt dispatch counters per source
        for (int i = 0; i < 32; i++)
            if (g_nintr_counts[i]) app("intr%d=%lu\n", i, g_nintr_counts[i]);
        return std::string(buf, n);
    }
    if (strncmp(path, "/drawsync", 9) == 0) {  // pollution/drawsync pipeline counters
        app("token_dispatches=%lu callbacks=%lu sleeps=%lu wakes=%lu vi_fields=%llu\n",
            g_ds_token_dispatches, g_ds_callbacks, g_ds_sleeps, g_ds_wakes, watchdog_vi_fields());
        return std::string(buf, n);
    }
    if (strncmp(path, "/fn?", 4) == 0) {
        u32 a = qarg(path, "a", 0);
        app("%08x  %s\n", a, sym(a).c_str());
        return std::string(buf, n);
    }
    if (strncmp(path, "/stack?", 7) == 0) {
        u32 fp = qarg(path, "sp", 0);
        app("guest stack from sp=%08x (back-chain + saved LR):\n", fp);
        for (int i = 0; i < 24 && fp >= 0x80000000u && fp < 0x81800000u; i++) {
            u32 lr = mem_r32(fp + 4);
            app("  [%2d] lr=%08x  %s\n", i, lr, sym(lr).c_str());
            u32 nx = mem_r32(fp);
            if (nx <= fp || nx < 0x80000000u || nx >= 0x81800000u) break;
            fp = nx;
        }
        return std::string(buf, n);
    }
    if (strncmp(path, "/cur", 4) == 0) {
        u32 cur = mem_r32(0x800000E4u);
        app("OS_CURRENT_THREAD = %08x\n", cur);
        if (cur >= 0x80000000u && cur < 0x81800000u) {
            // OSThread/OSContext: srr0 @ +0x198, lr @ +0x84, gpr1(sp) @ +0x4, state @ +0x2c8, prio @ +0x2d0
            u32 srr0 = mem_r32(cur + 0x198), lr = mem_r32(cur + 0x84), sp = mem_r32(cur + 0x4);
            app("  srr0=%08x  %s\n", srr0, sym(srr0).c_str());
            app("  lr  =%08x  %s\n", lr, sym(lr).c_str());
            app("  sp  =%08x  state=%u prio=%d\n", sp, mem_r32(cur + 0x2c8) & 0xffff, (int)mem_r32(cur + 0x2d0));
        }
        return std::string(buf, n);
    }
    if (strncmp(path, "/trace?", 7) == 0) {
        // Sample one guest word as fast as possible for `ms` (default 3000), report VALUE TRANSITIONS
        // (index, t_ms, old->new). Run in native AND pure-Dolphin (SUNBRIGHT_DISABLE_RECOMP=1), both
        // with SUNBRIGHT_PROBE=1, and diff the two transition sequences to see where they diverge.
        u32 a = qarg(path, "a", 0), ms = qarg_dec(path, "ms", 3000);
        if (ms > 15000) ms = 15000;   // single-threaded server: a long trace blocks every other probe
        auto t0 = std::chrono::steady_clock::now();
        u32 last = mem_r32(a); long samples = 0; int trans = 0;
        app("trace %08x for %u ms:\n", a, ms);
        app("  [%6ld] t=%5dms  start=%08x\n", 0L, 0, last);
        for (;;) {
            u32 v = mem_r32(a); samples++;
            if (v != last) {
                int t = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
                if (trans < 200) app("  [%6ld] t=%5dms  %08x -> %08x\n", samples, t, last, v);
                last = v; trans++;
            }
            if ((samples & 0x3fff) == 0 &&
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() >= (long)ms)
                break;
        }
        app("  done: %ld samples, %d transitions, final=%08x\n", samples, trans, last);
        return std::string(buf, n);
    }
    if (strncmp(path, "/pad?", 5) == 0) {
        // Inject a scripted pad action: /pad?do=<combo>&ms=<hold-ms> — same grammar as the
        // SUNBRIGHT_REPL fifo (combo = a|b|x|y|z|start|l|r|up|down|left|right joined by '+',
        // 'wait' to idle). Queued; the main loop holds the bits for ms. e.g. /pad?do=up&ms=2000
        char combo[64] = {0}; u32 ms = qarg_dec(path, "ms", 150);
        if (const char* p = strstr(path, "do=")) {
            size_t i = 0; p += 3;
            while (*p && *p != '&' && i + 1 < sizeof combo) combo[i++] = *p++;
        }
        if (!combo[0]) return std::string("usage: /pad?do=<combo>&ms=<ms>\n");
        char line[96]; snprintf(line, sizeof line, "%s %u", combo, ms);
        sunbright_repl_inject(line);
        app("queued: %s\n", line);
        return std::string(buf, n);
    }
#ifdef HAVE_DOLPHIN_CORE
    if (strncmp(path, "/abshot", 7) == 0 && path[7] != '2') {
        // Zero-drift A/B: in NGX_PRESENT mode Dolphin's GX pipeline still renders the XFB
        // each frame AND ngx renders its own texture for the SAME frame. Capture BOTH from
        // ONE process by toggling the present source around two screenshots (~1 frame apart,
        // identical present/readback path) → foo.gx.png (Dolphin GX render) + foo.ngx.png
        // (native render). With DBG_RASCOLOR + NGX_TEVDBG=ras set this isolates lighting;
        // with no debug env it compares the full render. No second process, no fastboot/
        // shader-cache drift. /abshot?name=foo
        if (!g_frame_dumper) return std::string("no frame dumper\n");
        char name[64] = {0};
        if (const char* p = strstr(path, "name=")) {
            size_t i = 0; p += 5;
            while (*p && *p != '&' && *p != ' ' && i + 1 < sizeof name) {
                char c = *p++; name[i++] = (c=='/'||c=='\\') ? '_' : c;
            }
        }
        if (!name[0]) snprintf(name, sizeof name, "ab_%llu",
                               (unsigned long long)std::chrono::duration_cast<std::chrono::seconds>(
                                   std::chrono::system_clock::now().time_since_epoch()).count());
        mkdir("scratch", 0755); mkdir("scratch/screenshots", 0755);
        const int saved = g_sb_ngx_present;
        auto grab = [&](const char* suffix, int present_mode) -> long long {
            g_sb_ngx_present = present_mode;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let one present settle the toggle
            char full[256]; snprintf(full, sizeof full, "scratch/screenshots/%s.%s.png", name, suffix);
            ::unlink(full);
            g_frame_dumper->SaveScreenshot(full);
            struct stat st{};
            for (int i = 0; i < 300; i++) {
                if (stat(full, &st) == 0 && st.st_size > 0) return (long long)st.st_size;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return -1;
        };
        long long gx = grab("gx", 0);     // Dolphin GX XFB
        long long ng = grab("ngx", 1);    // native ngx texture
        g_sb_ngx_present = saved;
        app("abshot %s: gx=%lld ngx=%lld bytes\n", name, gx, ng);
        return std::string(buf, n);
    }
    if (strncmp(path, "/abshot2", 8) == 0) {
        // TRUE zero-drift A/B: arm a SAME-PRESENT dual capture in Present.cpp. Writes
        // scratch/screenshots/ab2.gx.ppm (Dolphin GX XFB) + ab2.ngx.ppm (native ngx) from
        // the identical present → pixel-perfect camera alignment. Requires NGX_PRESENT mode.
        mkdir("scratch", 0755); mkdir("scratch/screenshots", 0755);
        ::unlink("scratch/screenshots/ab2.gx.ppm");
        ::unlink("scratch/screenshots/ab2.ngx.ppm");
        g_sb_ab_capture = 1;
        struct stat st1{}, st2{}; bool ok = false;
        for (int i = 0; i < 400; i++) {  // up to ~4s
            if (stat("scratch/screenshots/ab2.gx.ppm", &st1) == 0 && st1.st_size > 0 &&
                stat("scratch/screenshots/ab2.ngx.ppm", &st2) == 0 && st2.st_size > 0) { ok = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // Self-certify: report the published ngx snapshot's frame id. Both PPMs come from ONE
        // Present.cpp ProcessFrameDumping call (atomic same present); the GX XFB and the ngx
        // snapshot are the same frame in steady state. If two successive /abshot2 calls show the
        // SAME ngx_frame, the snapshot is stale (a no-3D frame kept the last buffer) — distrust it.
        app("abshot2 %s: gx=%lld ngx=%lld bytes  ngx_frame=%lu  (single core, untainted GX oracle, same present)\n",
            ok ? "saved" : "TIMEOUT", (long long)st1.st_size, (long long)st2.st_size, sb_ngx_front_frame());
        return std::string(buf, n);
    }
    if (strncmp(path, "/screenshot", 11) == 0) {
        // On-demand PNG of the current presented frame (the XFB), serviced on the next
        // present by Dolphin's FrameDumper (works headless — the readback path runs
        // regardless of swapchain). /screenshot?name=foo -> scratch/screenshots/foo.png
        if (!g_frame_dumper) return std::string("no frame dumper\n");
        char name[64] = {0};
        if (const char* p = strstr(path, "name=")) {
            size_t i = 0; p += 5;
            while (*p && *p != '&' && *p != ' ' && i + 1 < sizeof name) {
                char c = *p++;
                name[i++] = (c=='/'||c=='\\') ? '_' : c;   // no path escapes
            }
        }
        if (!name[0]) snprintf(name, sizeof name, "shot_%llu",
                               (unsigned long long)std::chrono::duration_cast<std::chrono::seconds>(
                                   std::chrono::system_clock::now().time_since_epoch()).count());
        mkdir("scratch", 0755); mkdir("scratch/screenshots", 0755);
        char full[256]; snprintf(full, sizeof full, "scratch/screenshots/%s.png", name);
        ::unlink(full);
        g_frame_dumper->SaveScreenshot(full);
        // Wait for the dumper thread to write the file (serviced within a frame or two).
        struct stat st{}; bool ok = false;
        for (int i = 0; i < 200; i++) {            // up to ~2 s
            if (stat(full, &st) == 0 && st.st_size > 0) { ok = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        app("%s %s (%lld bytes)\n", ok ? "saved" : "TIMEOUT", full, (long long)st.st_size);
        return std::string(buf, n);
    }
#endif
    if (strncmp(path, "/verify", 7) == 0) {
        // 60fps interpolation VISUAL midpoint check (interp_verify.cpp). /verify?n=K arms a capture
        // of K unique presents, waits for it to fill, and prints the midpoint analysis; /verify with
        // no n just reports the current ring. Drive motion first, e.g.
        //   curl '/pad?do=cright&ms=3000' &   then   curl '/verify?n=24'
        const u32 want = qarg_dec(path, "n", 0);
        if (want) {
            interp_verify_arm((int)want);
            // Small captures: block until done (one-call convenience). Large captures (a long walk):
            // arm and return immediately — poll /verify (no n) for "armed remaining=0".
            if (want <= 64) {
                for (int i = 0; i < 600 && sb_capture_frames > 0; i++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } else {
                app("armed %u-frame capture (full-res to scratch/verify) — poll /verify for remaining\n", want);
                return std::string(buf, n);
            }
        }
        n = interp_verify_report(buf, (int)sizeof buf);
        return std::string(buf, n);
    }
    if (strncmp(path, "/poll?", 6) == 0) {
        // One-shot snapshot of up to 6 addresses (a,b,c,d,e,f as hex), each named — a compact
        // "compare these key cells" line for A/B between native and Dolphin runs.
        const char* keys = "abcdef";
        for (int i = 0; keys[i]; i++) {
            char k[2] = {keys[i], 0};
            u32 a = qarg(path, k, 0);
            if (!a) continue;
            u32 v = mem_r32(a);
            app("%c: [%08x]=%08x  (%s)\n", keys[i], a, v, sym(v).c_str());
        }
        return std::string(buf, n);
    }
    if (strncmp(path, "/tracelog", 9) == 0) {
        // Dump the trace ring in chronological order, each event's tag + 4 named hex args.
        // /tracelog?s=<startseq>&n=<count> (decimal) windows the dump — the full 8192-entry ring
        // exceeds the response buffer, so page through it.
        uint64_t end = g_trace_seq.load(std::memory_order_relaxed);
        uint64_t start = end > SB_TRACE_N ? end - SB_TRACE_N : 0;
        if (uint64_t s_arg = qarg_dec(path, "s", 0); s_arg > start && s_arg < end) start = s_arg;
        if (uint64_t n_arg = qarg_dec(path, "n", 700); end - start > n_arg) end = start + n_arg;
        app("tracelog: %llu events (showing %llu..%llu)\n",
            (unsigned long long)end, (unsigned long long)start, (unsigned long long)end);
        for (uint64_t s = start; s < end; s++) {
            const TraceRec& r = g_trace[s % SB_TRACE_N];
            if (r.seq != s) continue;   // overwritten mid-read
            app("  #%-6llu %-12s a=%08x b=%08x c=%08x d=%08x\n",
                (unsigned long long)r.seq, r.tag, r.a, r.b, r.c, r.d);
        }
        return std::string(buf, n);
    }
    if (strncmp(path, "/help", 5) == 0 || strcmp(path, "/") == 0) {
        return "sunbright REPL (curl http://127.0.0.1:17654<path>):\n"
               "  /metrics            perf counters (JSON)\n"
               "  /r?a=HEX&n=N        read N words at guest addr (default 8)\n"
               "  /fn?a=HEX           resolve addr -> nearest function name\n"
               "  /stack?sp=HEX       walk guest back-chain LRs from sp, named\n"
               "  /cur                current OSThread + saved srr0/lr/sp/prio\n"
               "  /trace?a=HEX&ms=N   sample a word for N ms, list value transitions (A/B Dolphin vs native)\n"
               "  /poll?a=HEX&b=..    snapshot up to 6 cells (a..f), each named\n"
               "  /tracelog           dump the trace ring (events from sb_trace observers)\n"
               "  /interp60[?alpha=&blend=&perturb=]  60fps data path + live A/B controls\n";
    }
    return std::string();
}
#endif

using clock_t_ = std::chrono::steady_clock;

clock_t_::time_point g_start;

// Snapshot of counters + wall time at the previous /metrics request, so each response can
// report per-second RATES (the diagnostic that matters) without the client doing math.
struct Snap {
    uint64_t recomp, interp, native_os, tail, steps, poll;
    double   t;   // seconds since start
};
std::mutex g_snap_mtx;
Snap g_last{};
bool g_have_last = false;

double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
}

// Build the /metrics JSON body.
std::string build_metrics() {
    const double t = now_s();

    const uint64_t recomp    = g_probe.call_recomp.load(std::memory_order_relaxed);
    const uint64_t interp    = g_probe.call_interp.load(std::memory_order_relaxed);
    const uint64_t native_os = g_probe.call_native_os.load(std::memory_order_relaxed);
    const uint64_t tail      = g_probe.tail.load(std::memory_order_relaxed);
    const uint64_t steps     = g_probe.interp_steps.load(std::memory_order_relaxed);
    const uint64_t poll      = g_probe.poll_yield.load(std::memory_order_relaxed);
    const uint64_t interp_ns = g_probe.interp_ns.load(std::memory_order_relaxed);
    const double   interp_frac = t > 1e-6 ? (double(interp_ns) / 1e9) / t : 0.0;  // share of wall in interpreter

    // Rates since the previous probe.
    double dt = 0, r_recomp = 0, r_interp = 0, r_native = 0, r_tail = 0, r_steps = 0, r_poll = 0;
    {
        std::lock_guard<std::mutex> lk(g_snap_mtx);
        if (g_have_last) {
            dt = t - g_last.t;
            if (dt > 1e-6) {
                r_recomp = (recomp    - g_last.recomp)    / dt;
                r_interp = (interp    - g_last.interp)    / dt;
                r_native = (native_os - g_last.native_os) / dt;
                r_tail   = (tail      - g_last.tail)      / dt;
                r_steps  = (steps     - g_last.steps)     / dt;
                r_poll   = (poll      - g_last.poll)      / dt;
            }
        }
        g_last = {recomp, interp, native_os, tail, steps, poll, t};
        g_have_last = true;
    }

    double fps = 0, vps = 0, speed = 0, max_speed = 0, emu_secs = 0;
    bool core_running = false;
#ifdef HAVE_DOLPHIN_CORE
    auto& sys = Core::System::GetInstance();
    core_running = (Core::GetState(sys) == Core::State::Running);
    // Perf metrics are safe to read from any thread (atomics inside).
    auto& pm = sys.GetPerfMetrics();
    fps = pm.GetFPS();
    vps = pm.GetVPS();
    speed = pm.GetSpeed();
    max_speed = pm.GetMaxSpeed();
    if (core_running) {
        const u64 ticks = sys.GetCoreTiming().GetTicks();
        const u32 tps   = sys.GetSystemTimers().GetTicksPerSecond();
        if (tps) emu_secs = double(ticks) / double(tps);
    }
#endif

    char buf[2048];
    int n = snprintf(buf, sizeof buf,
        "{\n"
        "  \"uptime_s\": %.3f,\n"
        "  \"window_s\": %.3f,\n"
        "  \"core_running\": %s,\n"
        "  \"emu_secs\": %.3f,\n"
        "  \"dolphin\": { \"fps\": %.2f, \"vps\": %.2f, \"speed\": %.4f, \"max_speed\": %.4f },\n"
        "  \"calls_total\": { \"recomp\": %llu, \"interp\": %llu, \"native_os\": %llu, \"tail\": %llu, \"interp_steps\": %llu, \"poll_yield\": %llu },\n"
        "  \"calls_per_s\": { \"recomp\": %.0f, \"interp\": %.0f, \"native_os\": %.0f, \"tail\": %.0f, \"interp_steps\": %.0f, \"poll_yield\": %.1f },\n"
        "  \"interp_wall_frac\": %.4f\n"
        "}\n",
        t, dt, core_running ? "true" : "false", emu_secs,
        fps, vps, speed, max_speed,
        (unsigned long long)recomp, (unsigned long long)interp, (unsigned long long)native_os,
        (unsigned long long)tail, (unsigned long long)steps, (unsigned long long)poll,
        r_recomp, r_interp, r_native, r_tail, r_steps, r_poll,
        interp_frac);
    return std::string(buf, n > 0 ? (size_t)n : 0);
}

void serve_conn(int fd) {
    // The server is single-threaded: a client that connects but never sends (e.g. a curl
    // killed between connect and write) must not park the whole probe in recv() forever.
    timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    // ...and a client that times out mid-transfer (curl -m / --max-time) and stops reading
    // must not park the probe in send() forever: with no SO_SNDTIMEO, a large body (/ngxshape
    // 16KB, /tevshader 64KB) fills the socket send buffer and send() blocks indefinitely once
    // the peer's window closes, wedging EVERY later probe (this repeatedly cost prior sessions).
    timeval stv{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof stv);
    char req[1024] = {0};
    (void)recv(fd, req, sizeof req - 1, 0);

    // Parse "GET <path> HTTP/1.1" → route. /metrics (default) + the REPL inspection endpoints.
    std::string body;
    char path[512] = "/metrics";
    if (sscanf(req, "%*s %511s", path) == 1) {}
#ifdef HAVE_DOLPHIN_CORE
    if (strncmp(path, "/metrics", 8) != 0) {
        body = handle_repl(path);
        if (body.empty()) body = "unknown path; try /help\n";
    } else
#endif
        body = build_metrics();
    char hdr[256];
    int hn = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Content-Length: %zu\r\n\r\n",
        body.size());
    // MSG_NOSIGNAL: a client that timed out and closed (curl -m) must not SIGPIPE-kill the
    // whole emulator — long endpoints (/trace) regularly outlive the client.
    (void)send(fd, hdr, (size_t)hn, MSG_NOSIGNAL);
    (void)send(fd, body.data(), body.size(), MSG_NOSIGNAL);
    close(fd);
}

void server_loop(int port) {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("[probe] socket"); return; }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 127.0.0.1 only — never expose externally
    addr.sin_port = htons((uint16_t)port);
    if (bind(srv, (sockaddr*)&addr, sizeof addr) < 0) {
        fprintf(stderr, "[probe] bind :%d failed: %s\n", port, strerror(errno));
        close(srv);
        return;
    }
    if (listen(srv, 8) < 0) { perror("[probe] listen"); close(srv); return; }
    fprintf(stderr, "[probe] HTTP probe on http://127.0.0.1:%d/metrics\n", port);

    for (;;) {
        int fd = accept(srv, nullptr, nullptr);
        if (fd < 0) { if (errno == EINTR) continue; break; }
        serve_conn(fd);
    }
    close(srv);
}

}  // namespace

void probe_server_start() {
    static bool started = false;
    if (started) return;
    if (!getenv("SUNBRIGHT_PROBE")) return;
    started = true;
    g_probe_enabled = true;
    g_start = std::chrono::steady_clock::now();
    int port = 17654;
    if (const char* p = getenv("SUNBRIGHT_PROBE_PORT")) { int v = atoi(p); if (v > 0) port = v; }
    std::thread(server_loop, port).detach();
}
