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
#include "gxblend_summary.h"
#include "gxtev_summary.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>

namespace {

// Whole-frame gather-pipe bytes (big-endian, FIFO order). Filled by sb_gather_flush_impl on the
// CPU/PowerPC thread; consumed + cleared at the GXCopyDisp boundary on the same thread (the gather
// pipe is a CPU-thread structure and the GXCopyDisp override fires on the CPU thread), so no lock.
std::vector<u8> g_cap;
GxFrameInfo g_info;
long g_frame_no = 0;
unsigned long g_emitted = 0, g_parse_fail = 0, g_boundaries = 0;

// The per-draw dump windows below default to the first geometry frames [4,8). To reach a scene
// that only appears after boot (e.g. the settled stage-15 file-select, ~frame 200+), set
// SUNBRIGHT_DBG_GXAT=<frame> — the window becomes [GXAT, GXAT+4). Lets the GXBLEND/GXDRAW/GXTEV/
// GXCOPY oracles capture the file-select POST-pass composite for the overbright value comparison.
static inline bool gx_dbg_window() {
    static const long at = [](){ const char* v = std::getenv("SUNBRIGHT_DBG_GXAT"); return v && v[0] ? atol(v) : 4; }();
    return g_frame_no >= at && g_frame_no < at + 4;
}

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
    g_boundaries++;
    // recurse_dls=true: J3D scene geometry is issued through display lists whose vertex data lives in
    // guest RAM, not the FIFO — follow them so verts_pass holds the real per-pass vertex count.
    const bool ok = gxp_parse_frame(g_cap.data(), g_cap.size(), g_info, /*recurse_dls=*/true);
    // Carry the UNCONSUMED tail to the next frame. The gather pipe stages bytes in 32-byte chunks,
    // so when this GXCopyDisp boundary fires the frame's final command is usually truncated (<32 of
    // its bytes still in the pipe). Clearing the whole buffer would drop those bytes and start the
    // NEXT frame mid-command → a perpetual mis-frame cascade. Keeping [consumed, end) lets the split
    // command complete next frame — same contract as gx_stream's decode tail. (RESYNC guard: a genuine
    // unknown opcode at offset 0 consumes nothing; if the carried tail grows past a frame's worth,
    // drop it to recover rather than wedge.)
    const size_t consumed = ok ? g_cap.size() : g_info.fail_offset;
    if (consumed) g_cap.erase(g_cap.begin(), g_cap.begin() + consumed);
    if (g_cap.size() > (4u << 20)) g_cap.clear();   // runaway carry → resync
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
    // Diagnostic (SUNBRIGHT_DBG_GXCAP): running UNION of distinct light positions seen across the
    // WHOLE run, to falsify a per-segment under-count of the per-frame `n`. If this climbs to 8 for a
    // scene whose per-frame `n` reads 3, the 3 is a fragmentation artifact (the frame's 8 lights are
    // split across draw-buffer segments). If it stays at 3, the scene genuinely loads 3.
    if (dbg()) {
        static std::set<long> s_lpos;
        for (int i = 0; i < 8; ++i) if (fi.lights[i].valid) {
            long k = (long)(fi.lights[i].pos[0]) * 73856093L ^ (long)(fi.lights[i].pos[1]) * 19349663L
                   ^ (long)(fi.lights[i].pos[2]) * 83492791L;
            s_lpos.insert(k);
        }
        static size_t s_last = 0;
        if (s_lpos.size() != s_last) {
            s_last = s_lpos.size();
            fprintf(stderr, "[gxcap] distinct light positions seen so far = %zu (this frame n=%d)\n",
                    s_lpos.size(), ln);
        }
    }
    // PER-PASS emit (cross-engine pass tagging): one JSON line per non-empty pass, tagged with the
    // pass name so parity_sweep aligns LIKE pass to LIKE pass. The native parity dump is the 3D scene
    // (perspective) only, so its line carries "pass":"scene" — comparing to THIS perspective bucket's
    // real per-pass vertex count (verts_pass[0]) makes geometry comparable, instead of the old
    // whole-frame prims vs native verts scope mismatch. The ortho bucket is the 2D HUD ("hud").
    // Lighting is whole-frame XF state, attached to the scene pass (where the GX lights are loaded);
    // the hud line reports n=0 to avoid implying the 2D overlay is lit.
    static const char* kPassName[2] = {"scene", "hud"};
    for (int pass = 0; pass < 2; ++pass) {
        if (fi.prims_pass[pass] == 0) continue;     // skip an empty pass this frame
        const bool scene = (pass == 0);
        std::fprintf(f,
            "{\"frame\":%ld,\"pass\":\"%s\",\"nverts\":%u,\"nbatch\":%u,\"prims\":%u,\"geom\":{\"onscr\":%u,"
            "\"nan\":0,\"ndc\":[0,0,0,0,0,0],\"cks\":0.0,\"colcks\":0.0}",
            g_frame_no, kPassName[pass], fi.verts_pass[pass], fi.dls_pass[pass], fi.prims_pass[pass],
            fi.verts_pass[pass]);
        std::fprintf(f, ",\"projType\":%d,\"lights\":{\"n\":%d,\"l\":[", pass, scene ? ln : 0);
        int em = 0;
        if (scene) for (int i = 0; i < 8; ++i) {
            if (!fi.lights[i].valid) continue;
            std::fprintf(f, "%s{\"p\":[%.1f,%.1f,%.1f],\"c\":[%.3f,%.3f,%.3f]}", em++ ? "," : "",
                         fi.lights[i].pos[0], fi.lights[i].pos[1], fi.lights[i].pos[2],
                         fi.lights[i].color[0], fi.lights[i].color[1], fi.lights[i].color[2]);
        }
        std::fprintf(f, "]},\"amb\":[%.3f,%.3f,%.3f],\"matc\":[%.3f,%.3f,%.3f,%.3f]}\n",
                     fi.amb[0], fi.amb[1], fi.amb[2], fi.matc[0], fi.matc[1], fi.matc[2], fi.matc[3]);
    }
    // RENDER-TARGET STRUCTURE oracle (SUNBRIGHT_DBG_GXCOPY): print the in-order EFB-copy sequence for
    // the first geometry frames. Each intra-frame copy with to_xfb=0 is an EFB→TEXTURE snapshot — a
    // render-target boundary the native single-framebuffer composite has no equivalent for (it keeps
    // drawing, double-compositing the scene = the file-select overbright). This is the DIRECT element
    // comparison (Dolphin pass structure vs native's), not a pixel-delta knob.
    if (std::getenv("SUNBRIGHT_DBG_GXCOPY") && gx_dbg_window()) {
        fprintf(stderr, "[gxcopy] frame %ld: prims=%u dls=%u copies=%zu  seq:",
                g_frame_no, fi.prims, fi.display_lists, fi.efb_copies.size());
        for (const auto& c : fi.efb_copies)
            fprintf(stderr, " [@%u prims<=%u %s%s]", c.offset, c.prims_before,
                    c.to_xfb ? "->XFB" : "->TEX", c.clear ? " CLR" : "");
        fprintf(stderr, "\n");
    }
    // PER-DRAW BLEND / TEV value oracle (SUNBRIGHT_DBG_GXBLEND): the ground-truth blend equation +
    // TEV-stage count for every draw, grouped by EFB pass, run-length-deduped (consecutive draws with
    // identical pixel state collapse to one line with a count). The POST pass = the highest efb_pass;
    // diff its blend layers against native's phase-6 batches (sms_boot_present batchdbg `bm=s/d`). GX
    // blend factor names: 0 ZERO 1 ONE 2 SRCCLR 3 INVSRCCLR 4 SRCALPHA 5 INVSRCALPHA 6 DSTALPHA
    // 7 INVDSTALPHA. This is the VALUE comparison the file-select overbright fix must be driven by.
    if (std::getenv("SUNBRIGHT_DBG_GXBLEND") && gx_dbg_window() && !fi.draws.empty()) {
        fprintf(stderr, "[gxblend] frame %ld: %zu draws across %u EFB pass(es)\n",
                g_frame_no, fi.draws.size(), (unsigned)fi.efb_copies.size() + 1);
        // Map the parser's DrawRec to the pure summary unit (gxblend_summary.h) — the SAME code the
        // render_test unit-tests, so the value oracle's grouping/naming is verified Dolphin-free.
        std::vector<gxblend::Draw> ds; ds.reserve(fi.draws.size());
        for (const auto& d : fi.draws)
            ds.push_back({d.efb_pass, d.src, d.dst, d.blend_enable, d.subtract, d.logic_enable,
                          d.color_update, d.alpha_update, d.numtevstages, d.proj_type, d.prims});
        for (const std::string& line : gxblend::summarize(ds))
            fprintf(stderr, "  %s\n", line.c_str());
    }
    // ORDERED per-draw GX-state dump (SUNBRIGHT_DBG_GXDRAW): one line per draw in stream/draw order
    // (NOT run-length-collapsed like GXBLEND), mirroring native's SB_GXDRAW so tools/render/
    // gxstate_diff.py can group both engines by GX-state SIGNATURE and diff colorUpdate per signature.
    // Same factor-name table as GXBLEND. Bounded to the settled frame window [4,8).
    if (std::getenv("SUNBRIGHT_DBG_GXDRAW") && gx_dbg_window() && !fi.draws.empty()) {
        for (size_t i = 0; i < fi.draws.size(); ++i) {
            const auto& d = fi.draws[i];
            fprintf(stderr,
                "[gxdraw] fr=%ld i=%zu pass=%u cU=%u aU=%u be=%u src=%u dst=%u sub=%u tev=%u proj=%u v=%u\n",
                g_frame_no, i, d.efb_pass, d.color_update, d.alpha_update, d.blend_enable,
                d.src, d.dst, d.subtract, d.numtevstages, d.proj_type, d.prims);
        }
    }
    // PER-STAGE TEV COMBINER value oracle (SUNBRIGHT_DBG_GXTEV): for each DISTINCT TEV combiner among
    // the SRCALPHA/SRCCLR draws (the sea-water/composite signature that native paints opaque-white, b76)
    // decode every active stage's color/alpha combiner (a/b/c/d inputs, bias, op, scale, clamp, dest) +
    // the TEV color/konst registers. This is the ground-truth combiner to diff register-for-register
    // against native's generated frag (scratch/frames/bfrag_76.glsl) / NgxTevStage — settling whether
    // native's white sea is a TEV-GEN bug (wrong stage scale / `d` input) or an input (CLR0/vColor) bug.
    if (std::getenv("SUNBRIGHT_DBG_GXTEV") && gx_dbg_window()
        && fi.tev_snaps.size() == fi.draws.size() && !fi.draws.empty()) {
        uint64_t seen[64]; int nseen = 0;   // dedupe by a cheap combiner hash so each material prints once
        for (size_t i = 0; i < fi.draws.size(); ++i) {
            const auto& d = fi.draws[i];
            // The sea-water/composite signature: SRCALPHA(4) src, SRCCLR(2) dst, blending. (Also covers
            // the [noC]-class occlusion draws that share these factors — both are worth seeing by value.)
            if (!(d.blend_enable && d.src == 4 && d.dst == 2)) continue;
            const auto& t = fi.tev_snaps[i];
            uint64_t h = 1469598103934665603ull;
            for (int s = 0; s < t.nstages && s < 16; ++s) { h = (h ^ t.color_env[s]) * 1099511628211ull;
                                                            h = (h ^ t.alpha_env[s]) * 1099511628211ull; }
            bool dup = false; for (int k = 0; k < nseen; ++k) if (seen[k] == h) { dup = true; break; }
            if (dup) continue; if (nseen < 64) seen[nseen++] = h;
            fprintf(stderr, "[gxtev] frame %ld SRCALPHA/SRCCLR draw#%zu pass%u tev=%u verts=%u proj=%s\n",
                    g_frame_no, i, d.efb_pass, t.nstages, d.prims, d.proj_type ? "ortho" : "persp");
            for (int s = 0; s < t.nstages && s < 16; ++s) {
                fprintf(stderr, "    s%d %s\n", s, gxtev::format_color(t.color_env[s]).c_str());
                fprintf(stderr, "       %s\n",    gxtev::format_alpha(t.alpha_env[s]).c_str());
            }
            for (int r = 0; r < 4; ++r)   // TEV color/konst regs: RA=red(0-10)|alpha(12-22), BG=blue(0-10)|green(12-22)
                fprintf(stderr, "    reg%d R=%d A=%d B=%d G=%d\n", r,
                        (int)((t.tevreg_ra[r] & 0x7FF)), (int)((t.tevreg_ra[r] >> 12) & 0x7FF),
                        (int)((t.tevreg_bg[r] & 0x7FF)), (int)((t.tevreg_bg[r] >> 12) & 0x7FF));
        }
    }
    std::fflush(f);
    g_frame_no++;
    g_emitted++;
    if (dbg() && (g_emitted % 128) == 0)
        fprintf(stderr, "[gxcap] emitted=%lu boundaries=%lu parse_fail=%lu last prims=%u dls=%u lights=%d proj=%d\n",
                g_emitted, g_boundaries, g_parse_fail, fi.prims, fi.display_lists, ln, fi.proj_type);
}
