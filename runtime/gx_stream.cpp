// GX stream assembler — see gx_stream.h for the design contract.
#include "gx_stream.h"
#include "gx_parse.h"
#include "ngx/ngx_decode.h"   // R1: native GX decoder, run in lockstep for parity

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <chrono>
#include <cmath>

#ifdef HAVE_DOLPHIN_MEMMAP
#include "Core/HW/GPFifo.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "Core/Core.h"
#include "VideoCommon/OpcodeDecoding.h"
#include "VideoCommon/DataReader.h"
#endif

namespace {

// Tailored renderer (docs/port_roadmap.md "native renderer arc") — THE renderer,
// for both vanilla and 60 fps. We own the GX byte stream: instead of pushing the
// held bytes back into Dolphin's CP FIFO *ring* (GPFifoManager → 512 KB ring →
// async GPU thread drain), we decode them SYNCHRONOUSLY on the producing guest
// thread via Dolphin's OpcodeDecoder. There is no fixed ring, so the "FIFO is
// overflowed by GatherPipe! CPU thread is too fast!" crash (CommandProcessor.cpp
// :390 — CPReadWriteDistance > CPEnd-CPBase) is structurally impossible: each
// held run is decoded the moment its sync point is reached. This is what lets the
// 60 fps redraw double the command volume without lapping a ring. We still drive
// Dolphin's VideoCommon rasterizer (VertexManager → Vulkan, texture cache,
// shader-gen) — we own the *frontend* (FIFO pacing/lifetime), not the rasterizer
// (kept by doctrine).
bool dbg() {
    static int v = -1;
    if (v < 0) v = getenv("SUNBRIGHT_DBG_GXS") ? 1 : 0;
    return v == 1;
}

bool g_armed = false;

// Held gather-pipe bytes, in FIFO order (big-endian, exactly what GPFifo gets).
std::vector<u8> g_buf;

// Persistent decode buffer = our "video
// buffer". RunFifo decodes whole commands only and returns a pointer past the
// last complete one; a partial trailing command (rare — guest sync points land
// on command boundaries) carries to the next flush. Stats: bytes decoded, and
// the high-water tail (a non-zero steady tail would mean we're mis-splitting).
std::vector<u8> g_decbuf;
unsigned long long g_decoded_bytes = 0;
unsigned long g_decode_runs = 0;
size_t g_tail_hi = 0;

// Whole-frame capture: every armed byte, regardless of mid-frame flushes (the
// flush queue empties ~5×/frame at GXFlush; the replay needs the full frame).
// Rotated at the display-copy boundary; parsed by gx_parse.
std::vector<u8> g_frame, g_prev_frame;
GxFrameInfo g_frame_info, g_prev_info;
unsigned long g_parse_ok = 0, g_parse_fail = 0;
unsigned long long g_tokens_seen = 0, g_mtx_arrays_seen = 0;

// ── R1 parity harness (SUNBRIGHT_NGX_PARITY=1) ──────────────────────────────
// Run our native decoder (ngx_parse_frame) in lockstep with the Dolphin-based
// oracle (gxp_parse_frame) over the SAME frame stream and compare field-by-field.
// Both keep persistent CP state advanced by these calls only, so feeding them the
// identical sequence of frames keeps them in sync. Goal: zero mismatches.
bool ngx_parity() {
    static int v = -1;
    if (v < 0) v = getenv("SUNBRIGHT_NGX_PARITY") ? 1 : 0;
    return v == 1;
}
unsigned long g_ngx_frames = 0;      // frames compared
unsigned long g_ngx_mismatch = 0;    // frames with ANY field mismatch
unsigned long g_ngx_first_off = 0;   // first mismatching frame index (1-based) or 0
char g_ngx_first_what[64] = {0};     // what differed on that first mismatch

void ngx_check(const u8* p, size_t n, const GxFrameInfo& oracle) {
    GxFrameInfo mine;
    ngx_parse_frame(p, n, mine);
    g_ngx_frames++;

    const char* what = nullptr;
    char buf[64];
    auto cmp_u32 = [&](const char* name, u32 a, u32 b) {
        if (!what && a != b) { snprintf(buf, sizeof buf, "%s %u!=%u", name, a, b); what = buf; }
    };
    if (!what && mine.ok != oracle.ok) {
        snprintf(buf, sizeof buf, "ok %d!=%d @%u", mine.ok, oracle.ok, mine.fail_offset);
        what = buf;
    }
    cmp_u32("prims", mine.prims, oracle.prims);
    cmp_u32("dls", mine.display_lists, oracle.display_lists);
    cmp_u32("copies", mine.copies, oracle.copies);
    cmp_u32("tokens", (u32)mine.token_offsets.size(), (u32)oracle.token_offsets.size());
    cmp_u32("mtx", (u32)mine.mtx_arrays.size(), (u32)oracle.mtx_arrays.size());
    cmp_u32("failoff", mine.fail_offset, oracle.fail_offset);
    // exact offsets/values of the recorded patch points (byte-level parity)
    if (!what && mine.token_offsets.size() == oracle.token_offsets.size())
        for (size_t i = 0; i < mine.token_offsets.size(); i++)
            if (mine.token_offsets[i] != oracle.token_offsets[i]) {
                snprintf(buf, sizeof buf, "tokoff[%zu]", i); what = buf; break;
            }
    if (!what && mine.mtx_arrays.size() == oracle.mtx_arrays.size())
        for (size_t i = 0; i < mine.mtx_arrays.size(); i++) {
            const auto& a = mine.mtx_arrays[i]; const auto& b = oracle.mtx_arrays[i];
            if (a.offset != b.offset || a.array != b.array || a.base != b.base) {
                snprintf(buf, sizeof buf, "mtx[%zu]", i); what = buf; break;
            }
        }

    if (what) {
        g_ngx_mismatch++;
        if (!g_ngx_first_off) {
            g_ngx_first_off = g_ngx_frames;
            snprintf(g_ngx_first_what, sizeof g_ngx_first_what, "%s", what);
        }
        if (dbg() && g_ngx_mismatch <= 16)
            fprintf(stderr, "[ngx-parity] frame %lu MISMATCH: %s (bytes=%zu)\n",
                    g_ngx_frames, what, n);
    }
}

// Safety cap: a frame stream is typically a few hundred KB; if a frame somehow
// never hits a flush point, burst rather than grow unbounded.
constexpr size_t kFlushCap = 8u << 20;

// Stats (SUNBRIGHT_DBG_GXS=1): per-frame byte counts + flush causes.
unsigned long g_frames = 0;
unsigned long long g_frame_bytes = 0, g_total_bytes = 0;
unsigned long g_fl_gxflush = 0, g_fl_copy = 0, g_fl_cap = 0;

// Diagnostic: SUNBRIGHT_GXOWN_SYNC=1 flushes after every append. If a held-buffer
// run corrupts the FIFO but a sync run doesn't, the bytes are fine and something
// else is writing GPFifo directly (interleaving with held bytes).
bool sync_mode() {
    static int v = -1;
    if (v < 0) v = getenv("SUNBRIGHT_GXOWN_SYNC") ? 1 : 0;
    return v == 1;
}

// Foreign-writer detector. While we hold bytes, nothing should move Dolphin's
// gather pipe; if its fill count changes between our flushes, a guest function
// running OUTSIDE recomp (Dolphin JIT inline gather store, or interpreter via
// MMU) wrote the pipe — that is the held-mode FIFO corruption mechanism. Log
// Dolphin's ppc.pc so the culprit can be named and recompiled.
ptrdiff_t pipe_fill() {
#ifdef HAVE_DOLPHIN_MEMMAP
    auto& ppc = Core::System::GetInstance().GetPPCState();
    return ppc.gather_pipe_ptr - ppc.gather_pipe_base_ptr;
#else
    return 0;
#endif
}
ptrdiff_t g_expect_fill = 0;

void check_foreign() {
#ifdef HAVE_DOLPHIN_MEMMAP
    const ptrdiff_t fill = pipe_fill();
    if (fill == g_expect_fill) return;
    static int logged = 0;
    if (logged < 32) {
        auto& ppc = Core::System::GetInstance().GetPPCState();
        fprintf(stderr, "[gxs] FOREIGN gather write while holding: fill %td→%td dolphin_pc=%08x\n",
                g_expect_fill, fill, ppc.pc);
        logged++;
    }
    g_expect_fill = fill;
#endif
}

inline void append(const u8* p, size_t n) {
    if (!g_buf.empty()) check_foreign();
    g_buf.insert(g_buf.end(), p, p + n);
    g_frame.insert(g_frame.end(), p, p + n);
    g_frame_bytes += n;
    if (sync_mode()) { gxs_flush(nullptr); return; }
    if (g_buf.size() >= kFlushCap) { g_fl_cap++; gxs_flush(nullptr); }
}

}  // namespace

bool gxs_active() { return g_armed; }

namespace { bool g_in_flush = false; }   // guest threads are nthr-serialized
bool gxs_in_flush() { return g_in_flush; }

void gxs_arm() {
    if (g_armed) return;
    g_armed = true;
    fprintf(stderr, "[gxs] tailored GX frontend armed (first display copy) — "
                    "direct OpcodeDecoder, no CP ring\n");
}

void gxs_w8(u8 v)  { append(&v, 1); }
void gxs_w16(u16 v) { u8 b[2] = { (u8)(v >> 8), (u8)v }; append(b, 2); }
void gxs_w32(u32 v) {
    u8 b[4] = { (u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v };
    append(b, 4);
}
void gxs_w64(u64 v) { gxs_w32((u32)(v >> 32)); gxs_w32((u32)v); }

#ifdef HAVE_DOLPHIN_MEMMAP
// Owned-render sink: decode the held bytes through Dolphin's OpcodeDecoder right
// here, on the guest thread, with no CP ring. nthr serializes guest threads, so
// only one thread is ever in here; we declare it the GPU thread for the duration
// because VertexManager/TextureCache/Vulkan submission gate on Core::IsGPUThread.
void decode_owned() {
    if (g_buf.empty()) return;
    g_decbuf.insert(g_decbuf.end(), g_buf.begin(), g_buf.end());
    g_buf.clear();

    u8* const start = g_decbuf.data();
    u8* const end   = start + g_decbuf.size();
    Core::DeclareAsGPUThread();
    u32 cycles = 0;
    const double dec_t0 = gxs_decode_tick_begin();
    u8* const consumed = OpcodeDecoder::RunFifo<false>(DataReader(start, end), &cycles);
    gxs_decode_tick_end(dec_t0);
    Core::UndeclareAsGPUThread();

    const size_t used = static_cast<size_t>(consumed - start);
    const size_t tail = g_decbuf.size() - used;
    g_decoded_bytes += used;
    g_decode_runs++;
    if (tail > g_tail_hi) g_tail_hi = tail;
    // Keep the unconsumed partial-command tail for the next flush.
    if (used) g_decbuf.erase(g_decbuf.begin(), g_decbuf.begin() + used);
}
#endif

void gxs_flush(const char* why) {
    if (g_buf.empty()) return;
#ifdef HAVE_DOLPHIN_MEMMAP
    g_in_flush = true;
    decode_owned();   // synchronous OpcodeDecoder, no CP ring (the only sink)
    g_in_flush = false;
#endif
    g_total_bytes += g_buf.size();
    g_buf.clear();
    g_expect_fill = pipe_fill();
    if (why && why[0] == 'g') g_fl_gxflush++;
}

unsigned long long gxs_decoded_bytes() { return g_decoded_bytes; }
unsigned long      gxs_decode_runs()   { return g_decode_runs; }
const GxFrameInfo& gxs_prev_frame_info() { return g_prev_info; }

void gxs_ngx_parity_stats(unsigned long* frames, unsigned long* mismatch,
                          unsigned long* first_frame, const char** first_what) {
    if (frames)      *frames = g_ngx_frames;
    if (mismatch)    *mismatch = g_ngx_mismatch;
    if (first_frame) *first_frame = g_ngx_first_off;
    if (first_what)  *first_what = g_ngx_first_off ? g_ngx_first_what : "";
}

// ── Render-only interpolation replay ─────────────────────────────────────────────────────────────
const std::vector<u8>& gxs_cur_frame() { return g_frame; }

void gxs_replay_frame(const u8* data, size_t n) {
#ifdef HAVE_DOLPHIN_MEMMAP
    if (!data || !n) return;
    // Replay a complete, command-aligned frame stream straight through the OpcodeDecoder — renders
    // the captured geometry again (to the EFB) with no CP ring and no game code. The buffer is a
    // whole frame, so RunFifo consumes it fully; any unconsumed tail would be a capture bug.
    Core::DeclareAsGPUThread();
    u32 cycles = 0;
    OpcodeDecoder::RunFifo<false>(
        DataReader(const_cast<u8*>(data), const_cast<u8*>(data) + n), &cycles);
    Core::UndeclareAsGPUThread();
#else
    (void)data; (void)n;
#endif
}

// Inter-present (frame-boundary) wall-time stats over a sliding window — the
// objective jitter measure. Even 60 fps delivery = tight ~16.7 ms intervals;
// jitter shows as a large spread (stddev / min-max). Read via /frametime.
namespace {
double g_ft_last_ms = 0;
unsigned long g_ft_n = 0;
double g_ft_sum = 0, g_ft_sumsq = 0, g_ft_min = 1e30, g_ft_max = 0;
// GX decode (synchronous render submission on the guest thread) wall time, to see
// whether frame-time spikes are render or game-logic. g_ft_dec_frame accumulates
// decode ms within the current frame; captured at the boundary.
double g_ft_dec_frame = 0, g_ft_dec_sum = 0, g_ft_dec_atmax = 0;
// CoreTiming::Advance (Dolphin event/timing machinery) wall time per frame.
double g_ft_ct_frame = 0, g_ft_ct_sum = 0, g_ft_ct_atmax = 0;
double now_ms() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace
double gxs_decode_tick_begin() { return now_ms(); }
void   gxs_decode_tick_end(double t0) { g_ft_dec_frame += now_ms() - t0; }
double gxs_ct_tick_begin() { return now_ms(); }
void   gxs_ct_tick_end(double t0) { g_ft_ct_frame += now_ms() - t0; }
void gxs_frametime_reset() { g_ft_n = 0; g_ft_sum = g_ft_sumsq = g_ft_max = 0; g_ft_min = 1e30; g_ft_last_ms = 0; g_ft_dec_sum = g_ft_dec_atmax = 0; g_ft_ct_sum = g_ft_ct_atmax = 0; }
void gxs_frametime_stats(unsigned long* n, double* mean, double* stddev, double* mn, double* mx) {
    *n = g_ft_n;
    *mean = g_ft_n ? g_ft_sum / g_ft_n : 0;
    const double var = g_ft_n ? g_ft_sumsq / g_ft_n - (*mean) * (*mean) : 0;
    *stddev = var > 0 ? std::sqrt(var) : 0;
    *mn = g_ft_n ? g_ft_min : 0; *mx = g_ft_max;
}
void gxs_frametime_decode(double* mean_dec_ms, double* dec_at_max) {
    *mean_dec_ms = g_ft_n ? g_ft_dec_sum / g_ft_n : 0;
    *dec_at_max = g_ft_dec_atmax;   // decode ms of the slowest frame
}
void gxs_frametime_ct(double* mean_ct_ms, double* ct_at_max) {
    *mean_ct_ms = g_ft_n ? g_ft_ct_sum / g_ft_n : 0;
    *ct_at_max = g_ft_ct_atmax;
}

void gxs_frame_boundary() {
    if (!g_armed) return;
    gxs_flush("copy");
    g_fl_copy++;
    g_frames++;
    { const double t = now_ms();
      if (g_ft_last_ms > 0) { const double d = t - g_ft_last_ms;
          if (d > 0 && d < 1000) { g_ft_n++; g_ft_sum += d; g_ft_sumsq += d*d;
              if (d < g_ft_min) g_ft_min = d;
              if (d > g_ft_max) { g_ft_max = d; g_ft_dec_atmax = g_ft_dec_frame; g_ft_ct_atmax = g_ft_ct_frame; }
              g_ft_dec_sum += g_ft_dec_frame; g_ft_ct_sum += g_ft_ct_frame; } }
      g_ft_last_ms = t; g_ft_dec_frame = 0; g_ft_ct_frame = 0; }

    // Analyze the completed frame; only a fully-parsed frame qualifies as a
    // replay source (the very first armed frames mis-size vertices until J3D
    // reprograms VCD/VAT — ok=false catches that).
    const bool parse_ok = gxp_parse_frame(g_frame.data(), g_frame.size(), g_frame_info);
    // R1: native decoder parity check — MUST run on every frame (in lockstep) so
    // its persistent CP state tracks the oracle's. Compares against g_frame_info.
    if (ngx_parity()) ngx_check(g_frame.data(), g_frame.size(), g_frame_info);
    if (parse_ok) {
        g_parse_ok++;
        g_tokens_seen     += g_frame_info.token_offsets.size();
        g_mtx_arrays_seen += g_frame_info.mtx_arrays.size();
    } else {
        g_parse_fail++;
        if (dbg() && g_parse_fail <= 12) {
            const u32 o = g_frame_info.fail_offset;
            fprintf(stderr, "[gxs] parse FAIL at %u/%u op=%02x ctx:", o, g_frame_info.total,
                    g_frame_info.fail_opcode);
            for (u32 i = (o >= 8 ? o - 8 : 0); i < o + 12 && i < g_frame.size(); i++)
                fprintf(stderr, "%s%02x", i == o ? " | " : " ", g_frame[i]);
            fprintf(stderr, "\n");
        }
    }
    // NOTE: the SUNBRIGHT_PARITY_DUMP oracle emit moved to gx_capture.cpp. This gxs path (the
    // eradicated ngx owned-render frontend) is armed only by the recomp override and is dormant under
    // pure-JIT (no-recomp), where the parity oracle actually runs — see gx_capture.cpp's header.

    g_frame.swap(g_prev_frame);
    std::swap(g_frame_info, g_prev_info);
    g_frame.clear();

    if (dbg() && (g_frames % 128) == 0)
        fprintf(stderr,
                "[gxs] frames=%lu last_frame_bytes=%llu decoded=%lluMB runs=%lu "
                "tail_hi=%zu flushes gx=%lu copy=%lu parse ok=%lu fail=%lu "
                "ngx[cmp=%lu mismatch=%lu first=%lu:%s]\n",
                g_frames, g_frame_bytes, g_decoded_bytes >> 20, g_decode_runs,
                g_tail_hi, g_fl_gxflush, g_fl_copy, g_parse_ok, g_parse_fail,
                g_ngx_frames, g_ngx_mismatch, g_ngx_first_off,
                g_ngx_first_off ? g_ngx_first_what : "-");
    g_frame_bytes = 0;
}
