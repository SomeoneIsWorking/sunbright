// attrib_helpers.cpp — attribute an SDK draw helper's geometry to whoever CALLED it.
//
// THE PROBLEM THIS FIXES IS IN THE INSTRUMENT, not in the game. The graphics registry names an
// emitter by the return address at the GX waist, which for ordinary game code is exactly right: the
// caller of GXBegin is the system drawing. It is useless for the SDK's own draw helpers. GXDrawCube
// builds its geometry inside GXDrawCubeFace, so every cube in the game — from any subsystem, for
// any reason — lands in ONE registry row called `GXDrawCube+0x100`, which says nothing about who
// wanted a cube drawn and cannot be worked on.
//
// So the helper declares its caller. While GXDrawCube is running, the attribution of any primitive
// it emits is redirected to the address that called GXDrawCube — one frame further up, which is the
// game system. The registry then holds a row per CALLER rather than one row for the helper.
//
// This is the same "one frame further up" that tag_gap's second-level histogram does for
// J3DShapeDraw::draw, made permanent and applied where it is structurally necessary rather than
// where a measurement happened to want it.
//
// GXDrawSphere gets the same treatment for the same reason. GXDrawCubeFace is deliberately NOT
// hooked: it is GXDrawCube's own helper, so hooking it would redirect attribution to GXDrawCube —
// back to the address the redirect exists to get past.

#include "../overrides/overrides.h"
#include "frame_interp.h"
#include "graphics_db.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <unordered_map>

extern "C" void func_803627fc(CPUState&);   // GXDrawCube(void)
extern "C" void func_80362268(CPUState&);   // GXDrawSphere(u8)

void sbr_gfxdb_attribute_to(u32 guestAddr);
void sbr_gxfifo_draw_tag(uint64_t tag);
uint64_t sbr_gxfifo_pending_tag();
bool sbr_lerp_enabled();

namespace {

// Scoped so a helper that calls another helper cannot leave the redirect pointing at the inner
// call's caller — the same discipline every labelling seam here follows.
struct Redirect {
    bool tagged;
    explicit Redirect(u32 caller, uint64_t tag) : tagged(tag != 0) {
        sbr_gfxdb_attribute_to(caller);
        if (tagged) sbr_gxfifo_draw_tag(tag);
    }
    ~Redirect() {
        if (tagged) sbr_gxfifo_draw_tag(0);
        sbr_gfxdb_attribute_to(0);
    }
};

uint64_t singleton_tag(u32 site);

void ov_draw_cube(CPUState& cpu) {
    const u32 site = (u32)cpu.lr;
    // Never over an identity a seam upstream already established.
    Redirect r(site, sbr_gxfifo_pending_tag() == 0 ? singleton_tag(site) : 0);
    func_803627fc(cpu);
}

void ov_draw_sphere(CPUState& cpu) {
    Redirect r((u32)cpu.lr, 0);
    func_80362268(cpu);
}

// ── AND THEN THE CUBES CAN BE INTERPOLATED, because naming them said what they were ─────────────
//
// Attribution turned one useless `GXDrawCube+0x100` row into two rows in TMario::perform, and the
// decomp says exactly what they are (MarioMain.cpp:245,258,287): boxDrawPrepare(graphics->mViewMtx)
// loads a model x view matrix around Mario's bounding box and GXDrawCube draws a unit cube through
// it — the OCCLUSION PROBE that writes dst-alpha where Mario is, and the SILHOUETTE box that fills
// his see-through outline when he is behind geometry. Both move with Mario, and both were taking
// the camera delta alone, so Mario's silhouette sat half a tick behind Mario on every in-between
// frame.
//
// The geometry is matrix-carried (a unit cube; all the motion is in the matrix), so the ordinary
// paired path handles it — it only ever needed an identity.
//
// THE IDENTITY IS THE CALL SITE, and that is sound HERE for a reason that must not be generalised:
// there is exactly one TMario, and each of these sites sits inside its own `if (param_1 & flag)`
// branch of one perform pass, so a site draws at most one cube per tick. The key is therefore a
// per-object key that happens to be spelled as an address, not an ordinal over a varying
// population — which is the thing this project has had to withdraw twice.
//
// And the assumption CHECKS ITSELF rather than being trusted: if any site is ever seen drawing
// twice within one simulation tick, its premise is false, the tag is withheld from that site for
// the rest of the run, and the run says so. A second cube from one site would otherwise pair
// against the first one's previous pose, which is a wrong frame that looks like a right one.
struct Singleton {
    uint64_t lastTick = 0;
    unsigned drawsThisTick = 0;
    bool trusted = true;
};
std::unordered_map<u32, Singleton> g_singleton;

// The call sites whose singleton-ness has been argued from the source, one entry per site. Nothing
// is tagged by default: a cube from an unexamined site keeps the camera delta, which is the
// conservative answer.
bool is_known_singleton(u32 site) {
    // OBSERVED addresses only — an address nobody has seen the game reach is a magic constant, and
    // a wrong one would tag some other system's cube. The third entry below arrived exactly the way
    // this comment used to predict it would: the silhouette branch drew, the registry filed a row
    // for it (`camera-only`, i.e. following the camera but not Mario), and the row is what added it.
    //
    // Each is confirmed against the DOL, not inferred from source order. At 0x8024dab0 the US
    // binary has `bl GXDrawCube`, so 0x8024dab4 is its return address — the site key. The four
    // instructions before it are `GXSetBlendMode(GX_BM_BLEND, GX_BL_DSTALPHA, GX_BL_INVDSTALPHA,
    // GX_LO_NOOP)`, colour+alpha update on, `GXSetDstAlpha(GX_ENABLE, 0)`; the two after it are the
    // `lwz/rlwinm/stw` of `j3dSys.offFlag(0x2)`. That is MarioMain.cpp:287's silhouette block and
    // nothing else — the two occlusion probes set dst-alpha to 0x10 and 0 with colour update OFF
    // and are not preceded by a blend-mode change.
    //
    // It is a singleton on the same terms as the other two: one TMario, and the site sits inside
    // `if ((param_1 & 0x80000000) && (unk114 & UNK114_FLAG_VISIBLE))` in one perform pass. The
    // draws-per-tick check below still polices that rather than trusting it.
    return site == 0x8024d8fcu    // TMario::perform+0x654 — occlusion probe, dst-alpha 0x10
        || site == 0x8024d96cu    // TMario::perform+0x6c4 — occlusion probe, dst-alpha 0
        || site == 0x8024dab4u;   // TMario::perform+0x80c — silhouette box, blended DSTALPHA
}

uint64_t singleton_tag(u32 site) {
    if (!sbr_lerp_enabled() || !is_known_singleton(site)) return 0;
    Singleton& s = g_singleton[site];
    const uint64_t tick = sb::frame_interp::sim_tick_seq();
    if (tick != s.lastTick) {
        s.lastTick = tick;
        s.drawsThisTick = 0;
    }
    if (++s.drawsThisTick > 1 && s.trusted) {
        s.trusted = false;
        lucent::warn("taggap", "0x{:08x} drew {} cubes in ONE tick, so it is NOT the single-object "
                               "site its identity assumed. The tag is withdrawn from it for the "
                               "rest of this run — those draws take the camera delta instead of "
                               "pairing against another object's pose.",
                     site, s.drawsThisTick);
    }
    return s.trusted ? ((uint64_t)site << 32 | 1u) : 0;
}

} // namespace

SB_OVERRIDE(0x803627fcu, ov_draw_cube, "GXDrawCube",
            "graphics registry: attribute the cube's primitives to the game code that asked for a "
            "cube, not to the SDK helper that builds every cube in the game")
SB_OVERRIDE(0x80362268u, ov_draw_sphere, "GXDrawSphere",
            "graphics registry: attribute the sphere's primitives to its caller, same reason as "
            "GXDrawCube")
