// startanim_test — spec-derived unit test for the pure predicates
// underneath TMapObjBase::startAnim (@0x801b09d4). Pure logic,
// Dolphin-free / no ROM / no GPU.
//
// Regressions this catches:
//   * kSentinelSlot drift (e.g. writing 0 or -1 instead of 0xFFFF — would make
//     the outgoing-slot reset re-run every frame).
//   * has_slot() using `>=` instead of `>` — OOB read at the fencepost.
//   * kMapObjAnimDataSize drift (0x14 confirmed by two independent mulli
//     sites in the disasm).
//   * skip_reload() being flipped — either always reload (breaks getMActor
//     caching) or never reload (stale mMActor after slot change).
//   * kMapObjFlagBit_ClearOnStart drift — startBck uses the SAME 0x100 bit,
//     so a drift here would silently desync the two entry points.

#include "sms_boot_startanim.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

int main() {
    using namespace sb::startanim;

    // Hex constants — direct from the RE.
    CHECK(kSentinelSlot == 0xFFFFu, "kSentinelSlot must be 0xFFFF (matches makeObjDead reset value)");
    CHECK(kMapObjAnimDataSize == 0x14u, "TMapObjAnimData size is 0x14 (mulli r0,r0,0x14)");
    CHECK(kMapObjFlagBit_ClearOnStart == 0x100u, "startAnim/startBck flag bit (rlwinm 0,24,22 → bit 23 → 0x100)");
    CHECK(kMapObjFlagBit_ForceClearGate == 0x200u, "force-clear gate bit (rlwinm.  0,22,22 → bit 22 → 0x200)");
    CHECK(kMActorAnmReset == -1, "MActorAnmBase::unk0 reset value = -1 (matches makeObjDead)");

    // has_slot — strict > , not >=.
    CHECK( has_slot(1, 0),  "count=1 idx=0 → in-range");
    CHECK( has_slot(5, 4),  "count=5 idx=4 → last in-range");
    CHECK(!has_slot(4, 4),  "count=4 idx=4 → FENCEPOST: strict > required, MUST reject");
    CHECK(!has_slot(0, 0),  "count=0 idx=0 → nothing to play");
    CHECK(!has_slot(3, 7),  "count=3 idx=7 → out-of-range");
    // A relaxed `>=` would incorrectly accept the fencepost:
    CHECK(!has_slot(2, 2),  "count=2 idx=2 → MUST reject (would OOB-read anim.unk4[2])");

    // is_sentinel
    CHECK( is_sentinel(0xFFFFu), "0xFFFF is the sentinel");
    CHECK(!is_sentinel(0x0000u), "0 is a valid slot");
    CHECK(!is_sentinel(0xFFFEu), "0xFFFE is a valid (albeit huge) slot — sentinel is exactly 0xFFFF");

    // should_lazy_load_mactor
    CHECK(!should_lazy_load_mactor(true,  3), "already have mMActor → no lazy load");
    CHECK( should_lazy_load_mactor(false, 3), "no mMActor + entries exist → lazy-load from anim[0]");
    CHECK(!should_lazy_load_mactor(false, 0), "no mMActor but no entries → cannot lazy-load");

    // skip_reload — same slot means keep mMActor as-is.
    CHECK( skip_reload(2, 2), "same slot → skip mMActor reload");
    CHECK(!skip_reload(2, 3), "slot changed → reload");
    CHECK(!skip_reload(kSentinelSlot, 0), "sentinel → not the same as slot 0 → reload");

    // can_reset_mtxcalc — the native compat predicate gating the mtx-calc
    // reset write in the "no anim name" tail branch. It must return TRUE only
    // when ALL three pointers along the RE write path are non-null: mMActor's
    // MtxCalc (unk8), the model's ModelData, and the ModelData's root joint.
    // A single null anywhere in the chain → skip the reset, avoiding the
    // TFileLoadBlock::makeBlockNormal SEGV that happens when MActor::setModel
    // hasn't run yet ([[fileselect-startanim-mtxcalc-null]]).
    CHECK( can_reset_mtxcalc(true,  true,  true),  "all three pointers OK → do the reset");
    CHECK(!can_reset_mtxcalc(false, true,  true),  "MtxCalc null (setModel not run yet) → skip");
    CHECK(!can_reset_mtxcalc(true,  false, true),  "ModelData null (model not bound) → skip");
    CHECK(!can_reset_mtxcalc(true,  true,  false), "root joint null (empty model data) → skip");
    CHECK(!can_reset_mtxcalc(false, false, false), "all null → skip (upstream MActor un-init)");
    // Polarity smoke: predicate must NOT be inverted. A flipped version would
    // do the write EXACTLY when it's unsafe.
    CHECK( can_reset_mtxcalc(true,  true,  true) != false, "polarity: all-true → true (not inverted)");

    if (g_fail) { std::fprintf(stderr, "startanim_test: %d FAILURE(S)\n", g_fail); return 1; }
    std::printf("startanim_test: all passed\n");
    return 0;
}
