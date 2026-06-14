#pragma once
#include "ppc_decoder.h"   // u32
#include <map>
#include <set>
#include <string>
#include <vector>

// =============================================================================
// func_sig — recover a function's `this`/parameter ENGINE types from its symbol, to SEED
// type recovery (type_recovery.h TypeDB::signatures). This is the other DB input besides the
// field layouts (decomp_parse): which registers hold engine-object pointers on entry.
//
// SMS symbols (reference/sms_gmse01_funcs.txt) use the GNU-v2 / CodeWarrior C++ mangling,
// e.g. `loadDefault__8TRealoidFR20JSUMemoryInputStreamPCci`
//        = TRealoid::loadDefault(JSUMemoryInputStream&, const char*, int).
// We parse just enough to know: the class (the `this` pointer in r3) and which parameters are
// pointers/references to a class (which land in r4.. by the PPC-EABI GPR sequence — float/
// double args go to FPRs and do NOT consume a GPR). Only sites whose type is a KNOWN engine
// type are seeded; everything else is left for the recovery pass to find from dataflow.
//
// Portable C++17: pure string processing.
// =============================================================================

struct SigArg {
    int         gpr;        // EABI GPR index (3..10) holding this pointer/reference
    std::string type;       // engine type LEAF name (matches the layout DB keys)
    std::string qualified;  // namespaced form, e.g. "JDrama::TGraphics"
};

struct FuncSig {
    bool                 ok = false;          // the symbol parsed cleanly
    bool                 is_method = false;   // has a class scope (a `this` in r3)
    std::string          class_name;          // qualified `this` type ("" for free functions)
    std::string          class_leaf;          // leaf of class_name
    std::vector<SigArg>  ptr_args;            // pointer/ref-to-class args (incl. `this` at r3)
    bool                 ambiguous = false;   // hit a by-value struct / backref: GPRs past it
                                              // are NOT assigned (honest: not seeded, not guessed)
};

// Parse one GNU-v2 mangled symbol. ptr_args lists EVERY pointer/ref-to-class argument with its
// resolved GPR index (the `this` pointer included when is_method); the caller filters to the
// engine types it knows.
FuncSig demangle_signature(const std::string& mangled);

// Build TypeDB::signatures from a `<hexaddr> <mangled>` symbol file, keeping only sites whose
// type leaf is in `engine_types`. Returns guest addr -> {gpr index -> engine type leaf}.
std::map<u32, std::map<int, std::string>>
build_signatures(const std::string& funcs_txt_path, const std::set<std::string>& engine_types);
