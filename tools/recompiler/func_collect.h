#pragma once
#include "ppc_decoder.h"
#include <cstdint>
#include <cstddef>
#include <functional>
#include <unordered_set>
#include <vector>

// Function instruction collection — the step that decides which instructions belong to a function
// and therefore whether a branch becomes an intra-function `goto`, a `call_ppc`, or a `tail_ppc`.
// Extracted from main.cpp so it can be unit-tested (see tools/recompiler/tests): the recompiler's
// behaviour here repeatedly surprised assumptions, so it is covered by tests.
//
// `data`/`base`/`size` describe the loaded big-endian code image (data[0] is at guest addr `base`).
// [faddr, fend) bound the function (fend is normally the next function's address).
//
//   cfg == false (legacy/default LINEAR): decode straight through from faddr until the FIRST
//     unconditional branch, then stop. Functions whose body continues past one are TRUNCATED — their
//     later blocks fall outside the returned set, so forward branches into them become tail_ppc.
//   cfg == true (full CFG): walk the control-flow graph from faddr, following every branch target
//     that stays within [faddr, fend), so the whole function is collected and intra-function
//     branches become gotos.  (The legacy mode is what made e.g. J2DPicture::drawFullSet truncate.)
std::vector<PPCInstr> collect_function(const uint8_t* data, uint32_t base, size_t size,
                                       uint32_t faddr, uint32_t fend, bool cfg);

// Build the set of intra-function branch-target labels for an EmitContext, given the collected
// instruction list. A target counts as intra-function when it lies in [faddr, last_instr_pc + 4).
std::unordered_set<uint32_t> intra_branch_targets(const std::vector<PPCInstr>& instrs, uint32_t faddr);

// Discover computed-`bctr` jump-table case targets that land INSIDE this function [faddr, fend).
// A `bctr` whose CTR is loaded from a `base[index*4]` table is a switch: its case labels are reached
// ONLY through the indirect branch, so they are not in intra_branch_targets and the emitter would
// render the `bctr` as a `tail_ppc` handoff to Dolphin's JIT — corrupting non-volatile regs across
// the recomp↔JIT boundary (the boot render crash). We pattern-match `cmpli idx,N; … ; lis/addi base;
// lwzx/lwz ctr,[base+idx]; mtctr; bctr`, read N+1 table entries via `read_word` (reads any loaded
// section), and return the in-function targets so they become labels + a `switch(ctr){goto}`.
std::unordered_set<uint32_t> jumptable_targets(
    const std::vector<PPCInstr>& instrs, uint32_t faddr, uint32_t fend,
    const std::function<bool(uint32_t, uint32_t&)>& read_word);

// fend for an entry = the next REAL function boundary strictly greater than `faddr` (capped at
// `cap` = section end). `real_funcs` must be sorted ascending and contain ONLY genuine function
// starts (symbol/heuristic boundaries), NOT pointer-discovered interior labels. Using the next
// real boundary — rather than the next entry in the discovery-augmented list — keeps a discovered
// interior label from shrinking its containing function's fend and re-truncating it; the label
// instead collects to the end of that function as a valid alternate entry point.
uint32_t next_func_boundary(uint32_t faddr, const std::vector<uint32_t>& real_funcs, uint32_t cap);
