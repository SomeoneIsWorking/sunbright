// GX command-stream capture — the Dolphin-GX parity ORACLE (build/sunbright, pure-JIT).
//
// ARCHITECTURE (CLAUDE.md 2026-06-30): build/sunbright is the pure Dolphin-GX oracle. Dolphin
// renders the guest GX draws via its own CP/GPU path; we do NOT replace rendering here (that was
// the eradicated ngx owned-render path in gx_stream.cpp). This module only CAPTURES the same
// gather-pipe byte stream Dolphin's GPU consumes and, per frame, parses it with gxp_parse_frame
// and emits one JSON line matching sms-boot's sb_parity_dump.h schema, so
// tools/render/parity_sweep.py can diff the two engines' lighting/projection BY VALUE.
//
// Why a fork-level tap (sb_slot_gather_flush in GPFifo::UpdateGatherPipe) instead of the Write*
// funnel: under pure-JIT (no-recomp) the JIT inline-gather optimization writes the gather pipe
// directly (bumps gather_pipe_ptr, no GPFifoManager::Write* call), so the gpfifo_wrap hook and the
// memory_bridge funnel both miss it — gx_stream captured 0 frames. UpdateGatherPipe is the single
// choke point every byte (inline-gather included) passes through. See the handoff + memory
// [[gx-command-stream-oracle]].
//
// Capture-only and gated on SUNBRIGHT_PARITY_DUMP=<path>: when unset, the hook early-outs (the
// product / normal runs pay only a null-ish check). Frame boundary = GXCopyDisp, driven by the
// purejit-safe override in gx_stream_own.cpp calling sb_gx_capture_frame_boundary().

#include "gx_parse.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

// Whole-frame gather-pipe bytes (big-endian, FIFO order). Filled by sb_gather_flush_impl on the
// CPU/PowerPC thread; consumed + cleared at the GXCopyDisp boundary on the same thread (the gather
// pipe is a CPU-thread structure and the GXCopyDisp override fires on the CPU thread), so no lock.
std::vector<u8> g_cap;
GxFrameInfo g_info;
long g_frame_no = 0;
unsigned long g_emitted = 0, g_parse_fail = 0;

// Safety cap: a normal frame is a few hundred KB; if a copy boundary is ever missed, burst rather
// than grow unbounded (the very first "frame" also folds in all boot-time GX setup — bounded here).
constexpr size_t kCap = 16u << 20;

bool enabled() {
    static int v = -1;
    if (v < 0) { const char* p = getenv("SUNBRIGHT_PARITY_DUMP"); v = (p && p[0]) ? 1 : 0; }
    return v == 1;
}

std::FILE* outfile() {
    static std::FILE* f = [](){ const char* p = getenv("SUNBRIGHT_PARITY_DUMP");
        return (p && p[0]) ? std::fopen(p, "w") : nullptr; }();
    return f;
}

bool dbg() {
    static int v = -1;
    if (v < 0) v = getenv("SUNBRIGHT_DBG_GXCAP") ? 1 : 0;
    return v == 1;
}

}  // namespace

// Fork hook body (installed as sb_slot_gather_flush in sb_install_hooks). Sees every gather-pipe
// chunk Dolphin flushes to the CP FIFO, in order, big-endian.
extern "C" void sb_gather_flush_impl(const u8* bytes, std::size_t n) {
    if (!enabled() || !bytes || !n) return;
    if (g_cap.size() + n > kCap) g_cap.clear();   // never seen a boundary — drop, don't OOM
    g_cap.insert(g_cap.end(), bytes, bytes + n);
}

// Frame boundary (GXCopyDisp). Parse the captured frame and emit one parity JSON line matching the
// fields tools/render/parity_sweep.py consumes from sms-boot's sb_parity_dump.h. The lighting /
// ambient / material / projType are exact XF register state from the stream's loads (no transform).
// geom.onscr is a LIVENESS proxy (= prims) and ndc is zeroed — the real clip-space vertex transform
// is phase 2; the harness already treats nverts/onscr/ndc as capture-scope confounded cross-engine
// and relies on light-count + ambient/material/projType, which are exact here.
extern "C" void sb_gx_capture_frame_boundary() {
    if (!enabled() || g_cap.empty()) return;
    const bool ok = gxp_parse_frame(g_cap.data(), g_cap.size(), g_info);
    g_cap.clear();
    if (!ok) {
        g_parse_fail++;
        if (dbg() && g_parse_fail <= 12)
            fprintf(stderr, "[gxcap] parse FAIL at %u/%u op=%02x\n",
                    g_info.fail_offset, g_info.total, g_info.fail_opcode);
        return;
    }

    std::FILE* f = outfile();
    const GxFrameInfo& fi = g_info;
    // Only emit frames with real geometry — a blank/2D-only frame would pollute the settled-window
    // medians parity_sweep computes (it already skips onscr==0, but prims==0 is the cleaner gate).
    if (!f || fi.prims == 0) return;

    int ln = 0; for (int i = 0; i < 8; ++i) if (fi.lights[i].valid) ln++;
    std::fprintf(f,
        "{\"frame\":%ld,\"nverts\":%u,\"nbatch\":%u,\"geom\":{\"onscr\":%u,\"nan\":0,"
        "\"ndc\":[0,0,0,0,0,0],\"cks\":0.0,\"colcks\":0.0}",
        g_frame_no++, fi.prims, fi.display_lists, fi.prims);
    std::fprintf(f, ",\"projType\":%d,\"lights\":{\"n\":%d,\"l\":[", fi.proj_type, ln);
    int em = 0;
    for (int i = 0; i < 8; ++i) {
        if (!fi.lights[i].valid) continue;
        std::fprintf(f, "%s{\"p\":[%.1f,%.1f,%.1f],\"c\":[%.3f,%.3f,%.3f]}", em++ ? "," : "",
                     fi.lights[i].pos[0], fi.lights[i].pos[1], fi.lights[i].pos[2],
                     fi.lights[i].color[0], fi.lights[i].color[1], fi.lights[i].color[2]);
    }
    std::fprintf(f, "]},\"amb\":[%.3f,%.3f,%.3f],\"matc\":[%.3f,%.3f,%.3f,%.3f]}\n",
                 fi.amb[0], fi.amb[1], fi.amb[2], fi.matc[0], fi.matc[1], fi.matc[2], fi.matc[3]);
    std::fflush(f);
    g_emitted++;
    if (dbg() && (g_emitted % 128) == 0)
        fprintf(stderr, "[gxcap] emitted=%lu parse_fail=%lu last prims=%u dls=%u lights=%d proj=%d\n",
                g_emitted, g_parse_fail, fi.prims, fi.display_lists, ln, fi.proj_type);
}
