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
#include <cstring>
#include <algorithm>
#include <map>
#include <fstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#ifdef HAVE_DOLPHIN_MEMMAP
#include "Core/System.h"
#include "Core/PowerPC/PowerPC.h"
#endif

// Guest RAM base (defined in gx_parse.cpp) — lets the attribution walker read the guest back-chain.
extern u8* g_ram_base;

namespace {

// ── Oracle GX-draw ATTRIBUTION (SUNBRIGHT_GX_ATTRIB) ──────────────────────────────────────────────
// The oracle runs the real game under Dolphin JIT, so a captured GX draw (e.g. the pass3 reflective
// sea) has no obvious source function. Fix: at every gather-pipe flush, snapshot (byte-offset, guest
// pc, guest sp). At the frame boundary, for the sea-signature draw, find the flush that wrote its
// primitive and walk the guest back-chain (r1 → saved LR) to NAME the drawing function + its callers.
// The back-chain is real guest stack state, valid under JIT regardless of pc precision.
struct FlushMark { std::size_t off; unsigned pc; unsigned sp; };
std::vector<FlushMark> g_flush_marks;

bool attrib_enabled() {
    static int v = -1;
    if (v < 0) { const char* p = getenv("SUNBRIGHT_GX_ATTRIB"); v = (p && p[0] && p[0] != '0') ? 1 : 0; }
    return v == 1;
}

// addr → "name+0xNN" from reference/sms_gmse01_funcs.txt (same map the probe /fn uses).
std::string attrib_sym(unsigned a) {
    static std::vector<std::pair<unsigned, std::string>> t;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        const char* path = getenv("SUNBRIGHT_SYMBOLS");
        std::ifstream f(path ? path : "reference/sms_gmse01_funcs.txt");
        std::string line;
        while (std::getline(f, line)) {
            char* end = nullptr;
            unsigned long addr = strtoul(line.c_str(), &end, 16);
            if (end == line.c_str() || !end) continue;
            while (*end == ' ' || *end == '\t') end++;
            if (addr) t.emplace_back((unsigned)addr, std::string(end));
        }
        std::sort(t.begin(), t.end());
    }
    if (t.empty() || a < t.front().first) { char b[16]; snprintf(b, sizeof b, "%08x", a); return b; }
    auto it = std::upper_bound(t.begin(), t.end(), a,
        [](unsigned v, const std::pair<unsigned, std::string>& p){ return v < p.first; });
    --it;
    char b[256]; snprintf(b, sizeof b, "%s+0x%x", it->second.c_str(), a - it->first);
    return b;
}

unsigned attrib_r32(unsigned a) {   // big-endian guest word via g_ram_base
    if (!g_ram_base || a < 0x80000000u) return 0;
    const u8* p = g_ram_base + (a & 0x01FFFFFFu);
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}

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
#ifdef HAVE_DOLPHIN_MEMMAP
    if (attrib_enabled() && gx_dbg_window()) {
        auto& ppc = Core::System::GetInstance().GetPPCState();
        g_flush_marks.push_back({ g_cap.size(), ppc.pc, ppc.gpr[1] });
    }
#endif
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
        // Emit the PER-PASS ambient (the ambient the GPU used for THIS pass's draws) when recorded,
        // else fall back to the frame-global last-seen value. The scene pass's per-pass amb is the
        // valid oracle for native's scene-draw ambient (the frame-global one is the HUD's white).
        const bool ap = fi.amb_pass_set[pass];
        const float ax = ap ? fi.amb_pass[pass][0] : fi.amb[0];
        const float ay = ap ? fi.amb_pass[pass][1] : fi.amb[1];
        const float az = ap ? fi.amb_pass[pass][2] : fi.amb[2];
        std::fprintf(f, "]},\"amb\":[%.3f,%.3f,%.3f],\"matc\":[%.3f,%.3f,%.3f,%.3f],\"imm_verts\":%u,\"tris\":%u",
                     ax, ay, az, fi.matc[0], fi.matc[1], fi.matc[2], fi.matc[3], fi.imm_verts_pass[pass], fi.tris_pass[pass]);
        if (scene) {   // whole-frame EFB-pass triangle split (scene line only): main vs composite passes
            std::fprintf(f, ",\"tris_efb\":[");
            for (unsigned e = 0; e <= fi.max_efb_pass && e < 8; ++e)
                std::fprintf(f, "%s%u", e ? "," : "", fi.tris_efb[e]);
            std::fprintf(f, "]");
        }
        std::fprintf(f, "}\n");
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
#ifdef HAVE_DOLPHIN_MEMMAP
    // ATTRIBUTION: name the source function of each SRCALPHA/SRCCLR draw (the reflective-sea signature).
    // For each such draw, find the gather flush that wrote its primitive (largest mark off ≤ draw.offset)
    // and walk the guest back-chain from that flush's SP → names the object's draw method + callers.
    if (attrib_enabled() && gx_dbg_window() && !fi.draws.empty()) {
        unsigned seen_sp[64]; int nseen = 0;
        // SUNBRIGHT_GX_ATTRIB_IMM: name EVERY immediate-mode (in-FIFO GXBegin) perspective/scene draw,
        // not just the sea-blend signature — identifies what the oracle's ~1640 imm scene verts actually
        // are (is a 3900-vert TMapObjWave among them? the native drive_wave-overdraw question).
        const bool imm_mode = getenv("SUNBRIGHT_GX_ATTRIB_IMM") != nullptr;
        for (size_t i = 0; i < fi.draws.size(); ++i) {
            const auto& d = fi.draws[i];
            const bool sea_sig = d.blend_enable && d.src == 4 && d.dst == 2 && d.proj_type == 0;
            const bool imm_scene = imm_mode && d.immediate && d.proj_type == 0 && d.prims > 0;
            if (!sea_sig && !imm_scene) continue;
            // nearest flush mark at or before this primitive's stream offset
            const FlushMark* best = nullptr;
            for (const auto& m : g_flush_marks) {
                if (m.off <= d.offset && (!best || m.off > best->off)) best = &m;
            }
            if (!best) continue;
            bool dup = false; for (int k = 0; k < nseen; ++k) if (seen_sp[k] == best->sp) { dup = true; break; }
            if (dup) continue; if (nseen < 64) seen_sp[nseen++] = best->sp;
            fprintf(stderr, "[gxattrib] frame %ld draw#%zu tev=%u verts=%u off=%u pc=%08x %s | sp=%08x\n",
                    g_frame_no, i, d.numtevstages, d.prims, d.offset, best->pc,
                    attrib_sym(best->pc).c_str(), best->sp);
            unsigned fp = best->sp;
            for (int fr = 0; fr < 16 && fp >= 0x80000000u && fp < 0x81800000u; fr++) {
                unsigned lr = attrib_r32(fp + 4);
                fprintf(stderr, "    [%2d] lr=%08x  %s\n", fr, lr, attrib_sym(lr).c_str());
                unsigned nx = attrib_r32(fp);
                if (nx <= fp || nx < 0x80000000u || nx >= 0x81800000u) break;
                fp = nx;
            }
        }
    }
    // SUNBRIGHT_GX_ATTRIB_SUM: aggregate RAW VERTS per source function (back-chain) across ALL
    // perspective/scene draws this frame — names the biggest scene geometry contributors so a native
    // under-draw can be pinned to the object the oracle draws more of. Needs SUNBRIGHT_DBG_GXDRAW
    // (fi.draws) + SUNBRIGHT_GX_ATTRIB (flush marks) + the DBG_GXAT window.
    if (attrib_enabled() && gx_dbg_window() && getenv("SUNBRIGHT_GX_ATTRIB_SUM") && !fi.draws.empty()) {
        std::map<std::string, unsigned long> sumv;
        for (size_t i = 0; i < fi.draws.size(); ++i) {
            const auto& d = fi.draws[i];
            if (d.proj_type != 0 || d.prims == 0) continue;   // scene (perspective) draws only
            const FlushMark* best = nullptr;
            for (const auto& m : g_flush_marks)
                if (m.off <= d.offset && (!best || m.off > best->off)) best = &m;
            std::string who = best ? attrib_sym(best->pc) : std::string("?");
            if (best) {   // prefer the first back-chain frame naming a draw/perform (skip GX internals)
                unsigned fp = best->sp;
                for (int fr = 0; fr < 6 && fp >= 0x80000000u && fp < 0x81800000u; fr++) {
                    std::string s = attrib_sym(attrib_r32(fp + 4));
                    if (s.find("draw") != std::string::npos || s.find("Draw") != std::string::npos
                        || s.find("perform") != std::string::npos) { who = s; break; }
                    unsigned nx = attrib_r32(fp); if (nx <= fp) break; fp = nx;
                }
            }
            sumv[who] += d.prims;
        }
        std::vector<std::pair<std::string,unsigned long>> v(sumv.begin(), sumv.end());
        std::sort(v.begin(), v.end(), [](const std::pair<std::string,unsigned long>&a,
                                         const std::pair<std::string,unsigned long>&b){ return a.second > b.second; });
        fprintf(stderr, "[gxsum] frame %ld top scene draws by raw-verts (total scene tris=%u):\n",
                g_frame_no, fi.tris_pass[0]);
        for (size_t i = 0; i < v.size() && i < 16; i++)
            fprintf(stderr, "  %8lu  %s\n", v[i].second, v[i].first.c_str());
    }
    g_flush_marks.clear();
#endif
    std::fflush(f);
    g_frame_no++;
    g_emitted++;
    if (dbg() && (g_emitted % 128) == 0)
        fprintf(stderr, "[gxcap] emitted=%lu boundaries=%lu parse_fail=%lu last prims=%u dls=%u lights=%d proj=%d\n",
                g_emitted, g_boundaries, g_parse_fail, fi.prims, fi.display_lists, ln, fi.proj_type);
}
