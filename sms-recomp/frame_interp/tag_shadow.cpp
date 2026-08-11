// tag_shadow.cpp — give the shadow's draws a cross-tick identity so they interpolate.
//
// WHY THE SHADOW SPECIFICALLY. A draw interpolates only if it carries an identity, which
// j3d_capture.cpp emits at J3DShape::draw. Attribution of everything that does NOT
// (frame_interp/tag_gap.cpp, SBR_TAGGAP=1) came back with the entire untagged-DRAW population in
// two places, and both are the shadow:
//
//     J3DShapeDraw::draw                        8,840,210 draws   76.4%
//       ... called 100% from SMS_DrawShape, whose only callers are MarioUtil/ShadowUtil.cpp
//     TModelWaterManager::drawShineShadowVolume 2,733,200 draws   23.6%   (two call sites)
//     SMS_SettingDrawShape                              0 draws    0.0%   (state, no primitives)
//     J3DDisplayListObj::callDL                         0 draws    0.0%   (material DLs)
//
// The last two matter as a warning: counted by CALLS instead of DRAWS, callDL was 61.9% of the gap
// and looked like the thing to fix. It emits no primitives at all. Two populations, one number.
//
// Untagged draws fall through to patch_camera_only and receive the camera delta ALONE. For static
// scenery that is correct. A shadow is the opposite case: it tracks a moving actor, so it followed
// the camera at 60 Hz while its own position stepped at 30 — which is the juddering shadow the user
// reported while the actor above it moved smoothly.
//
// THE IDENTITY, and why it is fp. ShadowUtil draws each shadow from a linked list of
// TAlphaShadowQuad (grp.mFpHead -> mNext), and per entry does
//     PSMTXConcat(view, fp->mMtx, fpMv); GXLoadPosMtxImm(fpMv, GX_PNMTX0); drawShadowVolume(_, fp);
// so `fp` is the per-INSTANCE object and its matrix is a per-instance position matrix in the
// uniform block — exactly the shape patch_draw lerps. Tagging by the shadow MODEL instead would
// collapse every shadow in the scene into one identity and pair instance k with some other
// instance's transform: the same failure j3d_capture.cpp documents for J3DShape, whose signature
// was paired-draw motion of mean 31.9 world units per 1/30 s.
//
// THE PASS INDEX IS PART OF THE KEY. The same fp is drawn again in later passes (the dst-alpha
// mask, then the darken), so fp alone would put several draws of one tick under one identity and
// the pairing would have to guess between them. A per-fp draw counter, reset when the tag is
// cleared, disambiguates them in emission order — which IS stable here, because the passes iterate
// the same list in the same order every tick.
//
// COVERAGE REACHED, AND WHAT IT COST — measured, because "0% untagged" is only good news if the
// identities are right. Untagged INDEXED draws went 9.5% -> 0.0% (aurora's own counter) and
// untagged display-list draws 6.6% -> 0.0% (this file's). Against a control with the tagging off,
// the share of paired draws showing 10-100 units/tick of object motion — the instrument's own
// mispairing signature — and the count in the 100-1k bucket:
//
//     no shadow tags (control)      [10,100) 24.4%    [100,1k)      4    mean 8.4
//     fp only (genuine identity)    [10,100) 27.3%    [100,1k)  1,810    mean 12.3
//     all three schemes             [10,100) 26.4%    [100,1k) 25,113    mean 51.8
//
// The two ORDINAL schemes carry ~93% of the added mispairing, which is what their design predicts:
// an ordinal is a positional stand-in for identity and misaligns whenever a list changes length.
// `fp`, a real object address, adds far less — and some of even that 1,810 is likely genuine, since
// a shadow projected onto terrain jumps hundreds of units when the surface under it changes.
// Separating genuine motion from mispairing needs the histogram split by tag KIND (these tags have
// a small low word; J3DShape's is a heap pointer), which is one run's work and is the next step.
//
// Shipped with all three on, because an untagged shadow is wrong on EVERY frame its caster moves —
// it receives the camera delta alone, so it follows the camera and not the thing casting it —
// whereas a mispaired one is wrong on the frames where the list shifts. SBR_TAGSHADOW=fp keeps only
// the identity that is beyond doubt; SBR_TAGSHADOW=0 disables the lot.

#include "../overrides/overrides.h"
#include "mark_exact.h"
#include "graphics_db.h"
#include "populations.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace aurora::gfx::interp { long tick_index(); }

extern "C" void func_802305dc(CPUState&);   // TMBindShadowManager::drawShadowVolume(bool, TAlphaShadowQuad*)
extern "C" void func_8027c67c(CPUState&);   // TModelWaterManager::drawShineShadowVolume(MtxPtr)
extern "C" void func_80225c30(CPUState&);   // SMS_DrawShape(J3DModelData*, u16)
extern "C" void func_80218020(CPUState&);   // TLiveActor::requestShadow()
extern "C" void func_8022ecec(CPUState&);   // TMBindShadowManager::request(const TCircleShadowRequest&, u32)
extern "C" void func_8022ebbc(CPUState&);   // TMBindShadowManager::forceRequest(...)

void sbr_gxfifo_draw_tag(uint64_t tag);
uint64_t sbr_gxfifo_pending_tag();
bool sbr_lerp_enabled();

namespace {

// SBR_TAGSHADOW: 0 = off, fp = ONLY the genuine per-instance key, anything else = all three.
//
// The split exists because the three schemes are not equally trustworthy and the difference is
// measurable. `fp` is a real per-instance object address. The other two substitute an ORDINAL for
// the instance, which is only as good as the assumption that each pass walks its list in the same
// order every tick — so they are the ones to suspect when pairing goes wrong, and this makes that a
// one-run question instead of an argument.
enum class Scheme { Off, FpOnly, All };
Scheme scheme() {
    static const Scheme v = [] {
        const char* e = std::getenv("SBR_TAGSHADOW");
        // DEFAULT IS FpOnly, and the ordinal schemes are opt-in. Measured, same scenario, the
        // interpolator's own mispairing signature (draws pairing with a pose 100-1000 world units
        // away, which nothing reaches in 1/30 s):
        //     no shadow tags                          4
        //     fp only                             1,810
        //     fp + ordinal schemes               25,113
        //     fp + ordinals + request fingerprint 28,127   (the fingerprint does nothing — every
        //                                                   marukage shares type/radius/flags)
        //     fp + ordinals + set-change gate    19,985
        // The ordinal schemes carry ~93% of it, which is what a positional stand-in for identity
        // predicts. The user sees that share directly: other characters' marukage rendering at the
        // wrong place for a single frame every few seconds. Snapping is what those draws did before
        // any of this and is strictly better than teleporting, so they snap until a real owner
        // identity exists.
        // DEFAULT ON AGAIN, because the key is now sound. The retreat below is kept as the record
        // of why it was ever off.
        //
        // Shadows are keyed by the ACTOR that requested them, joined through the position the
        // manager copies verbatim from the request into the slot. Measured with that key and no
        // slot fallback: mispairing is 4 — IDENTICAL to a run with shadow tagging disabled
        // entirely — while 94.3% of the shadow-volume population interpolates. A sound identity
        // costs nothing and the unsound one cost everything.
        //
        //
        // Every reading that made slot-keying look acceptable was taken while this file read the
        // WRONG REGISTER: r4 is `useNear`, a bool, so all near shadows collapsed into the single
        // identity 1 and every far shadow was skipped for "fp == 0". Tagging was therefore nearly
        // inert, and the 98 mispairs it scored were the score of doing almost nothing.
        //
        // With r5 — the actual quad — the tagging is real for the first time, and so is the defect
        // the slot key carries: mispairs go 98 -> 1,128 against a no-tagging control of 4. That is
        // the marukage teleport with the volume turned up, and it is not something to ship while
        // the user is watching shadows.
        //
        // SBR_TAGSHADOW=fp turns it back on. It stays off until the OWNER join resolves, which is
        // the one key here that does not depend on a slot; its plumbing and its four denominators
        // are in place and it currently resolves nothing, which is the next thing to fix.
        // DEFAULT IS NOW ALL THREE SCHEMES, and that reverses a decision made on a CONFOUNDED
        // measurement. The verdict "the ordinal schemes carry ~93% of the added mispairing" was
        // taken while this file read r4 — a bool — so the shadow VOLUME was mispairing
        // catastrophically and the ordinals were blamed for it. Re-measured with the owner key as
        // the base:
        //     owner only ......... mispairs 16   volume 94.6%  shine   0%   model   0%
        //     owner + ordinals ... mispairs 16   volume 94.6%  shine 95.4%  model 99.8%
        // Identical mispairing, ~730,000 more draws interpolating. An ordinal is still a positional
        // stand-in and still misaligns when a list changes length; it simply does not, measurably,
        // in this game — which is a fact about the scene that only became visible once the base key
        // was sound.
        if (e == nullptr) return Scheme::All;
        if (e[0] == '0') return Scheme::Off;
        if (std::strcmp(e, "all") == 0) return Scheme::All;
        if (std::strcmp(e, "fp") == 0) return Scheme::FpOnly;
        return Scheme::FpOnly;
    }();
    return v;
}
bool enabled() { return scheme() != Scheme::Off; }
bool ordinal_schemes_on() { return scheme() == Scheme::All; }

// How many times each fp has been drawn in the tick so far, so repeated passes over the same list
// get distinct identities. Cleared once per tick by the seam below.
std::unordered_map<u32, u32> g_seenThisTick;
unsigned long g_tagged = 0, g_ticks = 0;

// ── THE SHINE SHADOW VOLUME, the other 23.6% ────────────────────────────────────────────────────
//
// TModelWaterManager::drawShineShadowVolume replays one baked sphere display list repeatedly in a
// loop, stacking slices to build the volume — so a single call produces many draws that differ only
// in the matrix loaded before each. There is no per-slice OBJECT to key on the way `fp` keys a
// shadow quad, and the function is entered once, so it cannot tag from its own frame.
//
// The identity is therefore (this call site, slice ordinal): slice k of the volume pairs with slice
// k of the previous tick, which is what the geometry actually is. The ordinal resets per CALL rather
// than per tick, because the volume is rebuilt from scratch each time it is drawn — a tick-scoped
// ordinal would misalign every slice the moment the volume is drawn twice in one tick.
//
// The scope is marked here and the tag is emitted by the GXCallDisplayList seam in tag_gap.cpp,
// which is the one place that sees each individual replay. Two overrides cannot share an address,
// so the two files split the work rather than both hooking it.
bool g_shineScope = false;
u32 g_shineOrdinal = 0;
unsigned long g_shineTagged = 0;
constexpr u64 kShineId = 0x5417EULL;   // a fixed id; distinct from any guest pointer

// ── SMS_DrawShape, the last of the population ───────────────────────────────────────────────────
//
// After the two volume paths were tagged, 100% of what remained came through one site:
// J3DShapeDraw::draw, reached only from SMS_DrawShape, whose only callers are the shadow passes in
// MarioUtil/ShadowUtil.cpp that draw a shadow MODEL directly instead of going through
// drawShadowVolume — pass 4's type-3 extras and the ship/boat shapes (mModels[0..3]).
//
// Those loops have the same shape as the volume ones: PSMTXConcat(view, fp->mMtx, fpMv);
// GXLoadPosMtxImm(fpMv, PNMTX0); SMS_DrawShape(mModels[k], 0). But SMS_DrawShape is not handed `fp`,
// so the per-instance object is not available in its frame and cannot be recovered from its
// arguments — mModels[k] is the shared shadow RESOURCE, and keying on it alone would collapse every
// instance into one identity, which is the mispairing this file exists to avoid.
//
// So the key is (model, nth-draw-of-this-model-this-tick). The ordinal stands in for the instance,
// and it is stable for the same reason the shine slices' is: each pass walks the same list in the
// same order every tick. When the list length changes — a shadow appears or disappears — the
// ordinals shift and the affected draws pair with the wrong instance for one tick; the vertex-count
// check catches the ones whose geometry differs and snaps them, and the rest are a single frame of
// a shadow lerping from another shadow's pose. That is a real limitation and it is why this is the
// LAST of the three rather than the model for the other two: where a genuine per-instance object
// exists, as `fp` does, it is used instead.
std::unordered_map<u32, u32> g_modelSeenThisTick;
unsigned long g_modelTagged = 0;

// ── THE OWNER IDENTITY ──────────────────────────────────────────────────────────────────────────
//
// The sound key the slot index was standing in for. TLiveActor::requestShadow builds a LOCAL
// TCircleShadowRequest from the actor's own position and hands it to the manager, which COPIES it
// into mRequests[i] (Strategic/liveactor.cpp:313, MarioUtil/ShadowUtil.cpp:182). So the submitted
// position is carried verbatim into the slot, and it joins the two sides exactly: record
// position -> actor at request time, then at draw time read fp->mReq->unk0 and look the actor up.
//
// Matched on EXACT float equality, not proximity. A threshold would be a different kind of guess
// from the one being replaced, and this needs none: the bytes are copied, not recomputed.
//
// An ACTOR is a genuine per-instance identity — long-lived, not recycled per tick — which is
// exactly what fp turned out not to be.
//
// WHAT THIS DOES NOT COVER, stated because a partial map must not read as a total one: the calc
// pass MUTATES unk0 for type-1 (body) shadows before they are drawn, sliding the centre along the
// light direction (ShadowUtil.cpp:288). Those no longer match what was submitted and fall back to
// the previous behaviour. Circle shadows — the marukage the user reported teleporting — are not
// mutated and are covered.
struct PosKey {
    u32 x, y, z;
    bool operator==(const PosKey& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct PosKeyHash {
    std::size_t operator()(const PosKey& k) const {
        std::size_t h = 1469598103934665603ull;
        for (u32 v : {k.x, k.y, k.z}) { h ^= v; h *= 1099511628211ull; }
        return h;
    }
};
// DOUBLE-BUFFERED, because the shadow manager DRAWS BEFORE the actors re-request. Measured: with a
// single map cleared each tick, all 572,296 draw-time lookups missed while the map itself peaked at
// 116 entries — populated, but not yet at the moment the draw needed it. The draw therefore has to
// consult the PREVIOUS tick's requests, which is also the correct pairing: the quad being drawn was
// built from them.
std::unordered_map<PosKey, u32, PosKeyHash> g_posToActor;      // filling this tick
std::unordered_map<PosKey, u32, PosKeyHash> g_posToActorPrev;  // what the draw reads

// ── KEYED BY THE REQUEST'S ADDRESS, which covers every shadow type ──────────────────────────────
//
// The position join cannot reach type-1 BODY shadows: the calc pass rewrites their position in
// place before the draw (ShadowUtil.cpp:288), so the submitted value no longer matches. The
// request's ADDRESS does not change when its contents do, and the draw has it — fp->mReq.
//
// Where request() stores the copy was MEASURED, not read off the header, whose offset comments are
// hand-written and demonstrably approximate. SBR_SHADOW_LAYOUT=1 scans the manager for a
// just-submitted position; consecutive accepted requests landed at manager+0x7c, +0xa0, +0xc4,
// +0xe8 — stride 0x24, exactly sizeof(TCircleShadowRequest), so the array is contiguous and filled
// from slot 0 each tick.
//
// The base is learned from the first accepted request of a tick and then VERIFIED on every
// subsequent one by reading the predicted address back and comparing the position. That readback is
// what makes this self-correcting rather than a hardcoded offset: if the prediction ever stops
// matching — a different manager, a layout change, a rejected request miscounted — it rescans
// instead of silently keying on the wrong slot.
constexpr u32 REQ_STRIDE = 0x24;
u32 g_reqBase = 0;          // absolute address of mRequests[0]
u32 g_acceptedThisTick = 0;
std::unordered_map<u32, u32> g_addrToActor, g_addrToActorPrev;
unsigned long g_addrHit = 0, g_addrMiss = 0, g_rescans = 0;

// ── WHY PASS-4 SHADOW SHAPES ARE STILL UNKEYED — a route that was tried and is CLOSED ───────────
//
// Pass 4 of drawShadow draws the type-3 (ship) shapes through SMS_DrawShape, which receives only the
// shared shadow MODEL; the per-instance quad lives in the enclosing loop and never reaches the call.
// That population is the largest one still snapping.
//
// The decomp shows the loop doing `PSMTXConcat(view, fp->mMtx, fpMv)` immediately before the draw,
// and mMtx is at quad+0x04 — so the quad looked recoverable as r4-4, verified by requiring its mReq
// to resolve to a known owner. It is not: MEASURED, drawShadow was entered 20,386 times and
// PSMTXConcat (US 0x803499f0) was called ZERO times inside it. Retail does not reach that symbol
// here — inlined, or a different concat entry — so the recovery has nothing to hook and the hooks
// were removed rather than left in place doing nothing at the cost of a wrapper on one of the
// hottest functions in the game.
//
// What would work instead: hook the pass-4 loop itself, or find the concat retail actually calls.
// Neither is done here.
u32 g_requestingActor = 0;
unsigned long g_ownerTagged = 0, g_ownerMissed = 0;
// The JOIN's own denominators. "0 owners resolved" has three causes — requestShadow never fired,
// request() never fired with an actor in scope, or the position did not match at draw time — and
// without these they are one number.
unsigned long g_reqShadowCalls = 0, g_noteReqCalls = 0, g_noteReqWithActor = 0, g_lookupMiss = 0;
// WHO ASKS FOR A SHADOW WITHOUT BEING AN ACTOR. "3,145 request() calls had no actor in scope" is a
// count, and a count cannot be worked on — it names no code. These are the requests whose owner can
// never be resolved, and one of them poisons its whole group's membership key, so the group's
// alpha-restore box snaps for the tick. Keyed by the RETURN ADDRESS, which names the call site.
std::unordered_map<u32, unsigned long> g_reqNoActorSite;
// IS THE UN-OWNED REQUEST'S OWN ADDRESS A USABLE IDENTITY? If the caller passes a request embedded
// in a persistent object, the same address recurs every tick and can serve as a synthetic owner; if
// it is a stack temporary, the address is meaningless and keying on it would invent an identity that
// changes with the stack. That decides the fix, so it is measured rather than assumed.
std::unordered_set<u32> g_noActorReqThisTick, g_noActorReqPrevTick;
unsigned long g_noActorReqRecurred = 0, g_noActorReqNew = 0;
unsigned long g_ownerGuardQuad = 0, g_ownerGuardReq = 0, g_ownerCalls = 0, g_lookupHit = 0;
unsigned long g_mapMaxSize = 0;

constexpr u32 REQ_UNK0 = 0x00;   // JGeometry::TVec3<f32> — the submitted position

// The shadow SET's size, per tick. Slots are only stable while it is.
u32 g_thisTickQuadCount = 0, g_lastTickQuadCount = 0, g_prevTickQuadCount = 0;
bool g_setStableThisTick = false;
unsigned long g_snappedForSetChange = 0;

} // namespace

// Non-zero while the shine shadow volume is being drawn; each call returns the next slice's tag.
u64 sbr_shine_shadow_next_tag() {
    if (!g_shineScope || !ordinal_schemes_on() || !sbr_lerp_enabled()) return 0;
    ++g_shineTagged;
    return (kShineId << 32) | (u64)(g_shineOrdinal++);
}

// Called once per tick from the frame seam: a draw ordinal that never resets would grow without
// bound and, worse, would make this tick's tag disagree with the previous tick's for the same
// shadow — so nothing would ever pair and the change would silently do nothing.
void sbr_tag_shadow_begin_tick() {
    g_seenThisTick.clear();
    g_modelSeenThisTick.clear();
    if (g_posToActor.size() > g_mapMaxSize) g_mapMaxSize = g_posToActor.size();
    g_posToActorPrev.swap(g_posToActor);
    g_posToActor.clear();
    g_addrToActorPrev.swap(g_addrToActor);
    g_addrToActor.clear();
    g_noActorReqPrevTick.swap(g_noActorReqThisTick);
    g_noActorReqThisTick.clear();
    g_acceptedThisTick = 0;
    // Decided ONCE per tick, from the two previous ticks' counts: this tick may only pair if the
    // set was the same size last tick as the tick before, because pairing compares THIS tick's
    // slots against LAST tick's.
    g_prevTickQuadCount = g_lastTickQuadCount;
    g_lastTickQuadCount = g_thisTickQuadCount;
    g_setStableThisTick = (g_lastTickQuadCount == g_prevTickQuadCount) && g_lastTickQuadCount != 0;
    g_thisTickQuadCount = 0;
    ++g_ticks;
}

void sbr_tag_shadow_report() {
    if (!enabled() || !sbr_lerp_enabled()) return;
    lucent::info("taggap",
                 "  owner join: requestShadow fired {} time(s); manager request() fired {} time(s), "
                 "{} of them with an actor in scope; {} draw-time position lookups MISSED. All four "
                 "are needed: a zero in the first two is a hook that never ran, a zero in the third "
                 "means the request does not come through requestShadow, and misses in the fourth "
                 "mean the position is not carried verbatim after all. owner_of ran {} time(s), "
                 "refused {} on an unreadable quad and {} on an unreadable request; {} lookups "
                 "HIT; the position map peaked at {} entries in a tick.",
                 g_reqShadowCalls, g_noteReqCalls, g_noteReqWithActor, g_lookupMiss, g_ownerCalls,
                 g_ownerGuardQuad, g_ownerGuardReq, g_lookupHit, g_mapMaxSize);
    // The un-owned requests, BY CALL SITE. Printed whether or not there are any: "no site" and "the
    // histogram was never filled" are different facts, and only one of them means the join is
    // complete.
    if (g_reqNoActorSite.empty()) {
        lucent::info("taggap", "shadow owner join: every request() this run arrived with an actor in "
                               "scope — there is no un-owned call site left to find. (If the run "
                               "drew no shadows at all this line says nothing; check the counts "
                               "above.)");
    } else {
        std::vector<std::pair<u32, unsigned long>> sites(g_reqNoActorSite.begin(),
                                                         g_reqNoActorSite.end());
        std::sort(sites.begin(), sites.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        lucent::info("taggap",
                     "shadow owner join: of the un-owned requests, {} arrived at an address seen on "
                     "the PREVIOUS tick and {} at a fresh one. A large first number means the "
                     "request lives in a persistent object and its address is a usable synthetic "
                     "owner; a large second means it is a stack temporary and keying on it would "
                     "invent an identity that changes with the stack.",
                     g_noActorReqRecurred, g_noActorReqNew);
        for (const auto& [lr, n] : sites) {
            lucent::info("taggap",
                         "shadow owner join: {} request(s) came from return address {:#010x} ({}) "
                         "with NO actor in scope, so their owner cannot be established and every "
                         "shadow GROUP containing one snaps instead of interpolating.",
                         n, lr, sbr_gfxdb_symbolize(lr));
        }
    }
    lucent::info("taggap",
                 "shadow tagging: {} volume + {} shine-slice + {} model draw(s) given an identity "
                 "over {} tick(s); {} keyed by their OWNING ACTOR and {} still by slot (the owner "
                 "could not be established — a type-1 body shadow, whose position the calc pass "
                 "mutates before the draw); {} draw(s) SNAPPED because the shadow set changed size "
                 "and no owner was known for them{}",
                 g_tagged, g_shineTagged, g_modelTagged, g_ticks, g_ownerTagged, g_ownerMissed,
                 g_snappedForSetChange,
                 (g_tagged + g_shineTagged + g_modelTagged) == 0
                     ? "   <-- NONE. Either no shadow drew in this scene, or the hook never fired; "
                       "those are different answers and this line cannot tell them apart, so check "
                       "SBR_TAGGAP=1 for whether the untagged population is still there."
                     : "");
}

namespace {

void ov_sms_draw_shape(CPUState& cpu) {
    // Labelled always, tagged only under SBR_TAGSHADOW=all — see the note above.
    const bool label = sbr_lerp_enabled() && sbr_gxfifo_pending_tag() == 0;
    if (label) sbr_gxfifo_draw_pop(SB_POP_SHADOW_MODEL);
    // r3 = J3DModelData*, r4 = u16 shape index.
    const u32 model = (u32)cpu.gpr[3];
    const u32 shapeIdx = (u32)cpu.gpr[4] & 0xFFFF;
    const bool tag = ordinal_schemes_on() && sbr_lerp_enabled() && model != 0 &&
                     sbr_gxfifo_pending_tag() == 0;
    if (tag) {
        const u32 key = model ^ (shapeIdx << 24);
        const u32 nth = g_modelSeenThisTick[key]++;
        sbr_gxfifo_draw_pop(SB_POP_SHADOW_MODEL);
        sbr_gxfifo_draw_tag(((u64)key << 32) | (u64)nth);
        ++g_modelTagged;
    }
    func_80225c30(cpu);
    if (tag) sbr_gxfifo_draw_tag(0);
    if (label) sbr_gxfifo_draw_pop(SB_POP_UNLABELLED);
    sbr_gxfifo_draw_pop(SB_POP_UNLABELLED);
}

void ov_draw_shine_shadow_volume(CPUState& cpu) {
    // This function emits TWO things: the sphere-slice display lists, which interpolate and are
    // tagged below, and two identity-matrix screen quads that are the same dst-alpha mask
    // SMS_FillScreenAlpha draws. The scope marks only the IMMEDIATE primitives (see mark_exact.h),
    // so the quads stop taking a camera delta they must not have and the slices are untouched.
    SbExactScope exact;
    // Labelled ALWAYS, tagged only when the ordinal schemes are enabled. The audit must be able to
    // say "this population snaps, and here is why" even — especially — for the paths whose tagging
    // was deliberately withdrawn; unlabelled they would be indistinguishable from draws nobody has
    // looked at.
    const bool label = sbr_lerp_enabled();
    if (label) sbr_gxfifo_draw_pop(SB_POP_SHADOW_SHINE);
    const bool was = g_shineScope;
    const u32 wasOrd = g_shineOrdinal;
    g_shineScope = true;
    g_shineOrdinal = 0;
    func_8027c67c(cpu);
    g_shineScope = was;
    g_shineOrdinal = wasOrd;
    if (label) sbr_gxfifo_draw_pop(SB_POP_UNLABELLED);
}

// TAlphaShadowQuad::mReq (+0x68) -> TCircleShadowRequest. The fields below are the ones that stay
// CONSTANT for a given caster across ticks: the two radii, the shadow type, a flag byte and the
// request flags. The position is deliberately excluded — it changes every tick, which is the whole
// point of interpolating it.
constexpr u32 QUAD_MREQ   = 0x68;
constexpr u32 REQ_UNKC    = 0x0C;   // f32
constexpr u32 REQ_UNK10   = 0x10;   // f32
constexpr u32 REQ_UNK1C   = 0x1C;   // u8  type
constexpr u32 REQ_UNK1D   = 0x1D;   // u8
constexpr u32 REQ_UNK20   = 0x20;   // u32 flags

// A fingerprint of what makes this shadow THIS shadow, rather than whoever held the slot before.
u32 request_fingerprint(u32 fp) {
    if (!sb_ram_fast(fp + QUAD_MREQ)) return 0;
    const u32 req = sb_r32(fp + QUAD_MREQ);
    if (!sb_ram_fast(req + REQ_UNK20)) return 0;
    u32 h = 2166136261u;
    auto mix = [&h](u32 v) { h ^= v; h *= 16777619u; };
    mix(sb_r32(req + REQ_UNKC));
    mix(sb_r32(req + REQ_UNK10));
    mix(sb_r8(req + REQ_UNK1C));
    mix(sb_r8(req + REQ_UNK1D));
    mix(sb_r32(req + REQ_UNK20));
    return h ? h : 1u;   // 0 is reserved for "could not read"
}

// TLiveActor::requestShadow — r3 is the actor. Held only for the duration of the call, so a
// request arriving from anywhere else cannot pick up a stale owner.
void ov_request_shadow(CPUState& cpu) {
    ++g_reqShadowCalls;
    const u32 prev = g_requestingActor;
    g_requestingActor = (u32)cpu.gpr[3];
    func_80218020(cpu);
    g_requestingActor = prev;
}

// ONE-OFF LAYOUT DISCOVERY. The join by POSITION cannot cover type-1 body shadows, because the calc
// pass rewrites their position in place before the draw (ShadowUtil.cpp:288) and the submitted value
// no longer matches. The request's ADDRESS does not change, so keying on that covers every type —
// but it requires knowing where request() stores the copy, and the header's offset comments for
// TMBindShadowManager are hand-written and demonstrably approximate (mRequests and mRequestCount are
// commented +0x10 and +0x14, four bytes apart, for an ARRAY of 0x24-byte structs).
//
// So it is measured instead of trusted: right after the real request() returns, scan the manager for
// the triple that was just submitted and report where it landed. Two hits give the base and the
// stride. Bounded to the first few requests and to a 32 KB window, and it only runs under
// SBR_SHADOW_LAYOUT=1 — it exists to be run once and have its answer written down.
void discover_layout(u32 manager, u32 req) {
    static const bool on = std::getenv("SBR_SHADOW_LAYOUT") != nullptr;
    if (!on) return;
    // SAMPLED LATE, NOT EARLY. The first version looked at the first six requests and reported "not
    // found" six times — those happen during boot, where the gate rejects them, so nothing was
    // stored and the scan was correct about a case that says nothing. Skipping into gameplay is the
    // difference between measuring the layout and measuring the title screen.
    static long seen = 0;
    if (++seen < 200000) return;
    // KEEP TRYING UNTIL SOMETHING IS FOUND, up to a bounded number of attempts. The previous version
    // stopped after six ATTEMPTS and reported six "not found"s — but request() is GATED, and a
    // rejected request stores nothing, so a handful of arbitrary samples is mostly measuring
    // rejections. Counting attempts and hits separately is what distinguishes "the layout is not
    // where I looked" from "I happened to look at requests the game threw away".
    static int hits = 0, attempts = 0;
    if (hits >= 4 || attempts >= 400 || !sb_ram_fast(req + 8)) return;
    ++attempts;
    const u32 x = sb_r32(req), y = sb_r32(req + 4), z = sb_r32(req + 8);
    for (u32 off = 0; off < 0x40000; off += 4) {
        if (!sb_ram_fast(manager + off + 8)) break;
        if (sb_r32(manager + off) == x && sb_r32(manager + off + 4) == y &&
            sb_r32(manager + off + 8) == z && (manager + off) != req) {
            ++hits;
            lucent::info("taggap",
                         "shadow layout: HIT {} (attempt {}) — request stored at manager+0x{:x} "
                         "(manager 0x{:08x}). Two or more give the array base and its stride.",
                         hits, attempts, off, manager);
            return;
        }
    }
    if (attempts == 400) {
        lucent::info("taggap",
                     "shadow layout: {} hit(s) in 400 attempts. request() is GATED, so a miss is "
                     "usually a rejected request rather than a wrong base — but 0 hits in 400 means "
                     "the manager is not r3, or the array is outside the scanned window.",
                     hits);
    }
}

// The owner id to record for a request. Normally the actor that asked, held by the requestShadow
// hook for the duration of the call — but a shadow does not have to come from a TLiveActor, and
// 3,145 requests a run do not: 2,363 from a function inside TMBindShadowManager::load's range (the
// manager's own loaded map shadows) and 782 from TFruitsBoat::requestShadow, which asks directly.
//
// For those the REQUEST'S OWN ADDRESS is the identity, and that is measured rather than assumed:
// 3,126 of the 3,145 arrive at an address that was already there on the previous tick, so the
// request lives in a persistent object. (Had they been stack temporaries the address would have
// changed with the stack and keying on it would have invented an identity — which is why the
// counter reporting both cases exists and stays.)
//
// This matters out of proportion to its size. An unresolvable member POISONS its whole shadow
// group's membership key, so one un-owned map shadow made every group containing it snap its
// alpha-restore box for that tick.
u32 owner_for(u32 req) { return g_requestingActor != 0 ? g_requestingActor : req; }

void note_request(CPUState& cpu) {
    ++g_noteReqCalls;
    if (enabled() && sbr_lerp_enabled() && g_requestingActor == 0) {
        ++g_reqNoActorSite[(u32)cpu.lr];
        const u32 req = (u32)cpu.gpr[4];
        if (g_noActorReqPrevTick.count(req) != 0) {
            ++g_noActorReqRecurred;
        } else {
            ++g_noActorReqNew;
        }
        g_noActorReqThisTick.insert(req);
    }
    if (!enabled() || !sbr_lerp_enabled()) return;
    if (g_requestingActor != 0) ++g_noteReqWithActor;
    const u32 req = (u32)cpu.gpr[4];
    if (!sb_ram_fast(req + REQ_UNK0 + 8)) return;
    g_posToActor[PosKey{sb_r32(req + REQ_UNK0), sb_r32(req + REQ_UNK0 + 4),
                        sb_r32(req + REQ_UNK0 + 8)}] = owner_for(req);
}

// After the real request() has run: work out WHERE it stored the copy, and record the owning actor
// under that address. Returns quietly when the request was rejected by the gate, which is the common
// case and not an error.
void note_stored(u32 manager, u32 req) {
    if (!enabled() || !sbr_lerp_enabled()) return;
    if (!sb_ram_fast(req + 8)) return;
    const u32 owner = owner_for(req);
    const u32 x = sb_r32(req), y = sb_r32(req + 4), z = sb_r32(req + 8);
    auto stored_at = [&](u32 addr) {
        return sb_ram_fast(addr + 8) && sb_r32(addr) == x && sb_r32(addr + 4) == y &&
               sb_r32(addr + 8) == z;
    };
    // Predict first — O(1) for every request after the base is known.
    if (g_reqBase != 0) {
        const u32 predicted = g_reqBase + g_acceptedThisTick * REQ_STRIDE;
        if (stored_at(predicted)) {
            g_addrToActor[predicted] = owner;
            ++g_acceptedThisTick;
            ++g_addrHit;
            return;
        }
    }
    // The prediction failed. Either this request was REJECTED (nothing was stored, the common case)
    // or the base is wrong. Scanning tells the two apart, and only runs on that path.
    ++g_rescans;
    for (u32 off = 0; off < 0x800; off += 4) {
        const u32 addr = manager + off;
        if (!sb_ram_fast(addr + 8)) break;
        if (addr != req && stored_at(addr)) {
            if (g_acceptedThisTick == 0) {
                g_reqBase = addr;   // first accepted request of a tick occupies slot 0
            }
            g_addrToActor[addr] = owner;
            ++g_acceptedThisTick;
            ++g_addrHit;
            return;
        }
    }
    ++g_addrMiss;   // rejected by the gate: nothing was stored, nothing to record
}

void ov_request(CPUState& cpu) {
    const u32 mgr = (u32)cpu.gpr[3], req = (u32)cpu.gpr[4];
    note_request(cpu);
    func_8022ecec(cpu);
    discover_layout(mgr, req);
    note_stored(mgr, req);
}
void ov_force_request(CPUState& cpu) {
    const u32 mgr = (u32)cpu.gpr[3], req = (u32)cpu.gpr[4];
    note_request(cpu);
    func_8022ebbc(cpu);
    discover_layout(mgr, req);
    note_stored(mgr, req);
}

// The actor that owns the shadow drawn from `fp`, or 0 if it cannot be established.
u32 owner_of(u32 fp) {
    ++g_ownerCalls;
    if (!sb_ram_fast(fp + QUAD_MREQ)) { ++g_ownerGuardQuad; return 0; }
    const u32 req = sb_r32(fp + QUAD_MREQ);
    if (!sb_ram_fast(req + REQ_UNK0 + 8)) { ++g_ownerGuardReq; return 0; }
    // ADDRESS FIRST — it covers every type, including the body shadows whose position is rewritten
    // between the request and the draw.
    {
        // Two separate lookups, not one iterator compared against the other map's end() — that is
        // undefined and it happened to compile.
        u32 found = 0;
        if (auto ia = g_addrToActorPrev.find(req); ia != g_addrToActorPrev.end()) {
            found = ia->second;
        } else if (auto ib = g_addrToActor.find(req); ib != g_addrToActor.end()) {
            found = ib->second;
        }
        if (found != 0) {
            ++g_lookupHit;
            return found;
        }
    }
    const PosKey key{sb_r32(req + REQ_UNK0), sb_r32(req + REQ_UNK0 + 4),
                     sb_r32(req + REQ_UNK0 + 8)};
    auto it = g_posToActorPrev.find(key);
    if (it == g_posToActorPrev.end()) {
        // Fall back to this tick's map, in case a scene ever requests before it draws — the
        // ordering is a property of the director's list order, not a law.
        it = g_posToActor.find(key);
        if (it == g_posToActor.end()) { ++g_lookupMiss; return 0; }
    }
    ++g_lookupHit;
    return it->second;
}


// ── THE ALPHA-RESTORE CUBE (SMS_DrawCube from TMBindShadowManager::drawShadow) ──────────────────
//
// The graphics registry reported this population as camera-only: 15k draws a run, following the
// camera but not their own motion. It is the dst-alpha box around each shadow GROUP — built from
// the group's blend-quad extents in WORLD space (ShadowUtil.cpp:571), so it moves whenever the
// actors under it do, and it was lagging them by half a tick on every in-between frame.
//
// WHERE THE GROUP COMES FROM, and why this is not a guess. SMS_DrawCube's arguments are two stack
// vectors, so the group is not in the argument registers — but PPC r14-r31 are CALLEE-SAVED, so at
// the callee's entry they still hold the caller's values. The disassembly of drawShadow around the
// call (0x8022f220) shows the loop keeping the manager in r31 and the group's BYTE OFFSET in r25:
//
//     lwz   r4, 0x1c(r31)      ; mGroups
//     addi  r0, r25, 0xc       ; + offsetof(TShadowGroup, mBoxHead)
//     lwzx  r5, r4, r0
//
// which also confirms the group layout the header gives (mMask +0, mFpHead +4, mBoxHead +0xc).
// Guarded by the return address: the registers only mean this when the caller is one of the three
// call sites in drawShadow, and any other caller of SMS_DrawCube is left alone.
//
// THE IDENTITY IS THE GROUP'S MEMBERSHIP, not the group. A group is a CLUSTER of shadow footprints,
// rebuilt every tick, and the box is the union of their extents — so "group 3" this tick and
// "group 3" last tick can be different sets of actors, and pairing on the group index would lerp
// between two unrelated boxes. Worse than the wire's case, the box always has 24 vertices, so the
// vertex-count gate cannot catch it.
//
// So the tag is a fingerprint of the group's MEMBER OWNERS, walked from mFpHead down the mNext
// chain and resolved through the same owner map the shadow volumes use. Membership changes ->
// different key -> no pair -> the box snaps, which is the correct answer when the box it would pair
// against is a different box. Membership stable -> the box interpolates with the actors it bounds.
// A member whose owner cannot be established poisons the fingerprint, because a key that ignored it
// would claim two different groups were the same one.
constexpr u32 GROUP_FPHEAD = 0x04;
constexpr u32 QUAD_MNEXT   = 0x6c;
constexpr u32 MGR_MGROUPS  = 0x1c;

// The three call sites in TMBindShadowManager::drawShadow, as return addresses (the instruction
// after each bl). Listed rather than range-checked: a range would silently adopt a fourth call site
// added by some other function that happens to sit nearby.
bool cube_from_draw_shadow(u32 lr) {
    return lr == 0x8022f224u || lr == 0x8022f434u || lr == 0x8022f478u;
}

// THE OTHER CALLER OF SMS_DrawCube, found by making the "foreign" counter name its call sites
// instead of only counting them: TModelWaterManager::drawWaterVolume, three sites, 282 draws each
// over 282 ticks.
//
// All three draw THE SAME BOX — the water volume's world-space AABB at unk5D70..unk5D7C
// (ModelWaterManager.cpp:1113-1154) — under three different render states: a repeat loop of
// `unk5D62` identical cubes that builds up destination alpha, a conditional one, and a final one in
// the water colour. So the geometry is identical across all of them and across ticks, and the box
// moves with the water, which is exactly what the vertex path interpolates.
//
// The identity is (this call site, which repeat) with no group lookup: the site is structural, and
// the repeat index is supplied by the same per-tick occurrence counter the shadow cubes use. The
// repeat COUNT is data-dependent (unk5D62), and that is harmless here in a way it would not be
// elsewhere — every repeat draws the same box, so pairing repeat k against repeat k of the previous
// tick is exact even when the count changes; a count that shrinks simply leaves the extra entries
// with nothing to pair against.
bool cube_from_water_volume(u32 lr) {
    return lr == 0x8027d7d8u || lr == 0x8027d87cu || lr == 0x8027d8e0u;
}

unsigned long g_cubeTagged = 0, g_cubeUnowned = 0, g_cubeForeign = 0;
std::unordered_map<u64, long> g_cubeKeyTick;
std::unordered_map<u64, u32> g_cubeNth;   // key -> how many cubes it has drawn THIS tick
long g_cubeNthTick = -1;
u32 g_cubeNthMax = 0;
unsigned long g_cubeKeyNew = 0, g_cubeKeyConsecutive = 0, g_cubeKeyGap = 0;
std::unordered_map<u32, unsigned long> g_cubeForeignSite;
unsigned long g_cubeWater = 0;

u64 group_membership_key(u32 mgr, u32 groupOff) {
    if (!sb_ram_fast(mgr + MGR_MGROUPS)) return 0;
    const u32 groups = sb_r32(mgr + MGR_MGROUPS);
    if (groups == 0 || !sb_ram_fast(groups + groupOff + GROUP_FPHEAD)) return 0;
    u32 fp = sb_r32(groups + groupOff + GROUP_FPHEAD);
    // ORDER-INDEPENDENT over the member owners — and it did NOT fix what it was written for, which
    // is worth saying at the code so nobody reads it as the fix.
    //
    // This used to be an FNV-1a chain whose comment argued that order dependence "would only cost a
    // snap". The vertex path's per-population breakdown looked like the bill for that assumption:
    // 2,675 of 5,162 alpha cubes in a Pianta Village run had no consecutive previous tick, against
    // 8 for the swing ropes and 1 for the bridge. Making the key order-independent changed those
    // numbers by exactly zero (2,487 lerped of 5,162, both ways).
    //
    // What the churn actually is, measured afterwards by counting key lifetimes rather than
    // theorising: the keys are STABLE. Over 290 ticks there were 62 first sightings, 2,538 keys seen
    // again on the very next tick, and 2,664 seen again after a GAP. About sixty groups exist and
    // each draws roughly every third or fourth tick — so the cubes are not changing identity, they
    // are not drawing every tick, and pairing across a gap is a different question (whether an
    // object that skipped a tick may interpolate across the skip) from the one this key answers.
    //
    // The order-independent form stays because it is the correct thing for a SET, and because the
    // measurement above is only trustworthy if the key means what it says. Each owner is mixed on
    // its own and the mixes are SUMMED, so the same actors give the same key whatever order the
    // footprint chain is in; the per-owner mix carries the strength, since summing raw pointers
    // would collide on any two groups whose addresses happen to sum alike.
    u64 h = 0;
    int members = 0;
    while (fp != 0 && members < 64) {
        const u32 owner = owner_of(fp);
        if (owner == 0) return 0;   // an unidentifiable member: refuse the whole key
        u64 m = (1469598103934665603ull ^ (u64)owner) * 1099511628211ull;
        m ^= m >> 29;
        m *= 0xbf58476d1ce4e5b9ull;
        m ^= m >> 32;
        h += m;
        if (!sb_ram_fast(fp + QUAD_MNEXT)) return 0;
        fp = sb_r32(fp + QUAD_MNEXT);
        ++members;
    }
    // The member COUNT rides along, so that two different sets whose mixes happen to sum alike are
    // still separated whenever their sizes differ.
    h = (h * 31ull) + (u64)members;
    return members == 0 ? 0 : (h | 1ull);
}

void ov_draw_shadow_volume(CPUState& cpu) {
    // r5, NOT r4. drawShadowVolume is a MEMBER function — `void TMBindShadowManager::
    // drawShadowVolume(bool useNear, TAlphaShadowQuad* fp)` — so r3 is `this`, r4 is `useNear` and
    // the quad is r5. The first version of this file read r4 and therefore keyed every shadow on a
    // BOOLEAN: all near shadows collapsed into the single identity 1, and every far shadow had
    // "fp == 0" and was skipped entirely.
    //
    // That is the real cause of the marukage rendering at the wrong place — not the slot-reuse story
    // this file previously told, which was a correct description of ShadowUtil that happened to be
    // about a pointer the code was never reading. Two of the counters said so and were not believed
    // for a while: the request fingerprint "did nothing" and the owner join refused 445,840 of
    // 445,840 lookups on an unreadable quad, both because 0x68 past a bool is not memory.
    const u32 fp = (u32)cpu.gpr[5];
    // The ACTOR that owns this shadow, joined through the position the manager copied verbatim from
    // the request. 0 when it cannot be established (a type-1 body shadow, whose position the calc
    // pass mutates before the draw) — those keep the old slot-plus-gate behaviour.
    const u32 owner = fp ? owner_of(fp) : 0;
    const u32 fingerprint = 0;
    // fp IS NOT A PER-INSTANCE IDENTITY, and an earlier version of this file claimed it was.
    //
    // ShadowUtil draws from `TAlphaShadowQuad& fp = mQuads[i]` — a FIXED ARRAY indexed by request
    // slot, and mRequestCount resets every tick while actors re-request in whatever order they
    // happen to run. So fp is a slot address: an ordinal in disguise. When the request order or
    // count changes, the actor in slot 3 this tick is a different actor from the one in slot 3 last
    // tick, and pairing on fp alone lerps one character's shadow from another's pose — which is
    // exactly the reported symptom, other characters' marukage appearing at the wrong place for a
    // single frame every few seconds.
    //
    // The sound fix is a real owner identity (TLiveActor::requestShadow, US 0x80218020, has the
    // actor in r3) mapped to the slot it lands in; that needs the manager's accept path and is not
    // done here. What IS done: fold the REQUEST's stable attributes into the key, so a slot whose
    // occupant changed produces a different tag, fails to find a partner, and SNAPS rather than
    // teleporting. Snapping is what an untagged draw already did; teleporting is strictly worse
    // than both.
    // A FINGERPRINT OF THE REQUEST WAS TRIED FIRST AND DID NOT WORK. Folding the request's stable
    // attributes (both radii, type, flag byte, flags word) into the key should have made a changed
    // occupant produce a changed tag. Measured: the 100-1k bucket went 25,113 -> 28,127 and mean
    // object motion 51.8 -> 52.2, i.e. no improvement at all — because every marukage shares the
    // same type, radius and flags, so the fingerprint is CONSTANT across casters and discriminates
    // nothing. Recorded because it is the obvious idea and it is worth not having twice.
    //
    // What IS exact: slots can only shift when the request SET changes, and the drawn count is that
    // set's size. If this tick draws a different number of shadows than the last, every slot may
    // have moved and NOTHING may be paired — so emit no tags for the tick and let them all snap for
    // one frame. This is a condition, not a threshold: it is either the same count or it is not.
    // It does not catch a same-size membership change, which is why the sound fix — a real owner
    // identity from TLiveActor::requestShadow (US 0x80218020, actor in r3) mapped to the slot it
    // lands in — is still the thing to build.
    const bool setStable = g_lastTickQuadCount == g_thisTickQuadCount + 1 ||
                           g_prevTickQuadCount == g_lastTickQuadCount;
    ++g_thisTickQuadCount;
    const bool live = enabled() && sbr_lerp_enabled() && fp != 0;
    // OWNER FIRST. With a real per-instance identity the request SLOT stops mattering, so the
    // set-change gate — which exists only because a slot's occupant can change between ticks — does
    // not apply to a draw whose owner is known.
    // ONLY THE SOUND KEY. A draw whose owning actor is known is tagged; everything else is left
    // untagged and takes the camera delta, exactly as it did before any of this work.
    //
    // The slot fallback is gone rather than gated. It is not a weaker identity, it is a WRONG one —
    // mRequestCount resets every tick and actors re-request in whatever order they run, so slot k
    // is a different caster from one tick to the next and pairing on it lerps one character's
    // shadow from another's pose. Measured across the three states, on the interpolator's own
    // mispairing signature (draws pairing with a pose 100-1000 units away):
    //     no shadow tagging at all ...................    4
    //     owner where known, slot elsewhere .......... 280
    //     slot for everything (r5, no owner join) ... 1,128
    // Keeping a fallback that is wrong-by-construction to raise a coverage number is the trade this
    // arc has already made once and had to undo.
    const bool tag = live && owner != 0;
    (void)setStable;
    (void)fingerprint;
    if (tag) {
        const u32 key = owner;
        const u32 nth = g_seenThisTick[key]++;
        sbr_gxfifo_draw_pop(SB_POP_SHADOW_VOLUME);
        sbr_gxfifo_draw_tag(((uint64_t)key << 32) | (uint64_t)nth);
        ++g_tagged;
        ++(owner != 0 ? g_ownerTagged : g_ownerMissed);
    } else if (live) {
        ++g_snappedForSetChange;
    }
    func_802305dc(cpu);
    // Close it, exactly as j3d_capture does: anything drawn after this must not inherit a shadow's
    // identity, which would pair unrelated geometry with a shadow's transform — a wrong answer that
    // renders like a working one.
    if (tag) sbr_gxfifo_draw_tag(0);
    sbr_gxfifo_draw_pop(SB_POP_UNLABELLED);
}

// Fold "which cube of this identity, this tick" into a key. Mixed in rather than added: the key is
// already a hash and its low bits carry as much information as the high ones, so a plain add would
// let (key K, cube 1) collide with (key K+1, cube 0) — two different groups' cubes wearing one
// identity.
u64 with_occurrence(u64 key) {
    const long tick = (long)aurora::gfx::interp::tick_index();
    if (tick != g_cubeNthTick) {
        g_cubeNth.clear();
        g_cubeNthTick = tick;
    }
    const u32 nth = g_cubeNth[key]++;
    if (nth > g_cubeNthMax) g_cubeNthMax = nth;
    return key ^ (0x9e3779b97f4a7c15ull * (u64)(nth + 1));
}

} // namespace

// The identity for the shadow group's alpha-restore cube, for the SMS_DrawCube seam in
// populations.cpp — one address gets one override, and that one is already spoken for, so the
// identity comes to it rather than the hook moving here. 0 means "do not tag": either the cube was
// not drawn from TMBindShadowManager::drawShadow, or the group's membership could not be
// established, and in both cases the camera delta alone is the honest answer.
u64 sbr_shadow_cube_tag(const CPUState& cpu) {
    if (!sbr_lerp_enabled()) return 0;
    if (cube_from_water_volume((u32)cpu.lr)) {
        ++g_cubeWater;
        // Salted so a water-volume site can never collide with a group-membership key, which is a
        // hash over guest pointers and could otherwise land on a small site-derived value. The
        // repeat index comes from the same per-tick occurrence counter the shadow cubes use.
        const u64 wkey = 0xcbf29ce484222325ull ^ (0x100000001b3ull * (u64)cpu.lr);
        return with_occurrence(wkey | 1ull);
    }
    if (!cube_from_draw_shadow((u32)cpu.lr)) {
        // NAME the foreign callers instead of only counting them. "846 drawn from somewhere other
        // than drawShadow and left alone" is a number nobody can act on; the return addresses say
        // WHICH code is drawing alpha cubes outside the shadow pass, which is either a fourth call
        // site this list is missing or a different system that needs its own identity.
        ++g_cubeForeign;
        ++g_cubeForeignSite[(u32)cpu.lr];
        return 0;
    }
    const u64 key = group_membership_key((u32)cpu.gpr[31], (u32)cpu.gpr[25]);
    if (key == 0) {
        ++g_cubeUnowned;
        return 0;
    }
    ++g_cubeTagged;
    // A GROUP DRAWS MORE THAN ONE CUBE PER TICK, and the tag has to say which. Measured, not
    // assumed: the vertex path's gap histogram put 2,583 of the cube draws at a gap of ZERO ticks —
    // the same tag drawing again within one tick — against 2,583 pairing normally. Each was
    // overwriting the other's recorded vertices, so both paired against the wrong pose and neither
    // could interpolate. Adding this index took the population from 48.2% to 97.3%.
    //
    // HOW MANY is measured too, and it is not what I first assumed. I wrote "two, one before and
    // one after the shadow" from reading the pass; the counter says the most any group drew in one
    // tick is FOUR. The index does not care — which is the point of counting occurrences rather than
    // hard-coding a pair.
    //
    // It IS an ordinal, which this project withdraws on sight, and it is sound here for the same
    // reason drawLower's two strips are: the sequence comes from the shadow pass's straight-line
    // code for a given group, not from a set that varies with what the scene is doing. If that ever
    // stops being true the failure is visible rather than silent — the reported maximum climbs.
    // IS THE KEY STABLE FROM TICK TO TICK? The vertex path reported 2,675 of 5,162 cubes with no
    // consecutive previous tick, which is either a key that churns or a group that genuinely does
    // not draw every tick. Those need opposite fixes, and only counting can separate them: a key
    // seen last tick and again now is stable; a key never seen before is new.
    {
        const long tick = (long)aurora::gfx::interp::tick_index();
        auto& seen = g_cubeKeyTick[key];
        if (seen == 0) {
            ++g_cubeKeyNew;
        } else if (seen + 1 == tick) {
            ++g_cubeKeyConsecutive;
        } else {
            ++g_cubeKeyGap;
        }
        seen = tick;
    }
    return with_occurrence(key);
}

void sbr_shadow_cube_report() {
    if (!sbr_lerp_enabled()) return;
    lucent::info("taggap",
                 "shadow alpha cube keys: {} first sighting(s), {} seen again on the NEXT tick "
                 "(stable — these are the ones that can interpolate), {} seen again after a GAP of "
                 "one or more ticks (the group did not draw, or its composition changed and changed "
                 "back). A key that churns every tick and a group that draws every other tick both "
                 "read as \"not consecutive\" in the vertex path; only this line separates them.",
                 g_cubeKeyNew, g_cubeKeyConsecutive, g_cubeKeyGap);
    lucent::info("taggap",
                 "water-volume cube: {} draw(s) given an identity of (call site, repeat). All three "
                 "sites draw the SAME world-space AABB under different render state, so the repeat "
                 "count being data-dependent costs nothing — every repeat is the same box.{}",
                 g_cubeWater,
                 g_cubeWater == 0
                     ? "   <-- none drew, which is scene-dependent (the water volume needs water in "
                       "the scene) and is NOT evidence the seam works."
                     : "");
    for (const auto& kv : g_cubeForeignSite) {
        lucent::info("taggap",
                     "shadow alpha cube: {} draw(s) came from return address 0x{:08x} ({}), which is "
                     "not one of TMBindShadowManager::drawShadow's three known call sites. Those get "
                     "no identity and take the camera delta alone.",
                     kv.second, kv.first, sbr_gfxdb_symbolize(kv.first));
    }
    lucent::info("taggap",
                 "shadow alpha cube: the most any single group drew in one tick was {} cube(s). The "
                 "identity carries that occurrence index, so however many a group emits they get "
                 "distinct identities rather than one wrong shared one. A number that keeps climbing "
                 "run over run would mean the index is standing in for something structural that "
                 "has not been found yet.",
                 g_cubeNthMax + 1);
    lucent::info("taggap",
                 "shadow alpha cube: {} tagged by their group's MEMBERSHIP, {} refused because a "
                 "member's owner could not be established (those snap rather than pair with a "
                 "differently-composed group), {} drawn from somewhere other than drawShadow and "
                 "left alone{}",
                 g_cubeTagged, g_cubeUnowned, g_cubeForeign,
                 (g_cubeTagged + g_cubeUnowned) == 0
                     ? "   <-- NO cube was seen from drawShadow at all. Either no shadow drew, or "
                       "the return-address guard no longer matches this build's call sites, which "
                       "would disable the identity silently."
                     : "");
}

SB_OVERRIDE(0x80218020u, ov_request_shadow, "TLiveActor::requestShadow",
            "60fps: note which ACTOR is requesting a shadow, so its draws can be keyed by a real "
            "per-instance identity instead of the request slot they happen to land in")
SB_OVERRIDE(0x8022ececu, ov_request, "TMBindShadowManager::request",
            "60fps: join the requesting actor to the request's position, which the manager copies "
            "verbatim into the slot the draw later reads")
SB_OVERRIDE(0x8022ebbcu, ov_force_request, "TMBindShadowManager::forceRequest",
            "60fps: as request(), for the ungated path")

SB_OVERRIDE(0x80225c30u, ov_sms_draw_shape, "SMS_DrawShape",
            "60fps: identity for the shadow passes that draw a model directly rather than through "
            "drawShadowVolume; keyed (model, ordinal) because no per-instance object reaches here")

SB_OVERRIDE(0x8027c67cu, ov_draw_shine_shadow_volume, "TModelWaterManager::drawShineShadowVolume",
            "60fps: mark the scope so each sphere slice of the shine shadow volume gets its own "
            "cross-tick identity; observe-only, always runs the real body")

SB_OVERRIDE(0x802305dcu, ov_draw_shadow_volume, "TMBindShadowManager::drawShadowVolume",
            "60fps: give each shadow instance (TAlphaShadowQuad*) a cross-tick identity so it "
            "interpolates with its caster instead of taking the camera delta alone")
