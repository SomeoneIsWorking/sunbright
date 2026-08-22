---
id: C061
kind: claim
status: holds
created: 2026-08-22
tags: performance,native60
depends: sms-recomp/runtime/intrinsics.h#call_ppc_direct, sms-recomp/runtime/rt_core.cpp#resolve_ppc_target, tools/recompiler/c_emitter.cpp
---

## Claim

Caching the final override-aware target per compile-time PPC address materially reduces guest dispatch resolution cost, but does not by itself put heavy Native-60 ticks under 16.67 ms.

## Evidence

The same stage-1 perf method measured resolve_ppc_target 0.67%, override_lookup 0.49%, and call_ppc 0.06% (1.22% combined, zero lost samples), down from the sparse-table baseline of 3.96%. Regeneration emitted 84,148 direct sites using program-wide per-target slots and retained 7,409 general-dispatch sites for true indirect transfers. A 700-present run-safe control exited cleanly with zero amdgpu faults; complete heavy ticks remain roughly 20.5-21.2 ms. See debug_journal/2026-08-22_native60_dispatch_optimization.md.

## What would falsify it

A controlled same-scene profile no longer shows lower combined target-resolution samples, a cached direct site resolves a different override/function than the general dispatcher, or this change alone is shown to bring the complete heavy tick to 16.67 ms.
