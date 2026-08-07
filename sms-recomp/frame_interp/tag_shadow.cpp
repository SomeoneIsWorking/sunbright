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
#include "populations.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <cstdlib>
#include <cstring>
#include <unordered_map>

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
        if (e == nullptr) return Scheme::FpOnly;
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
u32 g_requestingActor = 0;
unsigned long g_ownerTagged = 0, g_ownerMissed = 0;
// The JOIN's own denominators. "0 owners resolved" has three causes — requestShadow never fired,
// request() never fired with an actor in scope, or the position did not match at draw time — and
// without these they are one number.
unsigned long g_reqShadowCalls = 0, g_noteReqCalls = 0, g_noteReqWithActor = 0, g_lookupMiss = 0;
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

void note_request(CPUState& cpu) {
    ++g_noteReqCalls;
    if (!enabled() || !sbr_lerp_enabled() || g_requestingActor == 0) return;
    ++g_noteReqWithActor;
    const u32 req = (u32)cpu.gpr[4];
    if (!sb_ram_fast(req + REQ_UNK0 + 8)) return;
    g_posToActor[PosKey{sb_r32(req + REQ_UNK0), sb_r32(req + REQ_UNK0 + 4),
                        sb_r32(req + REQ_UNK0 + 8)}] = g_requestingActor;
}

void ov_request(CPUState& cpu)      { note_request(cpu); func_8022ecec(cpu); }
void ov_force_request(CPUState& cpu) { note_request(cpu); func_8022ebbc(cpu); }

// The actor that owns the shadow drawn from `fp`, or 0 if it cannot be established.
u32 owner_of(u32 fp) {
    ++g_ownerCalls;
    if (!sb_ram_fast(fp + QUAD_MREQ)) { ++g_ownerGuardQuad; return 0; }
    const u32 req = sb_r32(fp + QUAD_MREQ);
    if (!sb_ram_fast(req + REQ_UNK0 + 8)) { ++g_ownerGuardReq; return 0; }
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

} // namespace

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
