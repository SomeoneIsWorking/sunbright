#include <cstdio>
#include "disc_loader.h"
#include "dol_parser.h"
#include "ppc_decoder.h"
#include "c_emitter.h"

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
    if (spr >= SPR_GQR0 && spr <= SPR_GQR0 + 7) return true;
    return false;
}

static bool function_needs_jit(const std::vector<PPCInstr>& instrs) {
    for (const auto& i : instrs) {
        switch (i.op) {
        case PPCOp::MTMSR:
        case PPCOp::RFI:
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
    std::unordered_set<u32> ptr_funcs;
    for (const auto& s : dol.sections) {
        for (u32 off = 0; off + 4 <= s.size; off += 4) {
            u32 v; std::memcpy(&v, s.data.data() + off, 4); v = __builtin_bswap32(v);
            if (v & 3) continue;
            u32 w, prev;
            if (!text_word(v, w)) continue;          // points into .text?
            if (w == 0 || is_term(w)) continue;      // first insn must be real code
            bool boundary = is_section_start(v) || (text_word(v - 4, prev) && is_term(prev));
            if (boundary) ptr_funcs.insert(v);
        }
    }
    std::printf("Pointer-referenced function candidates: %zu\n", ptr_funcs.size());

    // Collect all functions from text sections
    for (const auto& [base, data] : dol.text_sections()) {
        auto funcs = find_functions(data.data(), base, data.size());
        for (u32 p : ptr_funcs)                       // merge in vtable/ptr entries
            if (p >= base && p < base + data.size()) funcs.push_back(p);
        std::sort(funcs.begin(), funcs.end());
        funcs.erase(std::unique(funcs.begin(), funcs.end()), funcs.end());
        all_funcs.insert(all_funcs.end(), funcs.begin(), funcs.end());

        // For each function, collect instructions until next function or bl
        for (size_t fi = 0; fi < funcs.size(); fi++) {
            u32 faddr = funcs[fi];
            u32 fend  = (fi + 1 < funcs.size()) ? funcs[fi + 1] : (base + (u32)data.size());
            fend = std::min(fend, base + (u32)data.size());

            auto& instrs = func_instrs[faddr];
            for (u32 addr = faddr; addr < fend; addr += 4) {
                u32 off = addr - base;
                u32 w_be;
                std::memcpy(&w_be, data.data() + off, 4);
                u32 w = __builtin_bswap32(w_be);
                instrs.push_back(decode(w, addr));
                if (is_unconditional_branch(instrs.back())) {
                    addr += 4;  // include the delay-slot (PPC has none, so just stop)
                    break;
                }
            }
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
             "#include \"../runtime/intrinsics.h\"\n"
             "#include <cstdint>\n"
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

            u32 func_end = addr + (u32)(ctx.instrs.size() * 4);
            for (const auto& instr : ctx.instrs) {
                u32 tgt = branch_target(instr);
                if (tgt != 0 && tgt >= addr && tgt < func_end)
                    ctx.branch_targets.insert(tgt);
                if ((instr.op == PPCOp::BC) && instr.target != 0
                    && instr.target >= addr && instr.target < func_end)
                    ctx.branch_targets.insert(instr.target);
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
        if (out_dir.empty()) out_dir = "/tmp/sms.dol";
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
