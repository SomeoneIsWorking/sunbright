#pragma once
#include "abi_layout.h"        // FKind, LField
#include "type_recovery.h"     // EngineLayout
#include <optional>
#include <set>
#include <string>
#include <vector>

// =============================================================================
// decomp_parse — extract an engine type's ordered FIELD LIST from a decomp header
// (reference/sms/include/...), the front end of building the tailored-recomp type DB
// (docs/ARCHITECTURE_TARGET.md, handoff step 1).
//
// The SMS decomp annotates every real member's GUEST offset inline, in one of two styles:
//     /* 0x18 */ s16 mXAngleMin;          (leading block comment — the dominant style)
//     int x1; // _00                       (trailing line comment)
// Those annotations are the authoritative guest offsets (and the oracle that
// abi_layout's computed offsets must reproduce). This is a FOCUSED parser for the
// regular shapes the decomp uses (one annotated member per line) — NOT a C++ frontend.
// It deliberately recognises only what it can size/place soundly and is HONEST about the
// rest (a field it can't classify is marked unsupported, never silently dropped), because
// in the tailored build a missed engine-field site is a correctness bug, not a safe
// fallback (type_recovery.h SHARP EDGE).
//
// Portable C++17: pure text processing, no host-arch/endianness assumptions.
// =============================================================================

struct ParsedField {
    int         offset = -1;       // annotated guest offset (authoritative)
    std::string type;              // type text as written, e.g. "JGeometry::TVec3<f32>"
    std::string name;              // member name
    std::string pointee;           // for a pointer field: the pointee type (no '*'/const), else ""
    FKind       kind = FKind::U32; // element kind (valid only when `sizable`)
    int         array_count = 1;   // >1 for arrays
    bool        is_pointer = false;
    bool        sizable = false;   // kind/array_count are a sound size model (scalar/ptr/array
                                   // of those) — i.e. this field can feed abi_layout. Embedded
                                   // value types / enums / unknown types are NOT sizable.
};

struct ParsedType {
    std::string              name;            // the class/struct name requested
    std::string              base;            // first base class ("" if none)
    bool                     polymorphic = false; // declares a `virtual` member
    std::vector<std::string> virtuals;        // virtual METHOD names in declaration order, e.g.
                                              // {"update","entry","calc","viewCalc","~J3DModel"}.
                                              // CodeWarrior lays a class's NEW virtuals into the
                                              // vtable in this order (after the base chain's).
    std::set<std::string>    simple_virtuals; // subset of `virtuals` that are `void name()` —
                                              // void return AND zero args, NOT a destructor.
                                              // ONLY these are safely host-dispatchable as
                                              // `((T*)h)->name()` with no arg/return marshalling.
    std::vector<ParsedField> fields;          // in declaration order
    bool                     found = false;   // the type was located in the text
    bool                     fully_sizable = false; // every field is `sizable` AND no base
                                                    // (so the whole type can feed abi_layout)
};

// Parse class/struct `type_name` out of header `text`. Returns found=false if not present.
ParsedType parse_decomp_type(const std::string& text, const std::string& type_name);

// Convenience: read the file at `path` and parse `type_name` from it.
ParsedType parse_decomp_file(const std::string& path, const std::string& type_name);

// Build the abi_layout field list for a fully-sizable type (asserts none if !fully_sizable).
std::vector<LField> to_lfields(const ParsedType& t);

// Build the EngineLayout (guest offset -> {host member, nested engine type}) the recovery
// pass consumes, straight from the annotated offsets. A pointer field gets nested_type set
// only when its pointee is in `engine_types` (so loads through it stay typed); otherwise "".
EngineLayout to_engine_layout(const ParsedType& t, const std::set<std::string>& engine_types);
