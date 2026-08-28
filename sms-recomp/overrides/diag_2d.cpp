// diag_2d.cpp — identify WHICH 2D class draws a given on-screen element.
//
// Enabled with SBR_DIAG_2D=1; inert otherwise (the overrides are registered but pass straight
// through, and these are hot J2D entry points).
//
// The widescreen work keeps running into the same question: an element is misplaced on screen, and
// the fix depends entirely on which draw path put it there — the quad emitter, J2DPicture::draw,
// fill_rect, or a text box. Guessing from a screenshot has been wrong twice (the subtitle band is
// not fill_rect; the HUD gauge parts do not all go through the emitter), so this reports the .blo
// name and the pane transform at each of the J2D entry points, and the answer arrives on the probe
// at /2dclass instead of in a rebuild.
//
//   curl 127.0.0.1:17654/2dclass

#include "../frame_interp/populations.h"
#include "../frame_interp/tag_2d.h"
#include "overrides.h"

#include "../runtime/probe_server.h"
#include "../runtime/render/scene.h"
#include "../runtime/sb_assert.h"
#include "j2d_picture_adapter.h"

#include <sunbright/native_render/picture_sink.h>

#include <intrinsics.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

extern "C" void func_802cc758(CPUState&); // J2DPicture::drawSelf(int, int)
// J2DPicture::drawSelf(int, int, f32 (*)[3][4]) — the overload that RECEIVES the transform as an
// argument. The (int, int) overload above is registered but never fires, so this is the path that
// actually reaches the hardware, and its r6 is the matrix a 2D capture needs.
extern "C" void func_802cc7c0(CPUState&);
extern "C" void func_802ccef4(CPUState&); // J2DPicture::draw
extern "C" void func_802d0b28(CPUState&); // J2DTextBox::draw
extern "C" void func_802d0d70(CPUState&); // J2DTextBox::drawSelf
extern "C" void func_802d01c8(CPUState&); // J2DScreen::drawSelf

namespace {

bool diag_on() {
    static int v = -1;
    if (v < 0) {
        const char* e = std::getenv("SBR_DIAG_2D");
        v = (e != nullptr && e[0] != '\0' && e[0] != '0') ? 1 : 0;
    }
    return v == 1;
}

struct Seen {
    unsigned long hits = 0;
    float m00 = 0, m03 = 0;
    // mGlobalBounds (J2DPane+0x24) — the SCREEN-SPACE rect, with every parent transform already
    // applied, so it needs no matrix. JUTRect is {int x1 @0, y1 @4, x2 @8, y2 @0xC}
    // (JSystem/JUtility/JUTRect.hpp), and J2DPane's layout is from JSystem/J2D/J2DPane.hpp — both
    // read rather than inferred, because a transposed rect would place a HUD quad plausibly but
    // wrongly. Reported BEFORE any geometry is synthesised from it: if these are not sane screen
    // coordinates, the offsets are wrong and nothing built on them can be trusted.
    int gx1 = 0, gy1 = 0, gx2 = 0, gy2 = 0;
    int visible = -1;
    int alpha = -1, colorAlpha = -1;
    // The transform as PASSED IN to the matrix-taking drawSelf overload.
    float argM00 = 0, argM03 = 0, argM11 = 0, argM13 = 0;
    int hasArgMtx = 0;
};
std::map<std::string, Seen> g_seen;

bool guest_obj(u32 p) {
    return p >= 0x80000000u && p < 0x81800000u;
}

f32 guest_f32(u32 ea) {
    const u32 bits = sb_r32(ea);
    f32 f;
    __builtin_memcpy(&f, &bits, sizeof f);
    return f;
}

// J2DPane: .blo name fourcc at +0x10, transform at +0x84 (X translation at +0x0C).
void note(const char* cls, u32 self) {
    if (!guest_obj(self))
        return;
    const u32 t = sb_r32(self + 0x10);
    char nm[5];
    for (int i = 0; i < 4; i++) {
        const u8 c = (t >> (24 - i * 8)) & 0xff;
        nm[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
    }
    nm[4] = '\0';
    char key[64];
    std::snprintf(key, sizeof key, "%-18s %s", cls, nm);
    auto& e = g_seen[key];
    ++e.hits;
    e.m00 = guest_f32(self + 0x84);
    e.m03 = guest_f32(self + 0x84 + 0x0C);
    e.gx1 = (int)sb_r32(self + 0x24);
    e.gy1 = (int)sb_r32(self + 0x28);
    e.gx2 = (int)sb_r32(self + 0x2C);
    e.gy2 = (int)sb_r32(self + 0x30);
    e.visible = (int)sb_r8(self + 0x0C);
    e.alpha = (int)sb_r8(self + 0xCC);
    e.colorAlpha = (int)sb_r8(self + 0xCD);
}

void note_rect(const char* cls, u32 self, u32 rect) {
    note(cls, self);
    if (!guest_obj(self) || !guest_obj(rect))
        return;
    const u32 t = sb_r32(self + 0x10);
    char nm[5];
    for (int i = 0; i < 4; ++i) {
        const u8 c = (t >> (24 - i * 8)) & 0xff;
        nm[i] = (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '.';
    }
    nm[4] = '\0';
    char key[64];
    std::snprintf(key, sizeof key, "%-18s %s", cls, nm);
    auto& e = g_seen[key];
    e.gx1 = static_cast<int>(sb_r32(rect + 0x00));
    e.gy1 = static_cast<int>(sb_r32(rect + 0x04));
    e.gx2 = static_cast<int>(sb_r32(rect + 0x08));
    e.gy2 = static_cast<int>(sb_r32(rect + 0x0c));
}

const bool g_probe = [] {
    sb_probe_register("/2dclass", "which J2D class drew each pane (needs SBR_DIAG_2D=1)",
                      [](const ProbeArgs&) {
                          if (!diag_on())
                              return std::string("set SBR_DIAG_2D=1 to record\n");
                          std::string out;
                          char buf[128];
                          for (const auto& [k, e] : g_seen) {
                              std::snprintf(buf, sizeof buf,
                                            "%-26s m00=%7.3f m03=%8.2f  gbounds=[%4d,%4d %4d,%4d] "
                                            "%dx%d vis=%d a=%d/%d hits=%lu\n",
                                            k.c_str(), (double)e.m00, (double)e.m03, e.gx1, e.gy1,
                                            e.gx2, e.gy2, e.gx2 - e.gx1, e.gy2 - e.gy1, e.visible,
                                            e.alpha, e.colorAlpha, e.hits);
                              if (e.hasArgMtx) {
                                  char b2[160];
                                  std::snprintf(b2, sizeof b2,
                                                "%-26s   argMtx: sx=%7.3f tx=%8.2f sy=%7.3f "
                                                "ty=%8.2f\n",
                                                k.c_str(), (double)e.argM00, (double)e.argM03,
                                                (double)e.argM11, (double)e.argM13);
                                  out += b2;
                              }
                              out += buf;
                          }
                          if (out.empty())
                              out = "nothing recorded yet\n";
                          return out;
                      });
    return true;
}();

void ov_pic_drawself(CPUState& cpu) {
    if (diag_on())
        note("J2DPicture::drawSelf", cpu.gpr[3]);
    func_802cc758(cpu);
}
// SBR_J2D_CAPTURE=1 — emit each visible 2D pane to the GX compatibility renderer as an orthographic
// quad.
//
// This is the 2D half of that reference frontend. The compatibility renderer taps J3DShape::draw
// only, so it draws no HUD at all (a projection census reports 0 orthographic drawables of ~885,
// every frame). Everything needed is available HERE and nowhere earlier: mGlobalBounds
// (J2DPane+0x24) is the screen-space rect with all parent transforms applied, and mColorAlpha
// (+0xCD) is live — both read 0 at J2DPicture::draw entry, which is why the capture point is this
// overload and not that one.
//
// The quad is emitted in the SAME 600x480 2D space the game composes in (J2DScreen's own root pane
// reports exactly that), and handed an orthographic projection, so the existing batching, TEV,
// blend and scissor paths draw it unchanged rather than needing a second pipeline.
bool j2d_capture_on() {
    static int v = -1;
    if (v < 0) {
        const char* e = std::getenv("SBR_J2D_CAPTURE");
        v = (e != nullptr && e[0] != '\0' && e[0] != '0') ? 1 : 0;
    }
    return v == 1;
}

// Pane fields read at ENTRY (valid there — the census confirmed mGlobalBounds and mColorAlpha are
// populated at this overload, unlike at J2DPicture::draw). The MATERIAL is deliberately NOT read
// here: J2D binds its texture and sets its TEV inside the body, so the FIFO state at entry belongs
// to the previous material. That mismatch is what made the captured quads invisible.
struct PaneQuad {
    bool valid = false;
    int x1, y1, x2, y2;
    u8 alpha;
    u32 self;
};

PaneQuad read_pane(CPUState& cpu) {
    PaneQuad q;
    const u32 self = cpu.gpr[3];
    if (sb_r8(self + 0x0C) == 0)
        return q; // mVisible
    q.x1 = (int)sb_r32(self + 0x24);
    q.y1 = (int)sb_r32(self + 0x28);
    q.x2 = (int)sb_r32(self + 0x2C);
    q.y2 = (int)sb_r32(self + 0x30);
    if (q.x2 <= q.x1 || q.y2 <= q.y1)
        return q;                 // degenerate: nothing to rasterise
    q.alpha = sb_r8(self + 0xCD); // mColorAlpha
    q.self = self;
    q.valid = true;
    return q;
}

bool read_guest_bytes(void*, std::uint32_t address, void* destination, std::size_t size) {
    if (size == 0)
        return true;
    if (address > UINT32_MAX - static_cast<std::uint32_t>(size - 1))
        return false;
    u8* first = sb_ram_fast(address);
    u8* last = sb_ram_fast(address + static_cast<std::uint32_t>(size - 1));
    if (first == nullptr || last != first + size - 1)
        return false;
    std::memcpy(destination, first, size);
    return true;
}

void submit_semantic_picture(CPUState& cpu) {
    if (!sb::native_render::has_picture_sink())
        return;
    sb::recomp::CapturedPicture capture{};
    const sb::recomp::GuestByteReader reader{.read = read_guest_bytes};
    SB_ASSERT(sb::recomp::capture_j2d_picture(reader, cpu.gpr[3], cpu.gpr[6], capture),
              "semantic J2DPicture capture failed: self=%08x parent_matrix=%08x", cpu.gpr[3],
              cpu.gpr[6]);
    SB_ASSERT(sb::native_render::submit_picture(capture.command, capture.image_views()),
              "semantic J2DPicture sink rejected a validated command: self=%08x", cpu.gpr[3]);
}

void emit_pane_quad(const PaneQuad& q) {
    const int x1 = q.x1, y1 = q.y1, x2 = q.x2, y2 = q.y2;
    const u8 ca = q.alpha;
    const u32 self = q.self;

    // 600x480 -> clip space. The game composes 2D in that space (root pane = [0,0 600,480]), so the
    // mapping is exact rather than fitted, and Y flips because screen Y grows downward.
    const auto cx = [](int v) { return (float)v / 600.0f * 2.0f - 1.0f; };
    const auto cy = [](int v) { return 1.0f - (float)v / 480.0f * 2.0f; };
    const float lx = cx(x1), rx = cx(x2), ty = cy(y1), by = cy(y2);
    const uint32_t rgba = 0xFFFFFF00u | (uint32_t)ca;

    SbrGeomVert v[6]{};
    const float px[6] = {lx, rx, rx, lx, rx, lx};
    const float py[6] = {ty, ty, by, ty, by, by};
    const float su[6] = {0, 1, 1, 0, 1, 0};
    const float sv[6] = {0, 0, 1, 0, 1, 1};
    for (int i = 0; i < 6; ++i) {
        v[i].x = px[i];
        v[i].y = py[i];
        v[i].z = 0.0f;
        v[i].nz = 1.0f;
        v[i].rgba = rgba;
        for (int t = 0; t < 4; ++t) {
            v[i].uv[t][0] = su[i];
            v[i].uv[t][1] = sv[i];
        }
    }

    SbrDrawable dr{};
    dr.streamPos = sbr_gxfifo_stream_pos();
    dr.key = ((uint64_t)self << 8) | 0xFFu; // stable per pane, distinct from J3D keys
    dr.geom = sbr_scene_intern_geometry(dr.key, v, 6);
    if (dr.geom == 0)
        return;
    dr.depth = sbr_gx_fifo_zmode();
    for (unsigned m = 0; m < 8; ++m)
        dr.tex[m] = sbr_gx_fifo_texture(m);
    dr.tev = sbr_gx_fifo_tev();
    dr.xf = sbr_gx_fifo_xf();
    // SBR_J2D_SOLID=1 (DIAGNOSTIC): draw the pane as an opaque vertex-coloured quad with the depth
    // test OFF and blending off. That is a KNOWN-VISIBLE configuration which depends on none of the
    // three suspects — material snapshot, z-mode, or projection convention — so it separates "the
    // quad does not rasterise at all" from "it rasterises but is shaded or rejected into
    // invisibility". Without this the three would have to be tested one guess at a time.
    static const bool solid = [] {
        const char* e = std::getenv("SBR_J2D_SOLID");
        return e != nullptr && e[0] != '\0' && e[0] != '0';
    }();
    if (solid) {
        dr.depth.test = 0;
        dr.depth.write = 0;
        dr.depth.blend = 0;
        dr.depth.cull = 0;
        dr.depth.colorUpdate = 1;
        dr.depth.alphaUpdate = 1;
        dr.tev = SbrTevState{}; // defaults: 1 stage, texture disabled
        dr.tev.numStages = 1;
        dr.tev.stage[0].texEnable = 0;
        dr.tev.stage[0].rasChannel = 0;
        // out = RASC (the vertex colour), alpha = RASA: c = mix(ZERO, ZERO, ZERO) + RASC
        dr.tev.stage[0].cA = 0;
        dr.tev.stage[0].cB = 0;
        dr.tev.stage[0].cC = 0;
        dr.tev.stage[0].cD = 10; // RASC
        dr.tev.stage[0].aA = 0;
        dr.tev.stage[0].aB = 0;
        dr.tev.stage[0].aC = 0;
        dr.tev.stage[0].aD = 5; // RASA
        dr.tev.alphaOp0 = 7;    // ALWAYS
        dr.tev.alphaOp1 = 7;
        for (int i = 0; i < 6; ++i)
            v[i].rgba = 0xFF00FFFFu; // opaque magenta: unmistakable
        dr.geom = sbr_scene_intern_geometry(dr.key ^ 0x5011Du, v, 6);
        if (dr.geom == 0)
            return;
    }
    // Identity model-view: the quad is already in clip space. IDENTITY PROJECTION for the same
    // reason — projecting a 2D element with the 3D matrix makes it cover the screen, which is the
    // documented failure mode in scene.h.
    dr.mtx[0] = dr.mtx[5] = dr.mtx[10] = 1.0f;
    for (int i = 0; i < 16; ++i)
        dr.proj[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    sbr_scene_add(dr);
}

void ov_pic_drawself_mtx(CPUState& cpu) {
    // Capture game-semantic state at entry. The recompiled body is always retained and called;
    // this path never reads its post-body FIFO/TEV state and never crosses into decomp objects.
    submit_semantic_picture(cpu);
    const PaneQuad pane = j2d_capture_on() ? read_pane(cpu) : PaneQuad{};
    if (diag_on()) {
        note("J2DPicture::drawSelfM", cpu.gpr[3]);
        // The matrix comes in as an ARGUMENT (r6), not from the pane — which is why reading
        // mGlobalMtx at the draw entry saw zeros for these panes.
        const u32 tag = sb_r32(cpu.gpr[3] + 0x10);
        char nm[5];
        for (int i = 0; i < 4; i++) {
            const u8 c = (tag >> (24 - i * 8)) & 0xff;
            nm[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
        }
        nm[4] = '\0';
        char key[64];
        std::snprintf(key, sizeof key, "%-18s %s", "J2DPicture::drawSelfM", nm);
        auto& e = g_seen[key];
        ++e.hits;
        if (cpu.gpr[6] != 0) {
            e.argM00 = guest_f32(cpu.gpr[6] + 0x00);
            e.argM03 = guest_f32(cpu.gpr[6] + 0x0C);
            e.argM11 = guest_f32(cpu.gpr[6] + 0x14);
            e.argM13 = guest_f32(cpu.gpr[6] + 0x1C);
            e.hasArgMtx = 1;
        }
    }
    func_802cc7c0(cpu);
    // AFTER the body: the FIFO now holds the material this pane bound, so the snapshot describes
    // the right texture and TEV. Emitting before the call captured the previous material.
    if (pane.valid)
        emit_pane_quad(pane);
}
void ov_pic_draw(CPUState& cpu) {
    sb::frame_interp::two_d::PaneScope identity(cpu.gpr[3],
                                                sb::frame_interp::two_d::DrawPath::Picture);
    if (diag_on())
        note("J2DPicture::draw", cpu.gpr[3]);
    func_802ccef4(cpu);
}
void ov_text_draw(CPUState& cpu) {
    if (diag_on())
        note("J2DTextBox::draw", cpu.gpr[3]);
    func_802d0b28(cpu);
}
void ov_text_drawself(CPUState& cpu) {
    if (diag_on())
        note("J2DTextBox::drawSelf", cpu.gpr[3]);
    func_802d0d70(cpu);
}
void ov_screen_drawself(CPUState& cpu) {
    if (diag_on())
        note("J2DScreen::drawSelf", cpu.gpr[3]);
    func_802d01c8(cpu);
}
} // namespace

// The window override lives in hud.cpp because it also performs the widescreen correction. Keep
// the census here so the diagnostic and shipping path observe the exact same draw and rect.
void sbr_diag_2d_note_window(u32 self, u32 rect) {
    if (diag_on())
        note_rect("J2DWindow::rect", self, rect);
}

SB_OVERRIDE(0x802cc758u, ov_pic_drawself, "J2DPicture::drawSelf", "diagnostic: 2D class census")
SB_OVERRIDE(0x802cc7c0u, ov_pic_drawself_mtx, "J2DPicture::drawSelf(Mtx)",
            "diagnostic: 2D class census — the overload that receives the transform")
SB_OVERRIDE(0x802ccef4u, ov_pic_draw, "J2DPicture::draw", "diagnostic: 2D class census")
SB_OVERRIDE(0x802d0b28u, ov_text_draw, "J2DTextBox::draw", "diagnostic: 2D class census")
SB_OVERRIDE(0x802d0d70u, ov_text_drawself, "J2DTextBox::drawSelf", "diagnostic: 2D class census")
SB_OVERRIDE(0x802d01c8u, ov_screen_drawself, "J2DScreen::drawSelf", "diagnostic: 2D class census")
