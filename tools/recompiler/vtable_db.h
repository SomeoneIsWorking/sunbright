#pragma once
#include "dol_parser.h"      // DOL, u32
#include <map>
#include <string>
#include <vector>

// =============================================================================
// vtable_db — the per-engine-type VTABLE SLOT database for offset-0
// virtual-dispatch-on-handle routing (docs/ARCHITECTURE_TARGET.md function-call
// boundary; handoff "OFFSET-0 VIRTUAL-DISPATCH-ON-HANDLE ROUTING" step 1).
//
// A virtual call in recompiled game code is `lwz vt,0(rA); lwz m,N(vt); mtctr m; bctrl`.
// When rA is an engine HANDLE (a host object held as a 32-bit token), we must NOT do the
// guest vtable load; we must call the HOST virtual method. To recognize the chain and route
// it we need, per engine type T, the map {vtable byte-offset N -> which method}.
//
// CONSTRUCTION is DOL-ANCHORED (the make-or-break validation the handoff demanded is the
// construction method itself, not a separate check): for type T we
//   1. find T's constructor symbol and disassemble it to read the vptr it stores at 0(this)
//      (`lis/addi` immediate -> `stw imm, 0(this)`), which is the guest vtable address;
//   2. read the consecutive function-pointer words from that vtable out of the DOL, skipping
//      the CodeWarrior header words (offset-to-top + RTTI, both 0 for the primary vtable);
//   3. resolve each slot's target guest address back to a symbol -> demangled class::method.
// The decomp header's ordered `virtual` list (decomp_parse) is kept as an independent
// CROSS-CHECK (declaration order == slot order in CodeWarrior single inheritance).
//
// EMISSION needs only the slot's METHOD NAME + signature: the recompiled game calls a
// generated thunk `((T*)sb_eng_host(handle))->method(args)` and the HOST C++ compiler resolves
// the real override via host virtual dispatch — so slot numbers are only the recognition key.
//
// Portable C++17: byte reads + text processing, no host-arch / endianness assumptions
// (DOL words are byte-swapped on load by dol_parser).
// =============================================================================

struct VSlot {
    int         byte_off = 0;     // byte offset from the stored vptr (e.g. 0x10 for J3DModel::calc)
    int         slot_index = 0;   // logical slot ((byte_off - header_size) / 4)
    u32         target = 0;       // guest address of the function this slot points at
    std::string method;           // method leaf name from the target symbol ("calc", "~T", "")
    std::string defining_class;   // class leaf the target method belongs to ("" if unresolved)
    std::string mangled;          // the raw target symbol ("" if not in the map)
};

struct VTable {
    bool               found = false;
    std::string        type;          // engine type leaf name
    u32                vptr = 0;       // stored vptr value (CodeWarrior vtable header start)
    int                header_size = 0;// bytes skipped before slot 0 (8 for the primary vtable)
    std::vector<VSlot> slots;          // in byte-offset order

    // The slot whose method leaf == `method` (first match), or nullptr.
    const VSlot* find_method(const std::string& m) const {
        for (const auto& s : slots) if (s.method == m) return &s;
        return nullptr;
    }
    // The slot at byte offset N, or nullptr.
    const VSlot* find_offset(int n) const {
        for (const auto& s : slots) if (s.byte_off == n) return &s;
        return nullptr;
    }
};

struct VTableDB {
    std::map<std::string, VTable> tables;   // type leaf -> vtable
};

// Build vtables for `active_types` (leaf names) from the DOL + symbol file. `funcs_txt_path`
// is `<hexaddr> <mangled>` lines (reference/sms_gmse01_funcs.txt). Types whose ctor / vtable
// can't be located are simply absent from the result (found=false), never guessed.
VTableDB build_vtable_db(const std::vector<std::string>& active_types,
                         const DOL& dol,
                         const std::string& funcs_txt_path);
