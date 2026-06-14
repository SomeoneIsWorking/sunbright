#pragma once
#include "ppc_decoder.h"   // u32 etc.
#include <string>
#include <vector>

// =============================================================================
// abi_layout — compute struct field offsets under the GameCube (guest) ABI and the
// host ABI from one ordered field list. This is the mechanical core of building the
// tailored-recomp TYPE DB (type_recovery.h TypeDB): the recompiled game reads a field by
// its GUEST offset, the host engine object stores it at its HOST offset, and the DB pairs
// the two. They diverge wherever a pointer precedes a field (guest pointer = 4 bytes,
// host = 8) or wherever 8-byte alignment shifts things.
//
// GameCube CodeWarrior / PPC-EABI layout rules (default, no #pragma pack), which the
// decomp's annotated `// _XX` offsets follow:
//   * each scalar is placed at the next offset that is a multiple of its natural
//     alignment (i8=1, i16=2, i32/f32/ptr=4, f64/i64=8);
//   * a polymorphic class gets a vtable pointer (ptr-sized) at offset 0, before members;
//   * struct alignment = max member alignment; total size = rounded up to that.
// The ONLY host/guest difference modelled is pointer size/alignment (4 vs 8) — everything
// else is identical on x86-64 and arm64 (both LP64, same scalar sizes/alignments).
//
// Portable C++17, no host-arch assumptions: pure arithmetic. Verified against the decomp's
// real annotations (JUTRect) AND against the host compiler via offsetof (abi_layout_test).
// =============================================================================

enum class FKind { I8, U8, I16, U16, I32, U32, I64, U64, F32, F64, Ptr };

struct LField {
    std::string name;
    FKind kind;
    std::string nested_type;   // if kind==Ptr and it points at an engine object, its type; else ""
    int array_count = 1;       // >1 for an array member; the element kind is `kind`
};

struct ABIOpts {
    int ptr_size;   // 4 = guest (GameCube), 8 = host (LP64 x86-64 / arm64)
    int ptr_align;  // == ptr_size for both ABIs we target
};
inline ABIOpts guest_abi() { return ABIOpts{ 4, 4 }; }
inline ABIOpts host_abi()  { return ABIOpts{ 8, 8 }; }

struct LResult {
    std::vector<int> offsets;  // one per field (NOT counting the implicit vtable slot)
    int size;
    int align;
};

// Compute field offsets. If `polymorphic`, a vtable pointer occupies [0, ptr_size) first
// and members follow (their offsets reflect that). NOTE: the GameCube CodeWarrior ABI does
// NOT always put the vtable at offset 0 — a class introducing virtuals over a non-polymorphic
// base appends it at the END instead (see docs/re_notes/abi_findings.md). So `polymorphic`
// models only the vtable-at-0 case; vtable placement is otherwise resolved from the decomp.
LResult compute_layout(const std::vector<LField>& fields, bool polymorphic, const ABIOpts& o);

// One field's natural size and alignment under ABI `o` (element size × array count for
// arrays; alignment is the element's). Exposed so callers can validate annotated offsets
// (alignment / non-overlap) without recomputing a full layout.
void field_size_align(const LField& f, const ABIOpts& o, int& size, int& align);
