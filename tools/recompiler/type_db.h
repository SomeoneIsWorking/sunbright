#pragma once
#include "abi_layout.h"
#include "type_recovery.h"   // EngineLayout, FieldDesc
#include <vector>

// type_db — build a tailored-recomp type DB (the layouts recover_eng_fields consumes) from
// engine-type FIELD LISTS, with GUEST offsets computed by abi_layout (GameCube ABI) rather
// than hand-entered. The host member NAMES carry through to emission (the emitter resolves
// host offsets via the names); the guest offsets are what the recompiled game reads, so the
// DB only needs guest-offset -> {member, nested_type}.
//
// This connects the three pieces — abi_layout (offsets) -> type_db (layouts) ->
// type_recovery (eng_fields) -> emitter — into one flow. The remaining gap to full
// automation is extracting the LField lists from the decomp headers (handoff step 1).
//
// Portable C++17.

// Build one engine type's layout from its ordered field list. `polymorphic` adds the
// implicit vtable slot at offset 0 (GameCube single-inheritance vtable pointer).
EngineLayout build_engine_layout(const std::vector<LField>& fields, bool polymorphic);
