// recomp_dead_stubs.cpp — GENERATED scaffolding for Phase C recomp eradication (2026-06-18).
//
// The static recompiler is no longer linked into `sunbright` (CMakeLists.txt dropped the
// generated/ glob). A number of recomp-era override / diagnostic files still NAME generated
// function bodies directly (the "super-call via direct func_XXXXXXXX symbol" pattern) instead
// of via recomp_lookup/recomp_raw. Every such call lives in a code path that is INERT under the
// no-recomp architecture (the override is not purejit-safe, so it is never dispatched — Dolphin's
// JIT runs the original instead). These weak stubs satisfy the linker for that dead code WITHOUT
// reviving the recompiler. They are verified-unreachable, so if one is ever actually called it is
// a bug in the deadness analysis — it aborts loudly, naming itself, rather than corrupting state.
//
// TRANSITIONAL: as the dead super-call/diagnostic code is deleted, the corresponding stubs go too.
// Regenerate the symbol set with:
//   grep -rho 'func_[0-9a-fA-F]\{8\}' runtime/ | grep -v generated | sort -u
#include "cpu_state.h"
#include <cstdio>
#include <cstdlib>

namespace {
[[noreturn]] void sb_dead_recomp(const char* name) {
    std::fprintf(stderr, "[FATAL] dead recomp body %s called under no-recomp — deadness analysis "
                         "wrong (this path should be inert; Dolphin JIT should own it)\n", name);
    std::abort();
}
}  // namespace

extern "C" {
__attribute__((weak)) void func_80014d1c(CPUState&) { sb_dead_recomp("func_80014d1c"); }
__attribute__((weak)) void func_8013f860(CPUState&) { sb_dead_recomp("func_8013f860"); }
__attribute__((weak)) void func_8013fa54(CPUState&) { sb_dead_recomp("func_8013fa54"); }
__attribute__((weak)) void func_8013fc88(CPUState&) { sb_dead_recomp("func_8013fc88"); }
__attribute__((weak)) void func_80140390(CPUState&) { sb_dead_recomp("func_80140390"); }
__attribute__((weak)) void func_802260cc(CPUState&) { sb_dead_recomp("func_802260cc"); }
__attribute__((weak)) void func_80227914(CPUState&) { sb_dead_recomp("func_80227914"); }
__attribute__((weak)) void func_8022f014(CPUState&) { sb_dead_recomp("func_8022f014"); }
__attribute__((weak)) void func_8022fa40(CPUState&) { sb_dead_recomp("func_8022fa40"); }
__attribute__((weak)) void func_80231108(CPUState&) { sb_dead_recomp("func_80231108"); }
__attribute__((weak)) void func_8023d36c(CPUState&) { sb_dead_recomp("func_8023d36c"); }
__attribute__((weak)) void func_8027beb0(CPUState&) { sb_dead_recomp("func_8027beb0"); }
__attribute__((weak)) void func_80296a50(CPUState&) { sb_dead_recomp("func_80296a50"); }
__attribute__((weak)) void func_80299838(CPUState&) { sb_dead_recomp("func_80299838"); }
__attribute__((weak)) void func_802a4e28(CPUState&) { sb_dead_recomp("func_802a4e28"); }
__attribute__((weak)) void func_802a5bd4(CPUState&) { sb_dead_recomp("func_802a5bd4"); }
__attribute__((weak)) void func_802a5f50(CPUState&) { sb_dead_recomp("func_802a5f50"); }
__attribute__((weak)) void func_802a6398(CPUState&) { sb_dead_recomp("func_802a6398"); }
__attribute__((weak)) void func_802deeb8(CPUState&) { sb_dead_recomp("func_802deeb8"); }
__attribute__((weak)) void func_802e0390(CPUState&) { sb_dead_recomp("func_802e0390"); }
__attribute__((weak)) void func_802ecbd0(CPUState&) { sb_dead_recomp("func_802ecbd0"); }
__attribute__((weak)) void func_802f80d0(CPUState&) { sb_dead_recomp("func_802f80d0"); }
__attribute__((weak)) void func_802f8bac(CPUState&) { sb_dead_recomp("func_802f8bac"); }
__attribute__((weak)) void func_80300ab8(CPUState&) { sb_dead_recomp("func_80300ab8"); }
__attribute__((weak)) void func_80300ce4(CPUState&) { sb_dead_recomp("func_80300ce4"); }
__attribute__((weak)) void func_803017b0(CPUState&) { sb_dead_recomp("func_803017b0"); }
__attribute__((weak)) void func_80301a28(CPUState&) { sb_dead_recomp("func_80301a28"); }
__attribute__((weak)) void func_80301e80(CPUState&) { sb_dead_recomp("func_80301e80"); }
__attribute__((weak)) void func_80301fc4(CPUState&) { sb_dead_recomp("func_80301fc4"); }
__attribute__((weak)) void func_80302034(CPUState&) { sb_dead_recomp("func_80302034"); }
__attribute__((weak)) void func_803020ac(CPUState&) { sb_dead_recomp("func_803020ac"); }
__attribute__((weak)) void func_80302224(CPUState&) { sb_dead_recomp("func_80302224"); }
__attribute__((weak)) void func_8030241c(CPUState&) { sb_dead_recomp("func_8030241c"); }
__attribute__((weak)) void func_803029a4(CPUState&) { sb_dead_recomp("func_803029a4"); }
__attribute__((weak)) void func_80303fac(CPUState&) { sb_dead_recomp("func_80303fac"); }
__attribute__((weak)) void func_8030a57c(CPUState&) { sb_dead_recomp("func_8030a57c"); }
__attribute__((weak)) void func_8030a604(CPUState&) { sb_dead_recomp("func_8030a604"); }
__attribute__((weak)) void func_8030a68c(CPUState&) { sb_dead_recomp("func_8030a68c"); }
__attribute__((weak)) void func_8030ad44(CPUState&) { sb_dead_recomp("func_8030ad44"); }
__attribute__((weak)) void func_8030ae44(CPUState&) { sb_dead_recomp("func_8030ae44"); }
__attribute__((weak)) void func_8030af44(CPUState&) { sb_dead_recomp("func_8030af44"); }
__attribute__((weak)) void func_8030b330(CPUState&) { sb_dead_recomp("func_8030b330"); }
__attribute__((weak)) void func_8030b5e0(CPUState&) { sb_dead_recomp("func_8030b5e0"); }
__attribute__((weak)) void func_8030b700(CPUState&) { sb_dead_recomp("func_8030b700"); }
__attribute__((weak)) void func_8030b8c8(CPUState&) { sb_dead_recomp("func_8030b8c8"); }
__attribute__((weak)) void func_8030ba90(CPUState&) { sb_dead_recomp("func_8030ba90"); }
__attribute__((weak)) void func_8030bc58(CPUState&) { sb_dead_recomp("func_8030bc58"); }
__attribute__((weak)) void func_8030be20(CPUState&) { sb_dead_recomp("func_8030be20"); }
__attribute__((weak)) void func_80310994(CPUState&) { sb_dead_recomp("func_80310994"); }
__attribute__((weak)) void func_803112d0(CPUState&) { sb_dead_recomp("func_803112d0"); }
__attribute__((weak)) void func_803140cc(CPUState&) { sb_dead_recomp("func_803140cc"); }
__attribute__((weak)) void func_80314f50(CPUState&) { sb_dead_recomp("func_80314f50"); }
__attribute__((weak)) void func_8031505c(CPUState&) { sb_dead_recomp("func_8031505c"); }
__attribute__((weak)) void func_8031d83c(CPUState&) { sb_dead_recomp("func_8031d83c"); }
__attribute__((weak)) void func_80320f30(CPUState&) { sb_dead_recomp("func_80320f30"); }
__attribute__((weak)) void func_8033ba90(CPUState&) { sb_dead_recomp("func_8033ba90"); }
__attribute__((weak)) void func_803433b4(CPUState&) { sb_dead_recomp("func_803433b4"); }
__attribute__((weak)) void func_80347798(CPUState&) { sb_dead_recomp("func_80347798"); }
__attribute__((weak)) void func_80347b20(CPUState&) { sb_dead_recomp("func_80347b20"); }
__attribute__((weak)) void func_80348948(CPUState&) { sb_dead_recomp("func_80348948"); }
__attribute__((weak)) void func_80348a68(CPUState&) { sb_dead_recomp("func_80348a68"); }
__attribute__((weak)) void func_80348ee8(CPUState&) { sb_dead_recomp("func_80348ee8"); }
__attribute__((weak)) void func_803492e0(CPUState&) { sb_dead_recomp("func_803492e0"); }
__attribute__((weak)) void func_8034acd8(CPUState&) { sb_dead_recomp("func_8034acd8"); }
__attribute__((weak)) void func_8034e548(CPUState&) { sb_dead_recomp("func_8034e548"); }
__attribute__((weak)) void func_8035532c(CPUState&) { sb_dead_recomp("func_8035532c"); }
__attribute__((weak)) void func_8035796c(CPUState&) { sb_dead_recomp("func_8035796c"); }
__attribute__((weak)) void func_8035d8f0(CPUState&) { sb_dead_recomp("func_8035d8f0"); }
__attribute__((weak)) void func_8035dae8(CPUState&) { sb_dead_recomp("func_8035dae8"); }
__attribute__((weak)) void func_8035ecec(CPUState&) { sb_dead_recomp("func_8035ecec"); }
__attribute__((weak)) void func_8035ee5c(CPUState&) { sb_dead_recomp("func_8035ee5c"); }
__attribute__((weak)) void func_8035ffb8(CPUState&) { sb_dead_recomp("func_8035ffb8"); }
}  // extern "C"
