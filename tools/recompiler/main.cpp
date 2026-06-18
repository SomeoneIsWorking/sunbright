#include <cstdio>
#include "disc_loader.h"
#include "dol_parser.h"
#include "ppc_decoder.h"
#include "c_emitter.h"
#include "func_collect.h"

#include <iostream>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <cstring>
#include <map>
#include <set>
#include <algorithm>
#include <unordered_map>

namespace fs = std::filesystem;

// A function we cannot faithfully recompile inline: it writes MSR, returns from
// an interrupt, or pokes the MMU/segment/TLB state. These instructions redirect
// control flow or carry hardware side effects that only Dolphin's JIT reproduces,
// so we leave the whole function out of the recomp table and let the JIT run it.
// SPRs we model directly in CPUState; mtspr/mfspr to anything else (HID0/HID2,
// L2CR, WPAR, BATs, DABR, …) carries HW side effects only Dolphin reproduces.
static bool spr_is_modeled(u16 spr) {
    if (spr == SPR_XER || spr == SPR_LR || spr == SPR_CTR) return true;
    if (spr == SPR_SRR0 || spr == SPR_SRR1) return true;   // exception/context save-restore — modeled
    if (spr >= SPR_GQR0 && spr <= SPR_GQR0 + 7) return true;
    return false;
}

static bool function_needs_jit(const std::vector<PPCInstr>& instrs) {
    for (const auto& i : instrs) {
        switch (i.op) {
        // MTMSR (→ msr_set_raw) and RFI (→ restore SRR1→MSR + branch SRR0) are modeled in the recomp,
        // so the OS interrupt/scheduler/context-switch primitives are recompiled (PC port owns them).
        // NOTE (2026-06-05): routing MTMSR/RFI back to JIT here HANGS boot immediately at early OS
        // init (the first context switch spins under run_jit_sync) — the pre-fb76ced handoff-based
        // boot can't be restored by function_needs_jit alone; the recompiled scheduler boots further
        // (reaches THP). The post-THP crash is fixed by finishing native threading, not this revert.
        // Only genuine HW side effects below stay JIT.
        case PPCOp::MTSR:  case PPCOp::MTSRIN:
        case PPCOp::TLBIE: case PPCOp::TLBSYNC:
        case PPCOp::ECIWX: case PPCOp::ECOWX:
            return true;
        case PPCOp::MTSPR:
        case PPCOp::MFSPR:
            // Touching a hardware SPR (cache/MMU/gather-pipe/power) — needs the JIT.
            if (!spr_is_modeled(decode_spr(i.spr))) return true;
            break;
        default:
            break;
        }
    }
    return false;
}

static void print_usage(const char* prog) {
    std::cerr << "Usage:\n"
              << "  " << prog << " <disc.rvz|disc.iso> --output <dir>    # Recompile\n"
              << "  " << prog << " <disc.rvz|disc.iso> --analyze-only     # Analyze\n"
              << "  " << prog << " <disc.rvz|disc.iso> --dump-dol --output <path>\n"
              << "  " << prog << " <disc> --disasm <hexaddr> [count]      # Disassemble\n"
              << "  " << prog << " <disc> --xref <hexaddr> [--funcs f]    # Who calls/branches to ADDR\n"
              << "  " << prog << " <disc> --callees <hexaddr> [--funcs f] # What the fn at ADDR calls\n"
              << "  " << prog << " --version\n";
}

static void print_version() {
    std::cout << "sunbright-recomp 0.1.0\n"
              << "GameCube/PPC static recompiler\n"
              << "Built: " << __DATE__ << " " << __TIME__ << "\n";
}

static void analyze_mode(const DiscLoader& disc, const DOL& dol) {
    DiscInfo di = disc.info();
    std::cout << "=== Disc Info ===\n"
              << "Game ID:  " << di.game_id << "\n"
              << "Title:    " << di.title   << "\n\n";

    std::cout << "=== DOL Layout ===\n"
              << "Entry:    0x" << std::hex << dol.entry << "\n";
    for (const auto& s : dol.sections) {
        std::cout << (s.is_text ? "  TEXT" : "  DATA")
                  << "  0x" << std::hex << s.addr
                  << " – 0x" << (s.addr + s.size)
                  << "  (" << std::dec << s.size << " bytes)\n";
    }

    std::cout << "\n=== Function Scan ===\n";
    size_t total_funcs = 0;
    size_t total_instrs = 0;

    for (const auto& [base, data] : dol.text_sections()) {
        auto funcs = find_functions(data.data(), base, data.size());
        std::cout << "  Section 0x" << std::hex << base << ": "
                  << std::dec << funcs.size() << " functions\n";
        total_funcs += funcs.size();
        total_instrs += data.size() / 4;
    }

    std::cout << "  Total: " << total_funcs << " functions, "
              << total_instrs << " instructions\n";

    std::cout << "\n=== Filesystem ===\n";
    auto files = disc.list_files();
    std::cout << "  " << files.size() << " files\n";
    size_t rel_count = 0;
    for (const auto& f : files) {
        if (f.ends_with(".rel")) rel_count++;
        if (files.size() <= 20 || f.ends_with(".rel") || f.ends_with(".dol"))
            std::cout << "  " << f << "\n";
    }
    if (rel_count > 0)
        std::cout << "  NOTE: " << rel_count << " REL modules found — recompile those too!\n";

    std::cout << "\n=== Opcode Histogram ===\n";
    std::map<PPCOp, int> hist;
    for (const auto& [base, data] : dol.text_sections()) {
        for (u32 off = 0; off + 4 <= data.size(); off += 4) {
            u32 w_be;
            std::memcpy(&w_be, data.data() + off, 4);
            u32 w = __builtin_bswap32(w_be);
            PPCInstr ins = decode(w, base + off);
            hist[ins.op]++;
        }
    }
    // Print top 20
    std::vector<std::pair<int, PPCOp>> sorted;
    for (auto& [op, cnt] : hist) sorted.emplace_back(cnt, op);
    std::sort(sorted.rbegin(), sorted.rend());
    int shown = 0;
    for (auto& [cnt, op] : sorted) {
        if (++shown > 20) break;
        PPCInstr dummy{}; dummy.op = op;
        std::cout << "  " << std::setw(8) << cnt << "x  " << dummy.mnemonic() << "\n";
    }
    int unknown = hist[PPCOp::UNKNOWN];
    if (unknown)
        std::cout << "  WARNING: " << unknown << " UNKNOWN instructions\n";
}

static int recompile_mode(const DiscLoader& disc, const DOL& dol, const std::string& out_dir) {
    fs::create_directories(out_dir);

    // Clean stale generated sources first. The bucket filenames are keyed by the
    // first function address in each chunk, so when the discovered function set
    // changes the buckets shift and old files would linger → duplicate symbols.
    for (const auto& e : fs::directory_iterator(out_dir)) {
        const std::string n = e.path().filename().string();
        if ((n.rfind("functions_", 0) == 0 && n.size() > 4 &&
             n.substr(n.size() - 4) == ".cpp") ||
            n == "functions.cpp" || n == "functions.h" || n == "jump_table.cpp")
            fs::remove(e.path());
    }

    std::vector<u32> all_funcs;
    std::unordered_map<u32, std::vector<PPCInstr>> func_instrs;

    // Discover function entries referenced by POINTER (vtables / function-pointer
    // tables), which bl/bc-target scanning misses — virtual methods are called via
    // bctr through a vtable, so their entries only appear as 32-bit pointers in
    // .data/.text. Without this they fall through to Dolphin's JIT entirely.
    // A candidate is a 4-byte-aligned word that points into .text, where the slot
    // looks like a real function start: it begins with a sane instruction and is
    // immediately preceded by a function terminator (blr/bctr/b/rfi/padding) or a
    // section boundary — so we don't split a real function mid-body.
    auto text_word = [&](u32 addr, u32& out) -> bool {
        for (const auto& s : dol.sections)
            if (s.is_text && addr >= s.addr && addr + 4 <= s.addr + s.size) {
                std::memcpy(&out, s.data.data() + (addr - s.addr), 4);
                out = __builtin_bswap32(out);
                return true;
            }
        return false;
    };
    auto is_term = [](u32 w) -> bool {
        if (w == 0x4E800020 || w == 0x4E800420) return true;   // blr, bctr
        if (w == 0) return true;                                // padding
        u32 op = w >> 26;
        if (op == 18 && !(w & 1)) return true;                  // b (no lk)
        if (op == 19) { u32 xo = (w >> 1) & 0x3ff; if (xo == 16 || xo == 528) return true; } // bclr/bcctr
        return false;
    };
    auto is_section_start = [&](u32 a) -> bool {
        for (const auto& s : dol.sections) if (s.is_text && a == s.addr) return true;
        return false;
    };
    // Pointer/vtable discovery is OPT-IN (SUNBRIGHT_DISCOVER_POINTERS): it roughly
    // doubles recompiled coverage, but under the current dispatch call model every
    // recomp→recomp return bounces to the JIT, so more recompiled code = more
    // round-trips = slower — and the newly-pulled-in code still has recomp bugs.
    // Default off keeps the validated, fast 6032-function baseline. Re-enable once
    // the correctness harness validates the extra code and the C-call model lands.
    std::unordered_set<u32> ptr_funcs;
    if (getenv("SUNBRIGHT_DISCOVER_POINTERS")) {
        // A value `v` is a function-entry candidate iff it points into .text, the first word
        // there is real code (not padding/terminator), and it sits at a function BOUNDARY
        // (a section start, or preceded by a terminator). The boundary test rejects interior
        // labels and stray data addresses that happen to land in .text, so over-adding is safe
        // (an extra real function is harmless; ptr entries never act as fend boundaries).
        auto consider = [&](u32 v) {
            if (v & 3) return;
            u32 w, prev;
            if (!text_word(v, w)) return;                // points into .text?
            if (w == 0 || is_term(w)) return;            // first insn must be real code
            if (is_section_start(v) || (text_word(v - 4, prev) && is_term(prev)))
                ptr_funcs.insert(v);
        };

        // (1) Data-stored pointers (vtables, function-pointer tables): any aligned word that
        //     looks like a .text entry.
        for (const auto& s : dol.sections)
            for (u32 off = 0; off + 4 <= s.size; off += 4) {
                u32 v; std::memcpy(&v, s.data.data() + off, 4); v = __builtin_bswap32(v);
                consider(v);
            }

        // (2) CODE-materialized pointers: `lis rX,hi` then `addi/ori rY,rX,lo` builds an address
        //     in a register. When that address lands in .text it is a function pointer passed by
        //     value (e.g. the per-element ctor handed to __construct_array) — NOT reachable via
        //     any data pointer or direct branch, so (1)/CFG/symbols all miss it, and it would
        //     fall to the interpreter. We track the lis high-half per register and pair it with a
        //     following addi/ori in the same basic block (cleared at any branch/terminator).
        //     OPT-IN (SUNBRIGHT_DISCOVER_CODEPTRS), separate from data-pointer discovery: it also
        //     pulls in ~454 previously-interpreted funcs incl. HW/MMU-sensitive OS code
        //     (__OSInitMemoryProtection, OSReceiveMessage, DVDInquiryAsync) that don't recompile
        //     cleanly yet (they need function_needs_jit-style routing / native ports first), so
        //     enabling it as-is destabilizes boot. WIP — keep off until those are handled.
        for (const auto& s : dol.sections) {
            if (!getenv("SUNBRIGHT_DISCOVER_CODEPTRS")) break;
            if (!s.is_text) continue;
            u32 hi[32]; bool hi_ok[32] = {};
            for (u32 off = 0; off + 4 <= s.size; off += 4) {
                u32 w; std::memcpy(&w, s.data.data() + off, 4); w = __builtin_bswap32(w);
                const u32 op = w >> 26;
                const u32 f21 = (w >> 21) & 31, f16 = (w >> 16) & 31;  // the two GPR fields
                // First CONSUME a tracked high-half (lis result still live in the base register).
                if (op == 14 && f16 != 0 && hi_ok[f16])               // addi rD,rA,SIMM
                    consider(hi[f16] + (u32)(s32)(s16)(w & 0xffff));
                else if (op == 24 && hi_ok[f21])                      // ori rA,rS,UIMM (rS=f21)
                    consider(hi[f21] | (w & 0xffff));
                // Then UPDATE validity. SOUNDNESS RULE: pair a lis only with a following addi/ori
                // that has NO intervening write to the base register. We don't fully decode every
                // opcode's destination, so conservatively invalidate BOTH GPR fields of every
                // instruction (over-invalidating only costs a missed pair → that fn falls to the
                // interpreter, which is handled; under-invalidating would mint a BOGUS entry from a
                // stale high-half — that crashed boot). A branch/terminator clears all (block end).
                if (op == 15 && f16 == 0) {              // lis rD,SIMM — set the tracked high-half
                    hi[f21] = (u32)((s32)(s16)(w & 0xffff) << 16); hi_ok[f21] = true;
                } else if (is_term(w)) {
                    for (bool& b : hi_ok) b = false;
                } else {
                    hi_ok[f21] = false; hi_ok[f16] = false;
                }
            }
        }
        std::printf("Pointer-referenced function candidates: %zu\n", ptr_funcs.size());
    }

    // Collect all functions from text sections
    for (const auto& [base, data] : dol.text_sections()) {
        // real_funcs = genuine function starts (symbol/heuristic). These — and ONLY these —
        // delimit function ENDS (fend). Pointer-discovered entries are additional ENTRY points
        // but must NOT act as fend boundaries: an interior label between two real functions would
        // otherwise shrink the preceding function's fend and re-truncate it (the JAIBasic audio
        // crash — checkInitDataFile was cut at a discovered interior label, splitting its body).
        const u32 sec_end = base + (u32)data.size();
        auto real_funcs = find_functions(data.data(), base, data.size());
        std::sort(real_funcs.begin(), real_funcs.end());
        real_funcs.erase(std::unique(real_funcs.begin(), real_funcs.end()), real_funcs.end());

        // Forced ENTRY points: real function starts reached ONLY via indirect (vtable/fn-ptr)
        // calls that neither symbols, CFG, nor the data-pointer scan discover, so they fall
        // through to the interpreter at runtime (SUNBRIGHT_INTERP_PROFILE flagged these as the
        // dominant interpreter load — JAudio/THP tick methods). Verified real prologues (mfspr
        // r0,LR; word at addr-4 is blr). Merged as ENTRY points ONLY (like ptr_funcs) — they are
        // NOT added to real_funcs, so they never act as fend boundaries (re-truncating the
        // preceding function — the JAIBasic-style bug). Also force-CFG below (collect whole).
        static const std::unordered_set<u32> kForceEntry = {
            0x8031d83cu,  // JASystem::TTrack tick (87.2% of interp steps)
            0x8001fa88u,  // THP movie player region (6.2%)
            0x80316ffcu,  // JASystem::Kernel portCmdInit region (2.5%)
            0x803121acu,  // __UpdateJcToDSP__Q28JASystem6Driver (0.7%)
            0x8031a2ecu,  // read16__Q28JASystem8TSeqCtrl (0.2%)
            // 2nd wave: undiscovered indirect-callees the above redistributed load onto
            // (re-profiled after forcing the first wave). Same JAudio class, verified real
            // function starts (prologue + preceding blr).
            0x803399ccu,  // JAudio region (44.5% of residual interp steps, long loop)
            0x8031a50cu,  // JAudio leaf (18.5%)
            0x8030fe50u,  // JAudio leaf (10.9%)
            0x80313ddcu,  // JAudio region (9.2%)
            // 3rd wave: the JASystem audio THREAD ENTRIES themselves (OSCreateThread targets — real
            // prologues, preceded by blr). Without these the audio thread runs its WHOLE life under
            // the interpreter (guest_thread_body falls to interp_run_until when the entry isn't a
            // recomp func), where the general-table TTrack-tick override (ttrack_tick_native) is NOT
            // consulted, so the tick spins and the cooperative nthr scheduler never switches to the
            // renderer → vps=0. Recompiled, the thread runs on the native stack: the override fires
            // and OS block points yield. (Ref mislabels them portCmdInit+0x278 / TCardManager+0x184.)
            0x803171ecu,  // JASystem::Kernel audio thread proc (portCmdInit)
            0x80311170u,  // JASystem::AudioThread main proc — the DSP-synced mix/seq thread (entry
                          // missed by discovery): its loop OSReceiveMessage's each audio frame then
                          // updateDac → TSeqParser::mainProc → TTrack tick. Under interp the
                          // ttrack_tick_native override is skipped so the tick spins (vps=0);
                          // recompiled it runs on the native stack and the override fires.
            0x80312000u,  // JASystem DSP-channel loop head INSIDE func_8031204c's body: 8031204c is
                          // also a real indirect entry, so its emission starts mid-loop and the
                          // backward branch to 80312000 left ITS body → tail_ppc handoff at every
                          // audio frame boundary (unwound the native audioproc loop, 2026-06-11).
                          // As a forced ENTRY the loop head gets its own emission; the backward
                          // branch becomes a recomp call that returns via blr.
            0x802b3264u,  // TCardManager thread proc (NOT audio — ref mislabels it TCardManager+0x184;
                          // its CARDProbeEx/__EXIProbe debounce loop is unblocked by the OSYieldThread
                          // CoreTiming-advance fix, ecd63ac).
            // 4th wave (2026-06-10): EVERY remaining OSCreateThread entry observed at boot —
            // verified mflr prologues. Pointer-only targets (entry built by lis/addi into
            // OSCreateThread's r4): neither bl-reachability nor data-table pointer discovery
            // finds them, so their threads ran their WHOLE LIVES under the interpreter (~100x):
            // the THP video decode crawled at 134M+ interp steps (pc=80371af0, Huffman inner
            // loop) = slow intro movie + general boot sluggishness + no input responsiveness.
            0x802c54b8u,  // JKRThread::start trampoline (4 worker threads incl. THP decode pool)
            0x802a9184u,  // render/draw-sync thread
            0x802a7878u,  // boot setup thread A
            0x802a7080u,  // boot setup thread B
            0x802b6fdcu,  // boot stage thread
            0x8001dcd0u,  // game thread (movie era)
            0x8001fc04u,  // game thread (movie era)
            0x800200d8u,  // game thread (movie era)
            0x80296dd4u,  // game thread
            // GX draw-sync token callback chain (registered via GXSetDrawSyncCallback — pointer-only;
            // the funcs-map label at 802a8db8 is a DIFFERENT method, the live global holds 802a9318):
            0x802a9318u,  // TDrawSyncManager::drawSyncCallback (static)
            0x802a9078u,  // TDrawSyncManager::drawSyncCallbackSub
            // GX shared TEV dispatchers (perf): tiny recompiled GXSetTev* entries TAIL (`b`) into
            // these shared bctr dispatchers; un-recompiled, every TEV call siglongjmp'd to the JIT
            // and bounced back — 12.26M of 12.58M tails at file select = the 0.6 fps in-scene
            // crawl (tail-hist, 2026-06-10). As recomp entries the chain stays on the C stack
            // (their bctr cases are already discovered entries).
            0x8035ce14u,  // GX TEV-register dispatcher (97.5% of tails)
            0x8035c334u,  // second GX dispatcher (2.5%)
        };

        auto funcs = real_funcs;                       // all recomp ENTRY points = real + discovered
        for (u32 p : ptr_funcs)                        // merge in vtable/ptr entries
            if (p >= base && p < sec_end) funcs.push_back(p);
        for (u32 p : kForceEntry)                      // merge in forced indirect-call entries
            if (p >= base && p < sec_end) funcs.push_back(p);
        std::sort(funcs.begin(), funcs.end());
        funcs.erase(std::unique(funcs.begin(), funcs.end()), funcs.end());
        all_funcs.insert(all_funcs.end(), funcs.begin(), funcs.end());

        // For each entry, collect instructions until the next REAL function boundary or bl
        for (size_t fi = 0; fi < funcs.size(); fi++) {
            u32 faddr = funcs[fi];
            u32 fend  = next_func_boundary(faddr, real_funcs, sec_end);

            // Force full-CFG collection for specific functions even when the global linear mode is
            // on. Linear mode stops at the first unconditional branch, which TRUNCATES functions
            // whose body continues past one (forward branches then become tail_ppc → JIT bounce,
            // making the dropped blocks — and anything they bl — unreachable by recomp overrides).
            // These J2D draw functions must be whole so the 2D quad emitter they bl (0x802cd2ec) is
            // emitted as call_ppc and can be owned natively (the widescreen HUD layout, hud.cpp):
            //   0x802cc838 J2DPicture::drawFullSet — bl's the quad emitter from its (truncated) tail.
            //   0x801441e0 TGCConsole2::drawWater, 0x80144840 drawJuice — these draw the FLUDD blue
            //     gauge; hud.cpp wraps them to shift the gauge for widescreen and must regain control
            //     when they return. Linear-truncated, they'd tail_ppc out (siglongjmp) and never
            //     return to the override → its scope flag would leak and shift the whole screen. Whole
            //     (force-CFG) they end in blr and return cleanly.
            static const std::unordered_set<u32> kForceCFG = {
                0x802cc838u, 0x801441e0u, 0x80144840u,
                // Forced indirect-call entries (see kForceEntry above) — collect whole so linear
                // mode doesn't truncate them mid-body into a JIT bounce (defeats the purpose).
                0x8031d83cu, 0x8001fa88u, 0x80316ffcu, 0x803121acu, 0x8031a2ecu,
                0x803399ccu, 0x8031a50cu, 0x8030fe50u, 0x80313ddcu,
                0x803171ecu, 0x80311170u, 0x802b3264u,   // audio + card thread procs (3rd wave)
                // 4th wave thread entries + the draw-sync callback chain: collect whole (CFG) so
                // linear mode can't truncate them mid-body into a JIT bounce.
                0x802c54b8u, 0x802a9184u, 0x802a7878u, 0x802a7080u, 0x802b6fdcu,
                0x8001dcd0u, 0x8001fc04u, 0x800200d8u, 0x80296dd4u,
                0x802a9318u, 0x802a9078u,
                0x8035ce14u, 0x8035c334u,  // GX shared TEV dispatchers (perf)
            };

            // Collection (linear-truncate vs full-CFG) is extracted to func_collect.{h,cpp} and
            // unit-tested (tools/recompiler/tests) — its behaviour repeatedly broke assumptions.
            const bool use_cfg = getenv("SUNBRIGHT_CFG") || kForceCFG.count(faddr);
            func_instrs[faddr] = collect_function(data.data(), base, data.size(), faddr, fend, use_cfg);
        }
    }

    // Drop functions that must run on Dolphin's JIT (MSR/MMU/TLB/interrupt code).
    // They simply don't appear in the recomp table, so dispatch falls through to
    // the JIT for them.
    {
        size_t before = all_funcs.size();
        std::vector<u32> kept;
        kept.reserve(before);
        for (u32 addr : all_funcs) {
            if (function_needs_jit(func_instrs[addr]))
                func_instrs.erase(addr);
            else
                kept.push_back(addr);
        }
        all_funcs.swap(kept);
        std::cout << "Routed " << (before - all_funcs.size())
                  << " HW/privileged functions to Dolphin JIT\n";
    }

    std::cout << "Recompiling " << all_funcs.size() << " functions...\n";

    // Functions are emitted in address order so each output file covers one
    // contiguous region of code (≈ one library/module), which keeps related
    // functions together and lets the chunks compile in parallel.
    std::sort(all_funcs.begin(), all_funcs.end());

    // ── Shared header: forward declarations for every recompiled function ─────
    // Each functions_*.cpp and jump_table.cpp include this, so cross-file calls
    // resolve without repeating 6000+ declarations in every translation unit.
    {
        std::ofstream h(out_dir + "/functions.h");
        h << "// AUTO-GENERATED by sunbright-recomp — DO NOT EDIT\n"
             "#pragma once\n"
             "#include \"../runtime/cpu_state.h\"\n"
             "#include \"../runtime/intrinsics.h\"\n";
        h << "#include <cstdint>\n"
             "#include <cmath>\n\n";
        for (u32 addr : all_funcs)
            h << "extern \"C\" void func_" << std::hex << addr << "(CPUState&);\n";
    }

    // ── Function bodies, split into address-bucketed files ────────────────────
    // ~256 functions per file → a couple dozen files instead of one 13 MB blob.
    constexpr size_t kFuncsPerFile = 256;
    int total_unhandled = 0;
    std::set<std::string> unhandled_ops;
    std::vector<std::string> part_files;

    for (size_t start = 0; start < all_funcs.size(); start += kFuncsPerFile) {
        const size_t end = std::min(start + kFuncsPerFile, all_funcs.size());
        char namebuf[64];
        std::snprintf(namebuf, sizeof(namebuf), "functions_%08x.cpp", all_funcs[start]);
        const std::string part = namebuf;
        part_files.push_back(part);

        std::ofstream f(out_dir + "/" + part);
        f << "// AUTO-GENERATED by sunbright-recomp — DO NOT EDIT\n"
          << "// Functions 0x" << std::hex << all_funcs[start]
          << " .. 0x" << all_funcs[end - 1] << "\n"
          << "#include \"functions.h\"\n";

        CEmitter emitter(f);
        for (size_t fi = start; fi < end; fi++) {
            u32 addr = all_funcs[fi];
            EmitContext ctx;
            ctx.func_addr = addr;
            ctx.instrs    = func_instrs[addr];

            ctx.branch_targets = intra_branch_targets(ctx.instrs, addr);
            // Pull computed-`bctr` jump-table case labels into the function so they emit as in-function
            // `switch(ctr){goto}` rather than a `tail_ppc` handoff to Dolphin's JIT (the boot crash).
            if (!ctx.instrs.empty()) {
                const u32 fend = ctx.instrs.back().pc + 4;
                auto any_word = [&](u32 a, u32& out) -> bool {
                    for (const auto& s : dol.sections)
                        if (a >= s.addr && a + 4 <= s.addr + s.size) {
                            std::memcpy(&out, s.data.data() + (a - s.addr), 4);
                            out = __builtin_bswap32(out); return true;
                        }
                    return false;
                };
                ctx.jumptable_targets = jumptable_targets(ctx.instrs, addr, fend, any_word);
                ctx.branch_targets.insert(ctx.jumptable_targets.begin(), ctx.jumptable_targets.end());
            }
            emitter.emit_function(ctx);
        }
        total_unhandled += emitter.unhandled_count();
        unhandled_ops.insert(emitter.unhandled_mnemonics().begin(),
                             emitter.unhandled_mnemonics().end());
    }

    if (total_unhandled > 0) {
        std::cerr << "WARNING: " << total_unhandled << " unhandled instructions\n";
        for (const auto& mn : unhandled_ops)
            std::cerr << "  UNHANDLED: " << mn << "\n";
        std::cerr << "Add these to ppc_decoder.cpp + c_emitter.cpp\n";
    }

    // Write jump_table.cpp
    {
        std::ofstream f(out_dir + "/jump_table.cpp");
        CEmitter emitter(f);
        emitter.emit_jump_table(all_funcs);
    }

    std::cout << "Output written to " << out_dir << "/\n"
              << "  functions.h     (" << all_funcs.size() << " declarations)\n"
              << "  functions_*.cpp (" << part_files.size() << " files, "
              << all_funcs.size() << " functions)\n"
              << "  jump_table.cpp  (" << all_funcs.size() << " entries)\n";

    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { print_usage(argv[0]); return 1; }
    if (strcmp(argv[1], "--version") == 0) { print_version(); return 0; }

    std::string disc_path = argv[1];
    bool analyze_only = false;
    bool dump_dol     = false;
    bool dump_funcs   = false;
    bool do_disasm    = false;
    u32  disasm_addr  = 0;
    int  disasm_count = 64;
    bool do_xref      = false;   // --xref ADDR  : who directly branches to ADDR
    bool do_callees   = false;   // --callees ADDR : what the function at ADDR directly calls
    u32  xref_addr    = 0;
    std::string funcs_path = "reference/sms_gmse01_funcs.txt";
    std::string out_dir;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--analyze-only") == 0) analyze_only = true;
        if (strcmp(argv[i], "--dump-dol")     == 0) dump_dol = true;
        if (strcmp(argv[i], "--dump-funcs")   == 0) dump_funcs = true;
        if (strcmp(argv[i], "--output") == 0 && i+1 < argc) out_dir = argv[++i];
        if (strcmp(argv[i], "--disasm") == 0 && i+1 < argc) {
            do_disasm = true;
            disasm_addr = (u32)strtoul(argv[++i], nullptr, 16);
            if (i + 1 < argc && argv[i+1][0] != '-') disasm_count = atoi(argv[++i]);
        }
        if (strcmp(argv[i], "--xref") == 0 && i+1 < argc) {
            do_xref = true; xref_addr = (u32)strtoul(argv[++i], nullptr, 16);
        }
        if (strcmp(argv[i], "--callees") == 0 && i+1 < argc) {
            do_callees = true; xref_addr = (u32)strtoul(argv[++i], nullptr, 16);
        }
        if (strcmp(argv[i], "--funcs") == 0 && i+1 < argc) funcs_path = argv[++i];
    }

    std::cout << "Loading disc: " << disc_path << "\n";
    DiscLoader disc(disc_path);
    if (!disc.is_open()) {
        std::cerr << "Failed to open disc image\n";
        return 1;
    }

    std::cout << "Extracting DOL...\n";
    auto dol_bytes = disc.load_dol();
    if (dol_bytes.empty()) {
        std::cerr << "Failed to extract DOL\n";
        return 1;
    }
    DOL dol = DOL::from_bytes(dol_bytes);

    if (dump_dol) {
        if (out_dir.empty()) out_dir = "scratch/bin/sms.dol";
        if (auto p = std::filesystem::path(out_dir).parent_path(); !p.empty())
            std::filesystem::create_directories(p);
        std::ofstream f(out_dir, std::ios::binary);
        f.write((char*)dol_bytes.data(), dol_bytes.size());
        std::cout << "DOL written to " << out_dir << " (" << dol_bytes.size() << " bytes)\n";
        return 0;
    }

    if (do_disasm) {
        for (const auto& [base, data] : dol.text_sections()) {
            if (disasm_addr < base || disasm_addr >= base + data.size()) continue;
            for (int k = 0; k < disasm_count; k++) {
                u32 a = disasm_addr + k * 4, off = a - base;
                if (off + 4 > data.size()) break;
                u32 w_be; std::memcpy(&w_be, data.data() + off, 4);
                u32 w = __builtin_bswap32(w_be);
                PPCInstr ins = decode(w, a);
                std::printf("%08x: %08x  %s\n", a, w, ins.mnemonic().c_str());
            }
            return 0;
        }
        std::cerr << "address not in any text section\n";
        return 1;
    }

    // ── Static cross-reference (durable RE tool) ────────────────────────────────────────────
    // --xref ADDR    : every direct branch (bl / b / bc) in the DOL whose target == ADDR, named by
    //                  the function that CONTAINS the call site (FuncName+0xNN). Answers "who calls X
    //                  / what gates X" without hand-decoding branch displacements.
    // --callees ADDR : every direct branch OUT of the function at ADDR (to its next-func boundary),
    //                  named by target. Answers "what does this function call".
    // Names come from --funcs (default reference/sms_gmse01_funcs.txt): "<hexaddr> <symbol>" per line.
    if (do_xref || do_callees) {
        std::vector<std::pair<u32, std::string>> syms;   // sorted by addr
        {
            std::ifstream f(funcs_path);
            if (!f) { std::cerr << "could not open funcs file: " << funcs_path << "\n"; return 1; }
            std::string line;
            while (std::getline(f, line)) {
                if (line.empty()) continue;
                char* end = nullptr;
                u32 a = (u32)strtoul(line.c_str(), &end, 16);
                if (end == line.c_str()) continue;
                while (*end == ' ' || *end == '\t') end++;
                syms.emplace_back(a, std::string(end));
            }
            std::sort(syms.begin(), syms.end());
        }
        // Largest symbol addr <= a → "name+0xNN" (or raw hex if before the first symbol).
        auto name_of = [&](u32 a) -> std::string {
            auto it = std::upper_bound(syms.begin(), syms.end(), a,
                        [](u32 v, const std::pair<u32,std::string>& p){ return v < p.first; });
            char buf[160];
            if (it == syms.begin()) { std::snprintf(buf, sizeof buf, "%08x", a); return buf; }
            --it;
            if (it->first == a) std::snprintf(buf, sizeof buf, "%s", it->second.c_str());
            else                std::snprintf(buf, sizeof buf, "%s+0x%x", it->second.c_str(), a - it->first);
            return buf;
        };
        auto is_direct_branch = [](const PPCInstr& i) {
            return i.op == PPCOp::B || i.op == PPCOp::BC;   // bl/b/ba/bla = B(+lk); bc family = BC
        };
        if (do_xref) {
            std::printf("xref: callers of %08x (%s)\n", xref_addr, name_of(xref_addr).c_str());
            int n = 0;
            for (const auto& [base, data] : dol.text_sections()) {
                for (size_t off = 0; off + 4 <= data.size(); off += 4) {
                    u32 w_be; std::memcpy(&w_be, data.data() + off, 4);
                    u32 a = base + (u32)off;
                    PPCInstr ins = decode(__builtin_bswap32(w_be), a);
                    if (is_direct_branch(ins) && ins.target == xref_addr) {
                        std::printf("  %08x  %-4s  in %s\n", a,
                                    ins.lk ? (ins.op == PPCOp::BC ? "bcl" : "bl")
                                           : (ins.op == PPCOp::BC ? "bc"  : "b"),
                                    name_of(a).c_str());
                        n++;
                    }
                }
            }
            std::printf("  (%d caller%s)\n", n, n == 1 ? "" : "s");
            return 0;
        }
        // --callees: scan [xref_addr, next-symbol) for direct branches out, named by target.
        u32 end_addr = 0xFFFFFFFFu;
        {
            auto it = std::upper_bound(syms.begin(), syms.end(), xref_addr,
                        [](u32 v, const std::pair<u32,std::string>& p){ return v < p.first; });
            if (it != syms.end()) end_addr = it->first;
        }
        std::printf("callees: direct branches out of %s  [%08x,%08x)\n",
                    name_of(xref_addr).c_str(), xref_addr, end_addr);
        int n = 0;
        for (const auto& [base, data] : dol.text_sections()) {
            if (xref_addr < base || xref_addr >= base + data.size()) continue;
            for (u32 a = xref_addr; a < end_addr; a += 4) {
                u32 off = a - base;
                if (off + 4 > data.size()) break;
                u32 w_be; std::memcpy(&w_be, data.data() + off, 4);
                PPCInstr ins = decode(__builtin_bswap32(w_be), a);
                if (is_direct_branch(ins)) {
                    std::printf("  %08x  %-4s -> %s\n", a,
                                ins.lk ? (ins.op == PPCOp::BC ? "bcl" : "bl")
                                       : (ins.op == PPCOp::BC ? "bc"  : "b"),
                                name_of(ins.target).c_str());
                    n++;
                }
            }
            break;
        }
        std::printf("  (%d branch%s)\n", n, n == 1 ? "" : "es");
        return 0;
    }

    if (dump_funcs) {
        // Emit every discovered function as "addr size" (size = gap to next start).
        // Accurate boundaries (find_functions follows bl targets + prologues), for
        // reliable JP→USA symbol porting by function-size-sequence fingerprinting.
        for (const auto& [base, data] : dol.text_sections()) {
            auto funcs = find_functions(data.data(), base, data.size());
            for (size_t i = 0; i < funcs.size(); i++) {
                u32 end = (i + 1 < funcs.size()) ? funcs[i + 1] : base + (u32)data.size();
                std::printf("%08x %x\n", funcs[i], end - funcs[i]);
            }
        }
        return 0;
    }

    if (analyze_only) {
        analyze_mode(disc, dol);
        return 0;
    }

    if (out_dir.empty()) {
        std::cerr << "Specify --output <directory>\n";
        return 1;
    }

    return recompile_mode(disc, dol, out_dir);
}
