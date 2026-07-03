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
#include "pin_state_schema.h"

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
#include <sys/stat.h>
#include <sys/types.h>

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
struct FlushMark { std::size_t off; unsigned pc; unsigned sp; unsigned r31; };
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
// Guest RAM readers used by the pin fingerprint. Same big-endian layout as attrib_r32.
inline u8 guest_r8(unsigned a) {
    if (!g_ram_base || a < 0x80000000u) return 0;
    return g_ram_base[(a & 0x01FFFFFFu)];
}
inline u16 guest_r16(unsigned a) {
    if (!g_ram_base || a < 0x80000000u) return 0;
    const u8* p = g_ram_base + (a & 0x01FFFFFFu);
    return ((u16)p[0] << 8) | p[1];
}
inline s16 guest_r16s(unsigned a) { return (s16)guest_r16(a); }
inline float guest_rf32(unsigned a) {
    u32 v = attrib_r32(a);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
inline void guest_read_vec3f(unsigned a, float out[3]) {
    out[0] = guest_rf32(a);
    out[1] = guest_rf32(a + 4);
    out[2] = guest_rf32(a + 8);
}

// Populate SbPinGameState from guest RAM at the RE'd US GMSE01 addresses.
// Symmetric to native's sb_pin_state_populate (native/src/pin_state_native.cpp).
void oracle_populate_pin_state(SbPinGameState& gs) {
    std::memset(&gs, 0, sizeof(gs));
    if (!g_ram_base) return;

    // TApplication is embedded at gpApplication (not a pointer).
    gs.app_state = guest_r8(SMS_US_GPAPPLICATION + TAPP_OFF_APPSTATE);
    unsigned stage    = guest_r8 (SMS_US_GPAPPLICATION + TAPP_OFF_NEXTAREA + 0);
    unsigned scenario = guest_r8 (SMS_US_GPAPPLICATION + TAPP_OFF_NEXTAREA + 1);
    gs.next_area_raw = (stage << 24) | (scenario << 16);
    gs.movie = attrib_r32(SMS_US_GPAPPLICATION + TAPP_OFF_MOVIE);
    gs.have_app = 1;

    // gpMarDirector (pointer slot). Only capture the pointer value + validity.
    gs.mardirector_ptr = attrib_r32(SMS_US_GPMARDIRECTOR);
    gs.have_mardirector = (gs.mardirector_ptr >= 0x80000000u) ? 1 : 0;

    // gpMarioPos — NOTE: native declares extern gpMarioPos as TVec3<f32>*, so on guest side
    // this slot holds a POINTER to Mario's world-position Vec3, not the Vec3 itself. Read the
    // pointer, then read Vec3 from wherever it points.
    unsigned mario_pos_ptr = attrib_r32(SMS_US_GPMARIOPOS);
    if (mario_pos_ptr >= 0x80000000u) {
        guest_read_vec3f(mario_pos_ptr, gs.mario_pos);
        gs.have_mario_pos = 1;
    }

    // gpMarioOriginal (pointer) + TMario fields.
    gs.mario_ptr = attrib_r32(SMS_US_GPMARIOORIGINAL);
    if (gs.mario_ptr >= 0x80000000u) {
        gs.mario_status       = attrib_r32(gs.mario_ptr + TMARIO_OFF_STATUS);
        gs.mario_anim_id      = guest_r16 (gs.mario_ptr + TMARIO_OFF_ANIMID);
        gs.mario_status_state = guest_r16 (gs.mario_ptr + TMARIO_OFF_STATUSSTATE);
        gs.mario_status_timer = guest_r16 (gs.mario_ptr + TMARIO_OFF_STATUSTIMER);
        gs.mario_motion_frame = 0.f;   // oracle side: no getMotionFrameCtrl accessor; skipped
        gs.have_mario = 1;
    }

    // gpCamera (CPolarSubCamera pointer) + pose. Timers come from gpCameraOption below.
    gs.camera_ptr = attrib_r32(SMS_US_GPCAMERA);
    if (gs.camera_ptr >= 0x80000000u) {
        guest_read_vec3f(gs.camera_ptr + TCAMERA_OFF_POSITION, gs.camera_pos);
        guest_read_vec3f(gs.camera_ptr + TCAMERA_OFF_TARGET,   gs.camera_target);
        guest_read_vec3f(gs.camera_ptr + TCAMERA_OFF_UPVEC,    gs.camera_up);
        gs.camera_mode        = (int)(s32)attrib_r32(gs.camera_ptr + TCAMERA_OFF_MODE);
        gs.camera_fovy        = guest_rf32(gs.camera_ptr + TCAMERA_OFF_FOVY);
        gs.camera_params_ptr  = attrib_r32(gs.camera_ptr + TCAMERA_OFF_CURRPARAMS);
        gs.have_camera = 1;
    }
    // gpCameraOption — TCameraOption where mIntroChaseTimer et al. live. RE'd from
    // ctrlOptionCamera_ decomp (see pin_state_schema.h SMS_US_GPCAMERAOPTION).
    unsigned camopt_ptr = attrib_r32(SMS_US_GPCAMERAOPTION);
    if (camopt_ptr >= 0x80000000u) {
        gs.camera_intro_timer     = guest_r16s(camopt_ptr + TCAMOPT_OFF_INTROTIMER);
        gs.camera_load_pan_frames = guest_r16s(camopt_ptr + TCAMOPT_OFF_LOADPANFR);
        gs.camera_load_pan_timer  = guest_r16s(camopt_ptr + TCAMOPT_OFF_LOADPANTIMER);
    }
    // gpLightManager — the manager instance TLightCommon::setLight reads for GX_LIGHT1.
    gs.lmgr_ptr = attrib_r32(SMS_US_GPLIGHTMANAGER);
    if (gs.lmgr_ptr >= 0x80000000u) {
        guest_read_vec3f(gs.lmgr_ptr + TLMGR_OFF_EFFECT_POS, gs.lmgr_effect_pos);
        gs.lmgr_effect_color   = attrib_r32(gs.lmgr_ptr + TLMGR_OFF_EFFECT_COLOR);
        gs.lmgr_effect_enabled = guest_r8   (gs.lmgr_ptr + TLMGR_OFF_EFFECT_ENABLED);
        gs.lmgr_effect_valid   = guest_r8   (gs.lmgr_ptr + TLMGR_OFF_EFFECT_VALID);
        gs.have_lmgr = 1;
    }
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
    // SUNBRIGHT_DBG_GXAT_WIDTH widens the window past the default 4 frames — needed to attribute a
    // scene reached only after real-time input (e.g. probe-driven Start presses) whose settled frame
    // number isn't known in advance.
    static const long width = [](){ const char* v = std::getenv("SUNBRIGHT_DBG_GXAT_WIDTH"); return v && v[0] ? atol(v) : 4; }();
    return g_frame_no >= at && g_frame_no < at + width;
}

// Safety cap: a normal frame is a few hundred KB; if a copy boundary is ever missed, burst rather
// than grow unbounded (the very first "frame" also folds in all boot-time GX setup — bounded here).
constexpr size_t kCap = 16u << 20;

bool enabled() {
    static int v = -1;
    if (v < 0) {
        const char* p  = getenv("SUNBRIGHT_PARITY_DUMP");
        const char* pt = getenv("SUNBRIGHT_PIN_TICK");
        // Capture is enabled if EITHER the parity dump (per-frame JSONL) OR the pin harness is
        // requested. The pin path needs GxFrameInfo (proj/view/lights/posmtx) parsed at frame
        // boundary — same machinery as parity dump — so the capture gate must be open.
        v = ((p && p[0]) || (pt && pt[0])) ? 1 : 0;
    }
    return v == 1;
}

// SUNBRIGHT_PARITY_DUMP_FROM=N — same-state capture pin. Suppress emission until N VI
// presents have elapsed, so both engines (driven with the same SUNBRIGHT_PAD_SCRIPT /
// SB_PAD_SCRIPT) start emitting at the same game state. Prerequisite for
// parity_sweep.drawdiff to lift its STATE-UNPINNED banner.
long parity_dump_from() {
    static long v = -1;
    if (v < 0) { const char* p = getenv("SUNBRIGHT_PARITY_DUMP_FROM"); v = (p && p[0]) ? atol(p) : 0; }
    return v;
}
bool state_pin_ready(long frame_no) {
    return frame_no >= parity_dump_from();
}

// SUNBRIGHT_PIN_TICK / sb_gx_set_pin_tick(N) — scene-sync harness pin (2026-07-04).
// At GXCopyDisp boundary where g_frame_no == pin_tick, emit scratch/frames/pin_NNNN_oracle.json
// with view (posmtx_efb[main]) + proj (proj_efb[main]) + viewport (vp_efb[main]) + lights.
// Symmetric to native's SB_PIN_TICK path in native/render/sms_boot_present.cpp. Consumed by
// tools/render/pin_diff.py which asserts cross-engine state parity BEFORE any pixel diff.
long g_pin_tick = -1;      // scene-ordinal anchor (drift-prone across engines)
long g_pin_intro = -1;     // game-tick anchor: mIntroChaseTimer == g_pin_intro (preferred)
long g_scene_frame = 0;    // ordinal of the current scene-perspective GXCopyDisp frame
bool g_pin_fired = false;
long pin_tick() {
    static long v = -2;
    if (v == -2) {
        const char* p = getenv("SUNBRIGHT_PIN_TICK");
        v = (p && p[0]) ? atol(p) : -1;
        if (v >= 0) g_pin_tick = v;
    }
    return g_pin_tick;
}
long pin_intro() {
    static long v = -2;
    if (v == -2) {
        const char* p = getenv("SUNBRIGHT_PIN_INTRO");
        v = (p && p[0]) ? atol(p) : -1;
        if (v >= 0) g_pin_intro = v;
    }
    return g_pin_intro;
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
        g_flush_marks.push_back({ g_cap.size(), ppc.pc, ppc.gpr[1], ppc.gpr[31] });
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
    // Same-state pin: suppress emission until frame count >= SUNBRIGHT_PARITY_DUMP_FROM. The
    // frame counter must advance before the emission gate — else the counter never increments
    // and the gate never opens (state_pin_ready → false forever). Do the increment first so the
    // pin is measured in real GXCopyDisp boundaries, not emitted frames.
    g_frame_no++;
    // Scene ordinal: bump when this frame has a perspective (3D) pass. Skips boot / loading /
    // intro-video frames the engines take different amounts of time through.
    if (fi.prims_pass[0] > 0) ++g_scene_frame;
    // Two possible pin anchors:
    //   SUNBRIGHT_PIN_TICK  — scene-ordinal (drifts because oracle burst-ticks game logic
    //                         during boot/turbo while native runs 1:1 tick/scene)
    //   SUNBRIGHT_PIN_INTRO — mIntroChaseTimer value at gpCameraOption + 0x0A. Both engines
    //                         reach the same value regardless of ordinal drift — this is the
    //                         true GAME-TICK anchor for the title scene-sync.
    long pt = pin_tick(), pi = pin_intro();
    bool tick_hit  = (pt >= 0 && fi.prims_pass[0] > 0 && g_scene_frame == pt);
    bool intro_hit = false;
    long intro_val = 0;
    if (pi >= 0 && fi.prims_pass[0] > 0) {
        unsigned camopt_ptr = attrib_r32(SMS_US_GPCAMERAOPTION);
        if (camopt_ptr >= 0x80000000u) {
            intro_val = guest_r16s(camopt_ptr + TCAMOPT_OFF_INTROTIMER);
            if (intro_val == pi) intro_hit = true;
        }
    }
    long pin_key = intro_hit ? pi : (tick_hit ? pt : -1);
    if (pin_key >= 0 && !g_pin_fired) {
        // Pick the MAIN scene EFB pass: the one whose posMtx is a REAL camera view (non-identity).
        // The other perspective passes (TMirrorCamera pre-pass at FOVy=52°, sky/dome pass at 40°
        // with world-space geometry) use identity posMtx — they're not the pass whose view we're
        // syncing against native's C_MTXLookAt output. Scan LAST-first so the final scene pass
        // wins over any earlier auxiliary pass that also happens to carry a non-identity view.
        auto is_identity_view = [](const float v[12]) {
            return v[0] == 1.f && v[5] == 1.f && v[10] == 1.f
                && v[1] == 0.f && v[2] == 0.f && v[3] == 0.f
                && v[4] == 0.f && v[6] == 0.f && v[7] == 0.f
                && v[8] == 0.f && v[9] == 0.f && v[11] == 0.f;
        };
        int mainEfb = -1;
        for (int i = 7; i >= 0; --i) {
            if (!fi.proj_efb_set[i] || !fi.posmtx_efb_set[i] || fi.proj_type_efb[i] != 0) continue;
            if (is_identity_view(fi.posmtx_efb[i])) continue;
            mainEfb = i; break;
        }
        // Fallback 1: any perspective efb pass with proj+posmtx (accept identity as last resort).
        if (mainEfb < 0) {
            for (int i = 7; i >= 0; --i) {
                if (fi.proj_efb_set[i] && fi.posmtx_efb_set[i] && fi.proj_type_efb[i] == 0) { mainEfb = i; break; }
            }
        }
        int projType = 0;
        float view[12] = {0}, proj6[6] = {0}, vp6[6] = {0};
        if (mainEfb >= 0) {
            projType = fi.proj_type_efb[mainEfb];
            std::memcpy(view, fi.posmtx_efb[mainEfb], sizeof(view));
            std::memcpy(proj6, fi.proj_efb[mainEfb], sizeof(proj6));
            std::memcpy(vp6, fi.vp_efb[mainEfb], sizeof(vp6));
        } else if (fi.proj_pass_set[0]) {   // per-pass scene fallback
            projType = fi.proj_type_pass[0];
            std::memcpy(view, fi.posmtx_pass[0], sizeof(view));
            std::memcpy(proj6, fi.proj_pass[0], sizeof(proj6));
            std::memcpy(vp6, fi.vp_pass[0], sizeof(vp6));
        }
        int nlt = 0;
        for (int i = 0; i < 8; ++i) if (fi.lights[i].valid) nlt++;

        ::mkdir("scratch", 0755); ::mkdir("scratch/frames", 0755);
        char path[192];
        std::snprintf(path, sizeof path, "scratch/frames/pin_%04ld_oracle.json", pin_key);
        FILE* pf = std::fopen(path, "w");
        if (pf) {
            std::fprintf(pf, "{\n  \"engine\": \"oracle\",\n  \"tick\": %ld,\n"
                             "  \"efb_pass\": %d,\n  \"proj_type\": %d,\n",
                         pin_key, mainEfb, projType);
            std::fprintf(pf, "  \"view\": [");
            for (int i = 0; i < 12; ++i) std::fprintf(pf, "%s%.9g", i ? "," : "", view[i]);
            std::fprintf(pf, "],\n  \"proj6\": [");
            for (int i = 0; i < 6; ++i)  std::fprintf(pf, "%s%.9g", i ? "," : "", proj6[i]);
            std::fprintf(pf, "],\n  \"vp\": [");
            for (int i = 0; i < 6; ++i)  std::fprintf(pf, "%s%.9g", i ? "," : "", vp6[i]);
            // ── Game-state fingerprint from guest RAM. Symmetric with native pin JSON. ──
            SbPinGameState gs{};
            oracle_populate_pin_state(gs);
            std::fprintf(pf, "],\n  \"have_state\": 1,\n");
            std::fprintf(pf, "  \"app\": {\"have\": %u, \"appState\": %u, \"nextArea\": %u, \"movie\": %u},\n",
                         gs.have_app, gs.app_state, gs.next_area_raw, gs.movie);
            std::fprintf(pf, "  \"mardirector\": {\"have\": %u, \"ptr\": %u},\n",
                         gs.have_mardirector, gs.mardirector_ptr);
            std::fprintf(pf, "  \"marioPos\": {\"have\": %u, \"pos\": [%.6g,%.6g,%.6g]},\n",
                         gs.have_mario_pos, gs.mario_pos[0], gs.mario_pos[1], gs.mario_pos[2]);
            std::fprintf(pf, "  \"mario\": {\"have\": %u, \"ptr\": %u, \"status\": %u, \"animId\": %u, "
                              "\"statusState\": %u, \"statusTimer\": %u, \"motionFrame\": %.6f},\n",
                         gs.have_mario, gs.mario_ptr, gs.mario_status, gs.mario_anim_id,
                         gs.mario_status_state, gs.mario_status_timer, gs.mario_motion_frame);
            std::fprintf(pf, "  \"camera\": {\"have\": %u, \"ptr\": %u, \"paramsPtr\": %u, "
                              "\"pos\": [%.6g,%.6g,%.6g], \"target\": [%.6g,%.6g,%.6g], "
                              "\"up\": [%.6g,%.6g,%.6g], \"mode\": %d, \"fovy\": %.6g, "
                              "\"introTimer\": %d, \"loadPanFrames\": %d, \"loadPanTimer\": %d},\n",
                         gs.have_camera, gs.camera_ptr, gs.camera_params_ptr,
                         gs.camera_pos[0], gs.camera_pos[1], gs.camera_pos[2],
                         gs.camera_target[0], gs.camera_target[1], gs.camera_target[2],
                         gs.camera_up[0], gs.camera_up[1], gs.camera_up[2],
                         gs.camera_mode, gs.camera_fovy,
                         gs.camera_intro_timer, gs.camera_load_pan_frames, gs.camera_load_pan_timer);
            std::fprintf(pf, "  \"lmgr\": {\"have\": %u, \"ptr\": %u, \"effectPos\": [%.6g,%.6g,%.6g], "
                              "\"effectColor\": %u, \"effectEnabled\": %u, \"effectValid\": %u},\n",
                         gs.have_lmgr, gs.lmgr_ptr,
                         gs.lmgr_effect_pos[0], gs.lmgr_effect_pos[1], gs.lmgr_effect_pos[2],
                         gs.lmgr_effect_color, gs.lmgr_effect_enabled, gs.lmgr_effect_valid);
            std::fprintf(pf, "  \"lights\": [");
            int emitted = 0;
            for (int i = 0; i < 8; ++i) if (fi.lights[i].valid) {
                if (emitted++) std::fprintf(pf, ",");
                std::fprintf(pf, "\n    {\"idx\": %d, \"col\": [%.6g,%.6g,%.6g], "
                                  "\"pos\": [%.6g,%.6g,%.6g]}",
                             i, fi.lights[i].color[0], fi.lights[i].color[1], fi.lights[i].color[2],
                             fi.lights[i].pos[0],   fi.lights[i].pos[1],   fi.lights[i].pos[2]);
            }
            std::fprintf(pf, "\n  ],\n  \"amb\": [%.6g,%.6g,%.6g]\n}\n",
                         fi.amb[0], fi.amb[1], fi.amb[2]);
            std::fclose(pf);
            char donepath[192];
            std::snprintf(donepath, sizeof donepath, "scratch/frames/pin_%04ld_oracle.done", pin_key);
            if (FILE* d = std::fopen(donepath, "w")) {
                std::fprintf(d, "pin key=%ld anchor=%s scene#%ld frame_no=%ld introTimer=%ld ok\n",
                             pin_key, intro_hit ? "intro" : "tick",
                             g_scene_frame, g_frame_no, intro_val);
                std::fclose(d);
            }
            std::fprintf(stderr, "[gxcap-pin] key=%ld anchor=%s scene#%ld intro=%ld efb=%d "
                                "proj[0]=%.4f view[0]=%.3f lights=%d -> %s\n",
                         pin_key, intro_hit ? "intro" : "tick",
                         g_scene_frame, intro_val,
                         mainEfb, proj6[0], view[0], nlt, path);
        } else {
            std::fprintf(stderr, "[gxcap-pin] cannot open %s\n", path);
        }
        g_pin_fired = true;
    }
    // Only emit frames with real geometry — a blank/2D-only frame would pollute the settled-window
    // medians parity_sweep computes (it already skips onscr==0, but prims==0 is the cleaner gate).
    if (!f || fi.prims == 0) return;
    if (!state_pin_ready(g_frame_no)) return;

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
        // Game-state fingerprint (TApplication::mAppState @ gpApplication+0x8, US GMSE01 =
        // 0x803E9700). Emitted BEFORE the projection fingerprint so parity_sweep can gate
        // STATE-MISMATCH before touching proj values. One byte in guest RAM, read via
        // g_ram_base — same helper attrib_r32 uses. When the guest hasn't populated it yet
        // (pre-boot) it reads 0 = APP_STATE_WAIT, which matches native's zero-init default.
        {
            unsigned app_state = 0xFFFFu;
            if (g_ram_base) {
                app_state = (unsigned)g_ram_base[(0x803E9700u + 0x8) & 0x01FFFFFFu];
            }
            std::fprintf(f, ",\"appState\":%u", app_state);
        }
        // FINGERPRINT (game-state signature — SETPROJECTION + SETVIEWPORT bytes from THIS frame's
        // FIFO stream, parsed by gx_parse). Both engines run identical guest code that sets
        // projection/viewport, so at the same game state these bytes match. parity_sweep.drawdiff
        // uses this fingerprint to pair frames across engines and downgrade the STATE-UNPINNED
        // banner. Read PER-PASS (proj_pass/vp_pass): the frame-global fi.proj/fi.vp is "last seen"
        // (= HUD ortho at end-of-frame), so it would put HUD state on the SCENE line and never
        // match native's true scene projection. proj_type is the REAL GX type (0=persp/1=ortho),
        // NOT the pass loop index — the coincidence perspective==scene==0 hid this bug but was
        // fragile. Emit only when the pass actually captured a projection/viewport.
        if (fi.proj_pass_set[pass]) {
            std::fprintf(f, ",\"projType\":%d,\"proj\":[%.5f,%.5f,%.5f,%.5f,%.5f,%.5f]",
                         fi.proj_type_pass[pass],
                         fi.proj_pass[pass][0], fi.proj_pass[pass][1], fi.proj_pass[pass][2],
                         fi.proj_pass[pass][3], fi.proj_pass[pass][4], fi.proj_pass[pass][5]);
        } else {
            std::fprintf(f, ",\"projType\":%d", pass);   // legacy fallback: pass index
        }
        if (fi.vp_pass_set[pass]) {
            std::fprintf(f, ",\"vp\":[%.1f,%.1f,%.1f,%.1f,%.4f,%.4f]",
                         fi.vp_pass[pass][0], fi.vp_pass[pass][1], fi.vp_pass[pass][2],
                         fi.vp_pass[pass][3], fi.vp_pass[pass][4], fi.vp_pass[pass][5]);
        }
        // Sky #16 dome projection audit (2026-07-04): emit the pass-first XFmem PNMTX0 (row-major
        // 3x4). The oracle's live PNMTX0 at first primitive is the camera/view matrix the GPU
        // applies to positions before the projection. A native-vs-oracle diff of this 3x4 + proj +
        // vp names the divergence in the view/camera chain (dome renders bright-blue on native vs
        // muted on oracle → different verts land under the same pixel → matrix delta).
        if (fi.posmtx_pass_set[pass]) {
            std::fprintf(f, ",\"posMtx\":[%.6f,%.6f,%.6f,%.4f,%.6f,%.6f,%.6f,%.4f,%.6f,%.6f,%.6f,%.4f]",
                         fi.posmtx_pass[pass][ 0], fi.posmtx_pass[pass][ 1], fi.posmtx_pass[pass][ 2], fi.posmtx_pass[pass][ 3],
                         fi.posmtx_pass[pass][ 4], fi.posmtx_pass[pass][ 5], fi.posmtx_pass[pass][ 6], fi.posmtx_pass[pass][ 7],
                         fi.posmtx_pass[pass][ 8], fi.posmtx_pass[pass][ 9], fi.posmtx_pass[pass][10], fi.posmtx_pass[pass][11]);
        }
        // Sky #16 dome projection audit (2026-07-04): per-EFB-pass matrices for the scene pass —
        // on SMS title, efb 0 = mirror pre-pass @ 52° (TMirrorCamera 1.3×), efb 1 = main scene @
        // 40° (where sky.bmd's dome draws). Emit as parallel arrays so a tool picks the right pass.
        if (scene && fi.max_efb_pass > 0) {
            std::fprintf(f, ",\"efbProj\":[");
            for (u32 k = 0; k <= fi.max_efb_pass && k < 8; ++k) {
                if (k) std::fprintf(f, ",");
                if (!fi.proj_efb_set[k]) { std::fprintf(f, "null"); continue; }
                std::fprintf(f, "[%.5f,%.5f,%.5f,%.5f,%.5f,%.5f]",
                             fi.proj_efb[k][0], fi.proj_efb[k][1], fi.proj_efb[k][2],
                             fi.proj_efb[k][3], fi.proj_efb[k][4], fi.proj_efb[k][5]);
            }
            std::fprintf(f, "],\"efbVp\":[");
            for (u32 k = 0; k <= fi.max_efb_pass && k < 8; ++k) {
                if (k) std::fprintf(f, ",");
                if (!fi.vp_efb_set[k]) { std::fprintf(f, "null"); continue; }
                std::fprintf(f, "[%.2f,%.2f,%.2f,%.2f,%.4f,%.4f]",
                             fi.vp_efb[k][0], fi.vp_efb[k][1], fi.vp_efb[k][2],
                             fi.vp_efb[k][3], fi.vp_efb[k][4], fi.vp_efb[k][5]);
            }
            std::fprintf(f, "],\"efbPosMtx\":[");
            for (u32 k = 0; k <= fi.max_efb_pass && k < 8; ++k) {
                if (k) std::fprintf(f, ",");
                if (!fi.posmtx_efb_set[k]) { std::fprintf(f, "null"); continue; }
                std::fprintf(f, "[%.6f,%.6f,%.6f,%.4f,%.6f,%.6f,%.6f,%.4f,%.6f,%.6f,%.6f,%.4f]",
                             fi.posmtx_efb[k][ 0], fi.posmtx_efb[k][ 1], fi.posmtx_efb[k][ 2], fi.posmtx_efb[k][ 3],
                             fi.posmtx_efb[k][ 4], fi.posmtx_efb[k][ 5], fi.posmtx_efb[k][ 6], fi.posmtx_efb[k][ 7],
                             fi.posmtx_efb[k][ 8], fi.posmtx_efb[k][ 9], fi.posmtx_efb[k][10], fi.posmtx_efb[k][11]);
            }
            std::fprintf(f, "]");
        }
        std::fprintf(f, ",\"lights\":{\"n\":%d,\"l\":[", scene ? ln : 0);
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
        // ── Per-draw ordered record (opt-in, SUNBRIGHT_PARITY_DRAWS=1) ────────────────────────────
        // Emit ONE JSON object per DrawRec in stream/draw order so parity_sweep can do an ordered-
        // position cross-engine diff and NAME the first divergent draw call (per 2026-07-02
        // workflow directive step #2). Same idea as sms-boot's per-batch batches[] emission but
        // keyed by ordered-position, not by grouped shader key (nvk shaderKey is renderer-specific
        // and not reconstructible on the oracle side). Fields chosen to survive cross-engine:
        // efb_pass, proj_type, blend {enable,src,dst,subtract}, color/alpha update, tev-stage count,
        // vertex count. The parity_sweep diff will match by position first, then use these fields
        // as the NAMED divergence anchors.
        //
        // Opt-in: only emit when SUNBRIGHT_PARITY_DRAWS is set (avoids inflating the JSONL when only
        // the coarse per-pass summary is wanted). The per-pass summary lines above are unchanged.
        //
        // Emitted for the CURRENT pass only (matches the per-pass summary line that carries it).
        // We loop draws.filter(d.proj_type==pass) so a per-pass "draws":[…] tail attaches to its
        // pass's summary line.
        if (std::getenv("SUNBRIGHT_PARITY_DRAWS") && !fi.draws.empty()) {
            std::fprintf(f, ",\"draws\":[");
            int em = 0;
            const bool haveLight = fi.light_snaps.size() == fi.draws.size();
            for (size_t di = 0; di < fi.draws.size(); ++di) {
                const auto& d = fi.draws[di];
                if ((int)d.proj_type != pass) continue;
                std::fprintf(f,
                    "%s{\"i\":%u,\"efb\":%u,\"proj\":%u,\"be\":%u,\"src\":%u,\"dst\":%u,\"sub\":%u,"
                    "\"cU\":%u,\"aU\":%u,\"tev\":%u,\"v\":%u,\"imm\":%u",
                    em++ ? "," : "", d.offset, d.efb_pass, d.proj_type, d.blend_enable,
                    d.src, d.dst, d.subtract, d.color_update, d.alpha_update, d.numtevstages,
                    d.prims, d.immediate);
                // Per-draw LIGHTING snapshot (SUNBRIGHT_DBG_GXLIGHT). This is the ORACLE side of
                // the overbright wash diagnostic — captures the exact chan_ctrl / amb / matc /
                // lights state feeding the raster stage at the moment of THIS draw. Native's
                // per-batch batchtev output emits the same fields, so a per-shape diff pinpoints
                // which pipeline stage native diverges on.
                if (haveLight) {
                    const auto& L = fi.light_snaps[di];
                    std::fprintf(f, ",\"cc\":%u,\"amb\":[%.3f,%.3f,%.3f],\"matc\":[%.3f,%.3f,%.3f,%.3f],\"lm\":%u,\"lights\":[",
                                 L.chan0_ctrl, L.amb[0], L.amb[1], L.amb[2],
                                 L.matc[0], L.matc[1], L.matc[2], L.matc[3], L.light_valid);
                    int lem = 0;
                    for (int li = 0; li < 8; ++li) {
                        if (!(L.light_valid & (1u << li))) continue;
                        std::fprintf(f, "%s{\"i\":%d,\"p\":[%.1f,%.1f,%.1f],\"c\":[%.3f,%.3f,%.3f]}",
                                     lem++ ? "," : "", li,
                                     L.light_pos[li][0], L.light_pos[li][1], L.light_pos[li][2],
                                     L.light_col[li][0], L.light_col[li][1], L.light_col[li][2]);
                    }
                    std::fprintf(f, "]");
                }
                std::fprintf(f, "}");
            }
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
        // DISTINCT J3DShape INSTANCE COUNT: when a flush lands directly inside J3DShape::draw
        // (802e0390), its prologue (verified via --disasm: `mflr r0; stw r0,4(r1); stwu r1,-0x30
        // (r1); stw r31,0x2c(r1); or r31,r3,r3; ...`) spills the CALLER's old r31 to the stack
        // FIRST, then only AFTER that does `or r31,r3,r3` (mr r31,this) load `this` into r31 — so
        // the stack slot [r1+0x2c] holds the caller's r31, NOT `this` (an earlier version of this
        // comment/code wrongly assumed otherwise; caught because every "shape" it named repeatedly
        // resolved to the SAME low, .text-range addresses across every frame — real heap objects
        // don't do that). `this` lives in the LIVE register r31 for the rest of the function's
        // execution instead, so FlushMark now also captures ppc.gpr[31] at the moment of the flush
        // (a live-register read, not a stack read) — that's the actual J3DShape* instance. Counting
        // DISTINCT pointer values (not just verts) answers "how many separate shapes does the
        // oracle actually draw" directly, comparable against native's own per-buffer shape counts
        // (SB_DRAWBUF_INV packet counts) without needing to name each shape's owning model.
        std::set<unsigned> shape_this_ptrs;
        for (size_t i = 0; i < fi.draws.size(); ++i) {
            const auto& d = fi.draws[i];
            if (d.proj_type != 0 || d.prims == 0) continue;   // scene (perspective) draws only
            const FlushMark* best = nullptr;
            for (const auto& m : g_flush_marks)
                if (m.off <= d.offset && (!best || m.off > best->off)) best = &m;
            std::string who = best ? attrib_sym(best->pc) : std::string("?");
            if (best) {
                // Prefer the OUTERMOST "perform" frame over the innermost "draw" frame. J3DShape::draw/
                // J3DDisplayListObj::callDL/etc are ONE shared virtual-dispatch function called by every
                // shape instance in the scene, so naming the innermost "draw" match (the old 6-frame,
                // first-match behavior) collapses hundreds of distinct objects (map, sky, palm tree,
                // decorative statics, water, chr) into a handful of generic buckets — useless for finding
                // which SPECIFIC object a native under-draw is missing. Each object's own ...::perform()
                // (TMap::perform, TMapObjWave::perform, TDrawBufObj::perform, TSky::perform, a specific
                // TMapStaticObj subclass's perform, ...) is a DISTINCT mangled symbol per class, so it
                // identifies the actual object. Walk deeper (16 frames) and take the LAST "perform" match
                // seen (i.e. the outermost/most specific caller), falling back to the innermost "draw"
                // match only if no "perform" frame was found in the walked range.
                std::string perform_match, draw_match;
                bool nested_in_shape_draw = (who.rfind("draw__8J3DShapeCFv", 0) == 0);
                unsigned fp = best->sp;
                for (int fr = 0; fr < 16 && fp >= 0x80000000u && fp < 0x81800000u; fr++) {
                    std::string s = attrib_sym(attrib_r32(fp + 4));
                    if (s.rfind("draw__8J3DShapeCFv", 0) == 0) nested_in_shape_draw = true;
                    if (s.find("perform") != std::string::npos) perform_match = s;   // keep overwriting -> outermost wins
                    else if (draw_match.empty() &&
                             (s.find("draw") != std::string::npos || s.find("Draw") != std::string::npos))
                        draw_match = s;   // keep innermost draw as a fallback only
                    unsigned nx = attrib_r32(fp); if (nx <= fp) break; fp = nx;
                }
                // r31 is callee-saved by ABI convention: whether the flush PC landed directly inside
                // J3DShape::draw's own body, or inside a leaf helper it CALLED (WriteMTXPS4x3,
                // GXLoadPosMtxImm, loadPosMtxIndx/loadNrmMtxIndx, callDL, SMS_InitPacket_OneTevColor —
                // none of which use r31 for their own purposes), r31 still holds J3DShape::draw's `this`
                // as long as we can prove via the back-chain that J3DShape::draw is somewhere on the
                // call stack (nested_in_shape_draw, checked above via both the direct match and the walk).
                // Without this broadening the shape count only saw draws whose flush PC landed EXACTLY
                // inside J3DShape::draw itself — a small minority — undercounting badly (measured: 21-28
                // per single-frame sample before, most flushes actually land in the generic helpers).
                if (nested_in_shape_draw) {
                    unsigned this_ptr = best->r31;
                    if (this_ptr >= 0x80000000u && this_ptr < 0x81800000u) shape_this_ptrs.insert(this_ptr);
                }
                if (!perform_match.empty()) who = perform_match;
                else if (!draw_match.empty()) who = draw_match;
            }
            sumv[who] += d.prims;
        }
        std::vector<std::pair<std::string,unsigned long>> v(sumv.begin(), sumv.end());
        std::sort(v.begin(), v.end(), [](const std::pair<std::string,unsigned long>&a,
                                         const std::pair<std::string,unsigned long>&b){ return a.second > b.second; });
        fprintf(stderr, "[gxsum] frame %ld top scene draws by raw-verts (total scene tris=%u, distinct J3DShape this-ptrs=%zu):\n",
                g_frame_no, fi.tris_pass[0], shape_this_ptrs.size());
        for (size_t i = 0; i < v.size() && i < 16; i++)
            fprintf(stderr, "  %8lu  %s\n", v[i].second, v[i].first.c_str());
        if (getenv("SUNBRIGHT_GX_ATTRIB_SHAPES")) {
            fprintf(stderr, "[gxshapes] frame %ld this-ptrs:", g_frame_no);
            for (unsigned p : shape_this_ptrs) fprintf(stderr, " %08x", p);
            fprintf(stderr, "\n");
        }
    }
    // SUNBRIGHT_LOG_CU_WRITERS: name the guest function behind every on-wire GXSetColorUpdate
    // transition this frame, by correlating each BPMEM_BLENDMODE cU-toggle offset to the nearest
    // preceding gather-flush FlushMark. Pairs with sms-boot's SB_COLUPD_ALL — diffing the two lists
    // names the dispatch path that fires cU=FALSE on the oracle but not on native (the file-select
    // overbright's smoking gun: 3-4k depth-only cU=0 prepass draws per frame on GC, ZERO on native).
    if (attrib_enabled() && gx_dbg_window() && std::getenv("SUNBRIGHT_LOG_CU_WRITERS")
        && !fi.cu_writes.empty()) {
        int n_false = 0, n_true = 0;
        std::map<std::string, unsigned long> false_by_sym;
        for (const auto& w : fi.cu_writes) {
            if (w.new_cU == 0) ++n_false; else ++n_true;
            const FlushMark* best = nullptr;
            for (const auto& m : g_flush_marks)
                if (m.off <= w.offset && (!best || m.off > best->off)) best = &m;
            const unsigned pc = best ? best->pc : 0u;
            std::string sym = best ? attrib_sym(pc) : std::string("?");
            // Hoist attribution to the outermost meaningful caller (same pattern as [gxsum]): the
            // gather-pipe flush PC often lands deep in a GX runtime helper (WriteMTXPS4x3,
            // __GXXfVtxSpecs, GXLoadPosMtxImm, GXBegin, __GXSetDirtyState) that no game code
            // directly programs. Walk the back-chain up to 16 frames; prefer an outer "perform"
            // frame, else an outer "draw"/"Draw" frame, else keep the raw sym. That surfaces
            // TMBindShadowManager::drawShadow / TMario::perform / TModelWaterManager::drawMirror
            // etc as the real writers of cU=FALSE, matching sms-boot's SB_COLUPD_ALL callers.
            if (best) {
                // Prefer the INNERMOST specific actor over the outermost dispatch: for cU-writer
                // attribution the useful signal is which SCENE ACTOR toggled cU (TMBindShadowManager,
                // TModelWaterManager, TMario, SMS_FillScreenAlpha, TEfbCtrl, …), not "the perform-list
                // dispatched something." So skip TPerformList::perform (the outer dispatcher) and stop
                // at the FIRST inner perform/draw/Fill match encountered walking from the flush PC
                // outward. If the flush PC itself already resolves to a specific-actor perform (not
                // TPerformList), keep it as-is.
                auto is_specific = [](const std::string& s){
                    if (s.rfind("perform__12TPerformList", 0) == 0) return false;   // outer dispatcher
                    return s.find("perform") != std::string::npos ||
                           s.find("draw") != std::string::npos ||
                           s.find("Draw") != std::string::npos ||
                           s.find("SMS_Fill") != std::string::npos;
                };
                if (!is_specific(sym)) {
                    unsigned fp = best->sp;
                    for (int fr = 0; fr < 16 && fp >= 0x80000000u && fp < 0x81800000u; fr++) {
                        std::string s = attrib_sym(attrib_r32(fp + 4));
                        if (is_specific(s)) { sym = s; break; }
                        unsigned nx = attrib_r32(fp); if (nx <= fp) break; fp = nx;
                    }
                }
            }
            if (w.new_cU == 0) false_by_sym[sym]++;
            fprintf(stderr, "[cU-writer] fr=%ld off=%u %s pc=%08x sym=%s\n",
                    g_frame_no, w.offset,
                    w.new_cU ? "cU=1" : "cU=0",
                    pc, sym.c_str());
        }
        fprintf(stderr, "[cU-writer-sum] fr=%ld total=%zu false=%d true=%d\n",
                g_frame_no, fi.cu_writes.size(), n_false, n_true);
        if (n_false > 0) {
            std::vector<std::pair<std::string, unsigned long>> v(false_by_sym.begin(), false_by_sym.end());
            std::sort(v.begin(), v.end(), [](const auto& a, const auto& b){ return a.second > b.second; });
            fprintf(stderr, "[cU-writer-false-by-caller] fr=%ld\n", g_frame_no);
            for (size_t i = 0; i < v.size() && i < 16; ++i)
                fprintf(stderr, "  %6lu  %s\n", v[i].second, v[i].first.c_str());
        }
    }
    g_flush_marks.clear();
#endif
    std::fflush(f);
    g_emitted++;
    if (dbg() && (g_emitted % 128) == 0)
        fprintf(stderr, "[gxcap] emitted=%lu boundaries=%lu parse_fail=%lu last prims=%u dls=%u lights=%d proj=%d\n",
                g_emitted, g_boundaries, g_parse_fail, fi.prims, fi.display_lists, ln, fi.proj_type);
}

// Pin-tick control for the scene-sync harness. Set from /pinshot?vi=N (probe_server.cpp).
// The setter arms the one-shot; the getter lets the endpoint poll for completion.
extern "C" void sb_gx_set_pin_tick(long tick) {
    g_pin_tick = tick;
    g_pin_fired = false;
    fprintf(stderr, "[gxcap-pin] armed pin_tick=%ld\n", tick);
}
extern "C" long sb_gx_get_pin_tick(void) { return g_pin_tick; }
extern "C" int  sb_gx_pin_fired(void)    { return g_pin_fired ? 1 : 0; }
extern "C" long sb_gx_get_frame_no(void) { return g_frame_no; }
