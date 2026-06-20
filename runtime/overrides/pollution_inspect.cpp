// Pollution-coverage INSPECTION tee (verification harness for the Sirena-goo native port).
//
// The "Manta Storm" goo is a GPU feedback coverage texture produced by
// TPollutionCounterLayer::countTexDegree (0x8019b3a0). Under ngx present Dolphin's EFB is empty, so
// the coverage never gets produced → invisible goo. Before porting that render natively we must
// confirm, on real data, WHICH draw path produces the goo and WHAT the task queues hold.
//
// This override (gated on SUNBRIGHT_DBG_POLL; zero impact when off — the original goes straight to
// Dolphin's JIT) tees countTexDegree, reads gpPollution's counter-layer state straight from guest
// RAM (object model, not Dolphin), dumps it, then runs the original so behaviour is unchanged.
//
// Counter layer = `this` (gpr[3]); gpPollution = this - 0x70 (TPollutionManager.unk70). Layout from
// reference/sms include/Map/{PollutionCount,PollutionLayer,Manager}.hpp + ResTIMG.hpp.
#include "../overrides.h"
#include "../cpu_state.h"
#include "../../runtime/ngx/ngx_pollution.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <unordered_map>

extern u8  mem_r8(u32 ea);
extern u16 mem_r16(u32 ea);
extern u32 mem_r32(u32 ea);
extern "C" void sb_ngx_efb_store_copy(uint32_t ea, int w, int h, const uint32_t* argb);
extern "C" void sb_ngx_efb_invalidate_tex(uint32_t ea);

namespace {

bool dbg() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("SUNBRIGHT_DBG_POLL"); v = (e && *e && *e != '0') ? 1 : 0; }
    return v;
}

// TPollutionCounterLayer field offsets (PollutionCount.hpp)
constexpr u32 CL_unk8   = 0x08;   // int  layer count
constexpr u32 CL_unk14  = 0x14;   // const TPollutionLayer** layer array
constexpr u32 CL_unk1A  = 0x1A;   // u16  tex-stamp count
constexpr u32 CL_unk1C  = 0x1C;   // TPollutionTexStamp* array
constexpr u32 CL_unk22  = 0x22;   // u16  revival-tex-stamp count
constexpr u32 CL_unk28  = 0x28;   // u16  model-stamp count
constexpr u32 CL_unkD4  = 0xD4;   // u16  joint-obj-stamp task count
constexpr u32 CL_unk178 = 0x178;  // u8*  per-layer active flag

// TPollutionLayer field offsets (PollutionLayer.hpp)
constexpr u32 L_unk30 = 0x30;   // u16  pollution type
constexpr u32 L_unk32 = 0x32;   // u16  flags
constexpr u32 L_unk54 = 0x54;   // u8*  coverage image data (= unk58 + imageDataOffset)
constexpr u32 L_unk58 = 0x58;   // ResTIMG*
inline u32 L_unk32_off() { return L_unk32; }

// TPollutionTexStamp (0x14 bytes): unk0 type, unk4 ResTIMG*, unk8 task count, unk10 task array
constexpr u32 TS_stride = 0x14;

// GC-tiled depth-map index (TPollutionPos::index): 8x4 blocks, row stride 1<<(unk8) cols.
inline u32 pp_index(int x, int y, int unk8) {
    return (u32)((y & 3) * 8 + (((x >> 3) + ((y >> 2) << (unk8 - 3))) * 0x20) + (x & 7));
}

// One-shot: de-tile the layer's pollution depth map (unk5C.mMap) → scratch PGM, so we can SEE the
// polluted region shape and compare it to the goo. unk5C is at layer+0x5C; mMap at unk5C+0x1C.
void dump_depthmap(u32 lay, int idx) {
    u32 pos   = lay + 0x5C;
    int w     = (int)mem_r32(pos + 0x0);
    int h     = (int)mem_r32(pos + 0x4);
    int unk8  = (int)mem_r32(pos + 0x8);
    u32 mMap  = mem_r32(pos + 0x1C);
    fprintf(stderr, "[poll] depthmap layer[%d] %dx%d unk8=%d mMap=%08x\n", idx, w, h, unk8, mMap);
    if (!mMap || w <= 0 || h <= 0 || w > 2048 || h > 2048) return;
    int nz = 0, nprohib = 0;
    char path[128]; snprintf(path, sizeof path, "scratch/bin/pollution_depth_%d.pgm", idx);
    FILE* f = fopen(path, "wb");
    if (f) fprintf(f, "P5\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            u8 d = mem_r8(mMap + pp_index(x, y, unk8));
            if (d != 0 && d != 0xff) nz++;
            if (d == 0xff) nprohib++;
            if (f) fputc(d, f);
        }
    if (f) fclose(f);
    fprintf(stderr, "[poll] depthmap[%d] polluted(0<d<255)=%d prohibit(255)=%d → %s\n", idx, nz, nprohib, path);
}

// Dump the live coverage (unk54, I8, same 8x4 tiling as the depth map) → PGM, to compare vs depth.
void dump_coverage(u32 lay, int idx) {
    u32 pos   = lay + 0x5C;
    int w     = (int)mem_r32(pos + 0x0);
    int h     = (int)mem_r32(pos + 0x4);
    int unk8  = (int)mem_r32(pos + 0x8);
    u32 cov   = mem_r32(lay + L_unk54);
    if (!cov || w <= 0 || h <= 0 || w > 2048 || h > 2048) return;
    int nz = 0; long sum = 0;
    char path[128]; snprintf(path, sizeof path, "scratch/bin/pollution_cov_%d.pgm", idx);
    FILE* f = fopen(path, "wb");
    if (f) fprintf(f, "P5\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            u8 d = mem_r8(cov + pp_index(x, y, unk8));
            if (d) { nz++; sum += d; }
            if (f) fputc(d, f);
        }
    if (f) fclose(f);
    fprintf(stderr, "[poll] coverage[%d] nonzero=%d mean=%ld → %s\n", idx, nz, nz ? sum/nz : 0, path);
}

// Timestamped coverage dump → scratch/bin/pollution_cov_<idx>_t<frame>.pgm (evolution snapshots).
void dump_coverage_t(u32 lay, int idx, int frame) {
    u32 pos  = lay + 0x5C;
    int w    = (int)mem_r32(pos + 0x0), h = (int)mem_r32(pos + 0x4), unk8 = (int)mem_r32(pos + 0x8);
    u32 cov  = mem_r32(lay + L_unk54);
    if (!cov || w <= 0 || h <= 0 || w > 2048 || h > 2048) return;
    char path[160]; snprintf(path, sizeof path, "scratch/bin/pollution_cov_%d_t%d.pgm", idx, frame);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) fputc(mem_r8(cov + pp_index(x, y, unk8)), f);
    fclose(f);
}

void dump(u32 cl) {
    u32 gp   = cl - 0x70;
    int n    = (int)mem_r32(cl + CL_unk8);
    u32 arr  = mem_r32(cl + CL_unk14);
    u32 actA = mem_r32(cl + CL_unk178);
    u32 jcnt = mem_r16(cl + CL_unkD4);
    u32 tcnt = mem_r16(cl + CL_unk1A);
    u32 rcnt = mem_r16(cl + CL_unk22);
    u32 mcnt = mem_r16(cl + CL_unk28);
    fprintf(stderr, "[poll] gpPollution=%08x counterLayer=%08x layers=%d  tasks: joint=%u tex=%u revival=%u model=%u\n",
            gp, cl, n, jcnt, tcnt, rcnt, mcnt);
    for (int i = 0; i < n && i < 16; i++) {
        u32 lay = mem_r32(arr + 4u * i);
        u8  act = lay ? mem_r8(actA + i) : 0;
        if (!lay) { fprintf(stderr, "  [%d] layer=NULL\n", i); continue; }
        u32 type = mem_r16(lay + L_unk30);
        u32 ea54 = mem_r32(lay + L_unk54);
        u32 img  = mem_r32(lay + L_unk58);
        u32 w = 0, h = 0, fmt = 0, ido = 0;
        if (img) { fmt = mem_r8(img); w = mem_r16(img + 2); h = mem_r16(img + 4); ido = mem_r32(img + 0x1C); }
        fprintf(stderr, "  [%d] act=%u type=%u img=%08x %ux%u fmt=%u ido=%x  unk54(cov)=%08x\n",
                i, act, type, img, w, h, fmt, ido, ea54);
    }
    // tex-stamp tasks (the most likely goo producer): per stamp, type / ResTIMG / live task count
    u32 ts = mem_r32(cl + CL_unk1C);
    for (u32 i = 0; ts && i < tcnt && i < 8; i++) {
        u32 e = ts + i * TS_stride;
        fprintf(stderr, "    texstamp[%u] type=%u img=%08x ntasks=%u cap=%u\n",
                i, mem_r16(e + 0), mem_r32(e + 4), mem_r32(e + 8), mem_r32(e + 0xC));
    }
}

}  // namespace

// FALSIFIABLE TEST (SUNBRIGHT_POLL_FORCE): is the Sirena goo a coverage-masked plane sampling unk54?
// If forcing full coverage into ngx's side buffer for the layer's unk54 EA makes the goo appear, the
// theory is confirmed and the remaining work is producing the REAL coverage. Temporary diagnostic.
bool force() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("SUNBRIGHT_POLL_FORCE"); v = (e && *e && *e != '0') ? 1 : 0; }
    return v;
}

void force_full_coverage(u32 cl) {
    int n = (int)mem_r32(cl + CL_unk8);
    u32 arr = mem_r32(cl + CL_unk14);
    for (int i = 0; i < n; i++) {
        u32 lay = mem_r32(arr + 4u * i);
        if (!lay) continue;
        u32 img = mem_r32(lay + L_unk58);
        u32 ea  = mem_r32(lay + L_unk54);
        if (!img || !ea) continue;
        int w = mem_r16(img + 2), h = mem_r16(img + 4);
        if (w <= 0 || h <= 0 || w > 1024 || h > 1024) continue;
        static std::vector<uint32_t> buf;
        buf.assign((size_t)w * h, 0xFFFFFFFFu);
        sb_ngx_efb_store_copy(ea, w, h, buf.data());
        sb_ngx_efb_invalidate_tex(ea);
    }
}

// ── NATIVE COVERAGE PRODUCER (the real port of countTexDegree's feedback) ──────────────────────────
// Default-on (disable with SUNBRIGHT_NO_POLLUTION). Reproduces the per-layer coverage texture natively
// into ngx's side buffer so the goo plane (which samples `unk54` as its I8 mask) shows under ngx present,
// where Dolphin's EFB — and thus the GX_CTF_R8 feedback copy — is discarded. Verified target: the
// DUMP_TEX baseline oracle (Sirena Manta-Storm steady state = uniform saturation, mean 254.7).
bool produce() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("SUNBRIGHT_NO_POLLUTION"); v = (e && *e && *e != '0') ? 0 : 1; }
    return v;
}
// Only push to the side buffer when ngx is actually presenting (honor the VALUE, not mere presence —
// same rule as main_sdl.cpp). Under the NGX_PRESENT=0 baseline the guest GX path fills the real EFB and
// the A/B oracle must see an unmodified render, so the producer stays inert there.
bool present() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("SUNBRIGHT_NGX_PRESENT"); v = (e && atoi(e) != 0) ? 1 : 0; }
    return v;
}

// Per-coverage-EA host buffer (linear, row-major u8 intensity) + seeded flag.
struct CovBuf { std::vector<uint8_t> px; int w = 0, h = 0; bool seeded = false; };
std::unordered_map<u32, CovBuf>& cov_bufs() { static std::unordered_map<u32, CovBuf> m; return m; }

// Produce one layer's coverage for this countTexDegree(param_1=idx) call.
void produce_coverage(u32 cl, u32 idx) {
    if (mem_r8(mem_r32(cl + CL_unk178) + idx) == 0) return;   // inactive layer (unk178[idx]==0)
    u32 arr = mem_r32(cl + CL_unk14);
    u32 lay = mem_r32(arr + 4u * idx);
    if (!lay) return;
    u32 img = mem_r32(lay + L_unk58);
    u32 cov = mem_r32(lay + L_unk54);
    if (!img || !cov) return;
    int w = mem_r16(img + 2), h = mem_r16(img + 4);
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) return;

    u16 type = mem_r16(lay + L_unk30), flags = mem_r16(lay + L_unk32);
    u8  c0   = mem_r8(lay + 0x84),    c1    = mem_r8(lay + 0x85);
    u32 pos  = lay + 0x5C;
    int unk8 = (int)mem_r32(pos + 0x8);
    u32 mMap = mem_r32(pos + 0x1C);

    CovBuf& cb = cov_bufs()[cov];
    if (!cb.seeded || cb.w != w || cb.h != h) {
        cb.w = w; cb.h = h; cb.px.assign((size_t)w * h, 0);
        if (type == 4) {
            // STOPGAP (faithful seed): the Manta-Storm beach starts FULLY polluted — the boss pre-pollutes
            // it at scenario load, in boss/event logic not yet ported. Reproduce that load-time state by
            // saturating the polluted region of the layer's own area/depth map (real game data, GC-tiled).
            // Matches the DUMP_TEX oracle steady state; the per-frame feedback below then HOLDS it (and
            // would erode any region a future clean-stamp drops below the threshold).
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    u8 depth = mMap ? mem_r8(mMap + sb_pollution::tiled_index(x, y, unk8)) : 0;
                    cb.px[(size_t)y * w + x] = depth ? 254 : 0;
                }
        } else {
            // Other pollution types (graffiti etc.): seed from the guest's real coverage RAM (de-tiled).
            // Zero under ngx present until the stamp-rasterization path lands — leaves those scenes
            // (plaza/airstrip) untouched (no nonzero push below).
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                    cb.px[(size_t)y * w + x] = mem_r8(cov + sb_pollution::tiled_index(x, y, unk8));
        }
        cb.seeded = true;
        if (dbg()) {
            int snz = 0; for (auto v : cb.px) if (v) snz++;
            fprintf(stderr, "[poll-produce] seed EA=%08x %dx%d type=%u flags=%u c0=%u c1=%u unk8=%d mMap=%08x seed_nz=%d\n",
                    cov, w, h, type, flags, c0, c1, unk8, mMap, snz);
        }
    }

    // Per-frame feedback (the real countTexDegree drawTex TEV), per texel. TODO: rasterize the active
    // stamps (drawTexStamp/doTask, drawJointObjStamp, model/revival) for dynamic pollute/clean; idle
    // Manta-Storm has none (verified tex/joint/revival/model = 0 every frame).
    bool any = false;
    for (auto& v : cb.px) { v = sb_pollution::feedback_step(v, type, flags, c0, c1); if (v) any = true; }
    if (!any) return;
    // NB: push + invalidate EVERY frame — the texcache entry for this EA is keyed by addr and a skipped
    // invalidate lets the goo-plane bind fall back to (empty) guest RAM, dropping the goo. (A "skip when
    // unchanged" optimization was tried and broke the render; the per-frame 512² re-decode is the cost.)

    static std::vector<uint32_t> argb;
    argb.resize((size_t)w * h);
    for (size_t i = 0; i < cb.px.size(); i++) {
        uint32_t v = cb.px[i];
        argb[i] = (v << 24) | (v << 16) | (v << 8) | v;   // I8 → (v,v,v,v) replicated mask
    }
    sb_ngx_efb_store_copy(cov, w, h, argb.data());
    sb_ngx_efb_invalidate_tex(cov);

    if (dbg()) {
        static int pushes = 0;
        if (++pushes == 120) {
            int nz = 0; long s = 0; for (auto v : cb.px) if (v) { nz++; s += v; }
            fprintf(stderr, "[poll-produce] push#%d EA=%08x host nz=%d mean=%ld\n", pushes, cov, nz, nz ? s/nz : 0);
            char p[96]; snprintf(p, sizeof p, "scratch/bin/pollution_host_%08x.pgm", cov);
            FILE* f = fopen(p, "wb");
            if (f) { fprintf(f, "P5\n%d %d\n255\n", w, h); for (auto v : cb.px) fputc(v, f); fclose(f); }
        }
    }
}

// EVOLUTION trace: for the layer being counted this call, full-scan its coverage (count, mean,
// count>=50) + the per-call feedback parameters (unk30 type / unk32 flags / unk84 C0 / unk85 C1) +
// summed live task counts across ALL tex-stamps. Logged every frame for the first N frames so the
// seed/flood vs steady-state is unambiguous. de-tiled dump at a few timestamps.
void trace_layer(u32 cl, u32 idx, u64 calls) {
    u32 arr = mem_r32(cl + CL_unk14);
    u32 lay = mem_r32(arr + 4u * idx);
    if (!lay) return;
    u32 pos  = lay + 0x5C;
    int w    = (int)mem_r32(pos + 0x0), h = (int)mem_r32(pos + 0x4), unk8 = (int)mem_r32(pos + 0x8);
    u32 cov  = mem_r32(lay + L_unk54);
    if (!cov || w <= 0 || h <= 0 || w > 2048 || h > 2048) return;
    int nz = 0, ge50 = 0; long sum = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            u8 d = mem_r8(cov + pp_index(x, y, unk8));
            if (d) { nz++; sum += d; }
            if (d >= 50) ge50++;
        }
    u32 type = mem_r16(lay + L_unk30), flags = mem_r16(lay + L_unk32_off());
    u32 c0 = mem_r8(lay + 0x84), c1 = mem_r8(lay + 0x85);
    // summed live tex-stamp tasks
    u32 ts = mem_r32(cl + CL_unk1C), tcnt = mem_r16(cl + CL_unk1A), texact = 0;
    for (u32 i = 0; ts && i < tcnt; i++) texact += mem_r32(ts + i * TS_stride + 8);
    u32 jcnt = mem_r16(cl + CL_unkD4), rcnt = mem_r16(cl + CL_unk22), mcnt = mem_r16(cl + CL_unk28);
    fprintf(stderr, "[poll-evo f%llu L%u] nz=%d ge50=%d mean=%ld | type=%u flags=%u C0=%u C1=%u | "
                    "tex=%u joint=%u revival=%u model=%u\n",
            (unsigned long long)calls, idx, nz, ge50, nz ? sum / nz : 0,
            type, flags, c0, c1, texact, jcnt, rcnt, mcnt);
}

SUNBRIGHT_OVERRIDE_IF_NATIVE(ov_poll_inspect, 0x8019b3a0u, produce() || dbg() || force()) {
    static u64 calls = 0;
    u32 cl  = cpu.gpr[3];
    u32 idx = cpu.gpr[4];
    if (dbg() && calls < 600) trace_layer(cl, idx, calls);
    if (dbg() && idx == 0 && (calls == 1 || calls == 8 || calls == 20 || calls == 60 || calls == 200)) {
        int n = (int)mem_r32(cl + CL_unk8); u32 arr = mem_r32(cl + CL_unk14);
        for (int i = 0; i < n; i++) { u32 lay = mem_r32(arr + 4u * i); if (lay) dump_coverage_t(lay, i, (int)calls); }
    }
    // Snapshot once per ~120 layer-0 calls (≈ once a couple seconds at 60 Hz, one frame's full state).
    // Per-frame task-activity watch (catch the brief seed): log whenever ANY task queue is non-empty,
    // or for the first 8 frames. unk8 of each tex-stamp (live task count) is the real signal.
    if (dbg() && idx == 0) {
        u32 ts = mem_r32(cl + CL_unk1C);
        u32 tcnt = mem_r16(cl + CL_unk1A);
        u32 jcnt = mem_r16(cl + CL_unkD4), rcnt = mem_r16(cl + CL_unk22), mcnt = mem_r16(cl + CL_unk28);
        u32 texact = 0; for (u32 i = 0; ts && i < tcnt && i < 8; i++) texact += mem_r32(ts + i*TS_stride + 8);
        if (calls < 8 || jcnt || rcnt || mcnt || texact)
            fprintf(stderr, "[poll-f%llu] joint=%u texact=%u revival=%u model=%u\n",
                    (unsigned long long)calls, jcnt, texact, rcnt, mcnt);
    }
    if (dbg() && idx == 0 && (calls % 30) == 0) {
        // coverage mean every 30 frames → localize the seed in time
        int n = (int)mem_r32(cl + CL_unk8);
        u32 arr = mem_r32(cl + CL_unk14);
        for (int i = 0; i < n; i++) {
            u32 lay = mem_r32(arr + 4u * i); if (!lay) continue;
            u32 cov = mem_r32(lay + L_unk54);
            long sum = 0; int nz = 0;
            for (int k = 0; k < 4096; k++) { u8 d = mem_r8(cov + (u32)k * 64); if (d) { nz++; sum += d; } }
            fprintf(stderr, "[poll-cov f%llu] layer[%d] sampled nz=%d mean=%ld\n",
                    (unsigned long long)calls, i, nz, nz ? sum/nz : 0);
        }
    }
    if (dbg() && idx == 0 && (calls++ % 120) == 0) {
        dump(cl);
        static bool once = false;
        if (!once) {
            once = true;
            u32 director = mem_r32(cpu.gpr[13] - 0x6048u);
            u8  mapno    = director ? mem_r8(director + 0x7C) : 0xFF;
            fprintf(stderr, "[poll] gpMarDirector=%08x mMap=%u (map9 ⇒ initTexImage depth-seeds unk54)\n", director, mapno);
            int n = (int)mem_r32(cl + CL_unk8);
            u32 arr = mem_r32(cl + CL_unk14);
            for (int i = 0; i < n; i++) { u32 lay = mem_r32(arr + 4u * i); if (lay) { dump_depthmap(lay, i); dump_coverage(lay, i); } }
        }
    }
    if (force() && idx == 0)
        force_full_coverage(cl);                 // diagnostic full-fill (overrides the producer)
    else if (produce() && present())
        produce_coverage(cl, idx);               // the real native coverage path
    sb_run_original_around(cpu, 0x8019b3a0u, nullptr, 0);
}
