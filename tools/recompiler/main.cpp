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

    std::vector<u32> all_funcs;
    std::unordered_map<u32, std::vector<PPCInstr>> func_instrs;

    // Collect all functions from text sections
    for (const auto& [base, data] : dol.text_sections()) {
        auto funcs = find_functions(data.data(), base, data.size());
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

    std::cout << "Recompiling " << all_funcs.size() << " functions...\n";

    // Write functions.cpp
    {
        std::ofstream f(out_dir + "/functions.cpp");
        CEmitter emitter(f);
        emitter.emit_header();

        // Forward declarations
        for (u32 addr : all_funcs)
            f << "extern \"C\" void func_" << std::hex << addr << "(CPUState&);\n";
        f << "\n";

        // Emit each function
        for (u32 addr : all_funcs) {
            EmitContext ctx;
            ctx.func_addr = addr;
            ctx.instrs    = func_instrs[addr];

            // Compute function address range
            u32 func_end = addr + (u32)(ctx.instrs.size() * 4);

            // Label any branch target that lands inside this function's range
            for (const auto& instr : ctx.instrs) {
                u32 tgt = branch_target(instr);
                if (tgt != 0 && tgt >= addr && tgt < func_end)
                    ctx.branch_targets.insert(tgt);
                // Also label BC targets in range
                if ((instr.op == PPCOp::BC) && instr.target != 0
                    && instr.target >= addr && instr.target < func_end)
                    ctx.branch_targets.insert(instr.target);
            }

            emitter.emit_function(ctx);
        }

        if (emitter.unhandled_count() > 0) {
            std::cerr << "WARNING: " << emitter.unhandled_count()
                      << " unhandled instructions\n";
            std::set<std::string> uniq_ops(emitter.unhandled_mnemonics().begin(),
                                           emitter.unhandled_mnemonics().end());
            for (const auto& mn : uniq_ops)
                std::cerr << "  UNHANDLED: " << mn << "\n";
            std::cerr << "Add these to ppc_decoder.cpp + c_emitter.cpp\n";
        }
    }

    // Write jump_table.cpp
    {
        std::ofstream f(out_dir + "/jump_table.cpp");
        CEmitter emitter(f);
        emitter.emit_jump_table(all_funcs);
    }

    std::cout << "Output written to " << out_dir << "/\n"
              << "  functions.cpp   (" << all_funcs.size() << " functions)\n"
              << "  jump_table.cpp  (" << all_funcs.size() << " entries)\n";

    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { print_usage(argv[0]); return 1; }
    if (strcmp(argv[1], "--version") == 0) { print_version(); return 0; }

    std::string disc_path = argv[1];
    bool analyze_only = false;
    bool dump_dol     = false;
    std::string out_dir;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--analyze-only") == 0) analyze_only = true;
        if (strcmp(argv[i], "--dump-dol")     == 0) dump_dol = true;
        if (strcmp(argv[i], "--output") == 0 && i+1 < argc) out_dir = argv[++i];
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
