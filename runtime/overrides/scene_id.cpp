// 2D-element identification (SUNBRIGHT_2DID=1).
//
// To do per-element widescreen (centre overlays, fill fades, edge-anchor HUD) we first have to know
// WHICH element is which. This wraps the J2D leaf/scene draws and logs, per drawn element: the frame
// number, the element type, its object pointer (a stable per-object ID — objects live at fixed heap
// addresses), and its screen rect — taken straight from the draw ARGUMENTS (no struct RE needed):
//   J2DScreen::draw(this, x, y, grafctx)         — top-level 2D screen
//   J2DPicture::draw(this, x, y, w, h, b, b, b)  — image element (rect = r4..r7)
//   J2DTextBox::draw(this, x, y)                 — text element
// A full-screen rect (0,0,640,448) is a backdrop/fade; a small corner rect is HUD; etc. Output goes
// to scratch/2d_elements/elements.log, grouped by frame, so it lines up with the SUNBRIGHT_DUMP PNGs
// (use tools/crop_2d_elements.py to cut each element's rect out of the frame dump → a PNG of itself).
//
// Off by default (gated on SUNBRIGHT_2DID); wraps are pure super-calls, so rendering is unchanged.

#include "../overrides.h"
#include "../intrinsics.h"
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <string>
#include <algorithm>
#include <sys/stat.h>

#ifdef HAVE_DOLPHIN_CORE
#  include "VideoCommon/VideoEvents.h"
#endif

namespace {

// The screen draws its child panes through their per-pane drawSelf(x, y, Mtx) — that's where each
// individual element (image / text) actually renders, so hook those, not the standalone draw().
constexpr u32 J2DSCREEN_DRAW      = 0x802cfda8u;   // J2DScreen::draw(int x, int y, const J2DGrafContext*) — top level
constexpr u32 J2DPICTURE_DRAWSELF = 0x802cc7c0u;   // J2DPicture::drawSelf(int x, int y, Mtx) — image element
constexpr u32 J2DTEXTBOX_DRAWSELF = 0x802d0dd8u;   // J2DTextBox::drawSelf(int x, int y, Mtx) — text element

bool g_on = false;
FILE* g_log = nullptr;
std::atomic<uint64_t> g_frame{0};
#ifdef HAVE_DOLPHIN_CORE
Common::EventHook g_field_hook;
#endif

// Subscribe to the per-field event LAZILY (on the first draw, when Dolphin's video globals are
// constructed) — NOT in a static initializer, where GetVideoEvents() runs before System exists
// and corrupts memory.
void ensure_frame_hook() {
#ifdef HAVE_DOLPHIN_CORE
    static bool done = false;
    if (done) return;
    done = true;
    g_field_hook = GetVideoEvents().vi_end_field_event.Register([] { g_frame.fetch_add(1); });
#endif
}

// Log one element. We de-dup within a frame and only write a frame's inventory ONCE its set of
// element IDs changes (the same screen redraws the same panes every frame), so the log is a compact
// list of "this screen contains these elements" rather than thousands of identical lines.
std::string g_frame_set, g_last_set;
// One-time per-ID struct dump (SUNBRIGHT_2DID_STRUCT) to find the bounds offset + name tag.
std::string g_seen_struct;
void dump_struct_once(const char* type, u32 id) {
    if (!g_log || !getenv("SUNBRIGHT_2DID_STRUCT")) return;
    if (id < 0x80000000u || id >= 0x81800000u) return;
    char key[12]; std::snprintf(key, sizeof key, "%08x", id);
    if (g_seen_struct.find(key) != std::string::npos) return;
    g_seen_struct += key;
    std::fprintf(g_log, "STRUCT %-8s id=%08x  hex:", type, id);
    for (u32 o = 0; o <= 0x60; o += 4) std::fprintf(g_log, " %08x", mem_r32(id + o));
    std::fprintf(g_log, "\n              flt:");
    for (u32 o = 0; o <= 0x60; o += 4) std::fprintf(g_log, " %.1f", mem_rf32(id + o));
    std::fprintf(g_log, "\n");
    std::fflush(g_log);
}

// J2DPane layout (decoded): +0x08 type fourCC, +0x10 NAME fourCC (the layout tag — "titl","s_01"…),
// +0x14/+0x18/+0x1c/+0x20 bounds x0/y0/x1/y1 (s32, 640×480 space). Name + rect make a pane usable.
void pane_name(u32 id, char out[5]) {
    u32 t = mem_r32(id + 0x10);
    for (int i = 0; i < 4; i++) { u8 c = (t >> (24 - i * 8)) & 0xff; out[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.'; }
    out[4] = 0;
}

void log_elem(const char* type, u32 id) {
    ensure_frame_hook();
    dump_struct_once(type, id);
    char e[80];
    if (id >= 0x80000000u && id < 0x81800000u) {
        char nm[5]; pane_name(id, nm);
        // +0x14..0x20 is the pane-LOCAL rect; +0x24..0x30 is the GLOBAL (absolute screen) rect after
        // layout — for a nested pane these differ (e.g. shn0 local (27,7) vs global (57,191)). Use
        // the global one so the rect is a real screen position (and the crop tool lines up).
        const s32 x0 = (s32)mem_r32(id + 0x24), y0 = (s32)mem_r32(id + 0x28),
                  x1 = (s32)mem_r32(id + 0x2c), y1 = (s32)mem_r32(id + 0x30);
        std::snprintf(e, sizeof e, "  %-8s '%s' id=%08x rect=(%d,%d %dx%d)\n",
                      type, nm, id, x0, y0, x1 - x0, y1 - y0);
    } else {
        std::snprintf(e, sizeof e, "  %-8s id=%08x\n", type, id);
    }
    g_frame_set += e;
}
void flush_frame_set() {
    if (g_frame_set != g_last_set && !g_frame_set.empty() && g_log) {
        std::fprintf(g_log, "── frame %llu ── %zu elements ──\n%s",
                     (unsigned long long)g_frame.load(),
                     (size_t)std::count(g_frame_set.begin(), g_frame_set.end(), '\n'),
                     g_frame_set.c_str());
        std::fflush(g_log);
        g_last_set = g_frame_set;
    }
    g_frame_set.clear();
}

// J2DScreen::draw is the top of a 2D pass — flush the previous pass's collected element set, then
// this screen's panes (Picture/TextBox drawSelf) append to the new set as they render.
void ov_screen(CPUState& cpu) {
    flush_frame_set();
    log_elem("Screen", cpu.gpr[3]);
    if (RecompFunc o = recomp_raw(J2DSCREEN_DRAW)) o(cpu); else call_ppc(cpu, cpu.lr);
}
void ov_picture(CPUState& cpu) {
    log_elem("Picture", cpu.gpr[3]);
    if (RecompFunc o = recomp_raw(J2DPICTURE_DRAWSELF)) o(cpu); else call_ppc(cpu, cpu.lr);
}
void ov_textbox(CPUState& cpu) {
    log_elem("TextBox", cpu.gpr[3]);
    if (RecompFunc o = recomp_raw(J2DTEXTBOX_DRAWSELF)) o(cpu); else call_ppc(cpu, cpu.lr);
}

}  // namespace

static const bool s_2did_registered = [] {
    if (!getenv("SUNBRIGHT_2DID")) return true;
    g_on = true;
    (void)mkdir("scratch", 0755);
    (void)mkdir("scratch/2d_elements", 0755);
    g_log = std::fopen("scratch/2d_elements/elements.log", "w");
    register_override(J2DSCREEN_DRAW,      &ov_screen);
    register_override(J2DPICTURE_DRAWSELF, &ov_picture);
    register_override(J2DTEXTBOX_DRAWSELF, &ov_textbox);
    // (the per-field frame counter is subscribed lazily in ensure_frame_hook() — see above)
    std::fprintf(stderr, "[2did] 2D-element identification ON → scratch/2d_elements/elements.log\n");
    return true;
}();
