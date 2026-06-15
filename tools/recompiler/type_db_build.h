#pragma once
#include "type_recovery.h"   // TypeDB, EngineLayout
#include <map>
#include <set>
#include <string>
#include <vector>

// =============================================================================
// type_db_build — assemble a tailored-recomp TypeDB for a set of ACTIVE engine types directly
// from the decomp tree: resolve each type's header, COMPOSE its full layout across the
// single-inheritance base chain, and build the this/param signature seeds from the symbol file.
// This is the piece that lets the recompiler target REAL engine types (handoff step 2).
//
// Inheritance composition is sound for SINGLE inheritance (the dominant SMS case): the decomp
// annotates every class's own fields at OBJECT-ABSOLUTE offsets (a base subobject sits at offset
// 0, so a base header's `/* 0xN */` is already the object offset). So the full layout is just the
// UNION, keyed by annotated offset, of the type's own fields and every base's fields. Multiple /
// virtual inheritance is NOT handled (rare here) — flagged via `unresolved_bases`.
//
// Portable C++17: text + filesystem walking, no host-arch assumptions.
// =============================================================================

struct TypeDBBuildResult {
    TypeDB                   db;
    std::vector<std::string> missing_types;     // active types whose header wasn't found
    std::vector<std::string> unresolved_bases;  // base names referenced but not located
    int                      header_count = 0;  // headers indexed
    // type name -> defining-header PATH (as walked from include_dir). For a port-side bridge
    // codegen TU to #include the decomp headers and resolve host struct defs/sizes by name.
    std::map<std::string, std::string> type_headers;
};

// Index type name -> defining header path by scanning `include_dir` recursively.
std::map<std::string, std::string> index_headers(const std::string& include_dir);

// Compose one type's full layout (own + base chain) using a prebuilt header index. `engine_types`
// decides which pointer fields get a nested type (so chained access stays typed). Appends any
// base name it couldn't resolve to `unresolved`.
EngineLayout compose_layout(const std::string& type_name,
                            const std::map<std::string, std::string>& index,
                            const std::set<std::string>& engine_types,
                            std::vector<std::string>* unresolved = nullptr);

// Build the full DB for `active_types`: composed layouts + signatures (from `funcs_txt_path`,
// filtered to the active set).
TypeDBBuildResult build_type_db(const std::set<std::string>& active_types,
                                const std::string& include_dir,
                                const std::string& funcs_txt_path);
