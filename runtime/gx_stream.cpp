// GX stream assembler — see gx_stream.h for the design contract.
#include "gx_stream.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef HAVE_DOLPHIN_MEMMAP
#include "Core/HW/GPFifo.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#endif

namespace {

bool env_on() {
    static int v = -1;
    if (v < 0) v = getenv("SUNBRIGHT_GXOWN") ? 1 : 0;
    return v == 1;
}
bool dbg() {
    static int v = -1;
    if (v < 0) v = getenv("SUNBRIGHT_DBG_GXS") ? 1 : 0;
    return v == 1;
}

bool g_armed = false;

// Held gather-pipe bytes, in FIFO order (big-endian, exactly what GPFifo gets).
std::vector<u8> g_buf;

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
    g_frame_bytes += n;
    if (sync_mode()) { gxs_flush(nullptr); return; }
    if (g_buf.size() >= kFlushCap) { g_fl_cap++; gxs_flush(nullptr); }
}

}  // namespace

bool gxs_active() { return g_armed; }

namespace { bool g_in_flush = false; }   // guest threads are nthr-serialized
bool gxs_in_flush() { return g_in_flush; }

void gxs_arm() {
    if (g_armed || !env_on()) return;
    g_armed = true;
    fprintf(stderr, "[gxs] GX stream assembler armed (first display copy)\n");
}

void gxs_w8(u8 v)  { append(&v, 1); }
void gxs_w16(u16 v) { u8 b[2] = { (u8)(v >> 8), (u8)v }; append(b, 2); }
void gxs_w32(u32 v) {
    u8 b[4] = { (u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v };
    append(b, 4);
}
void gxs_w64(u64 v) { gxs_w32((u32)(v >> 32)); gxs_w32((u32)v); }

void gxs_flush(const char* why) {
    if (g_buf.empty()) return;
#ifdef HAVE_DOLPHIN_MEMMAP
    g_in_flush = true;
    auto& gpf = Core::System::GetInstance().GetGPFifo();
    size_t i = 0;
    const size_t n = g_buf.size();
    for (; i + 4 <= n; i += 4) {
        u32 v;
        memcpy(&v, &g_buf[i], 4);
        gpf.Write32(__builtin_bswap32(v));
    }
    for (; i < n; i++) gpf.Write8(g_buf[i]);
    g_in_flush = false;
#endif
    g_total_bytes += g_buf.size();
    g_buf.clear();
    g_expect_fill = pipe_fill();
    if (why && why[0] == 'g') g_fl_gxflush++;
}

void gxs_frame_boundary() {
    if (!g_armed) return;
    gxs_flush("copy");
    g_fl_copy++;
    g_frames++;
    if (dbg() && (g_frames % 128) == 0)
        fprintf(stderr,
                "[gxs] frames=%lu last_frame_bytes=%llu total=%lluMB flushes gx=%lu copy=%lu cap=%lu\n",
                g_frames, g_frame_bytes, g_total_bytes >> 20,
                g_fl_gxflush, g_fl_copy, g_fl_cap);
    g_frame_bytes = 0;
}
