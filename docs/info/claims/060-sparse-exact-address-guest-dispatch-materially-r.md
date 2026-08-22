---
id: C060
kind: claim
status: holds
created: 2026-08-22
tags: performance,native60
depends: sms-recomp/runtime/guest_address_table.h, sms-recomp/runtime/rt_core.cpp#lookup, sms-recomp/overrides/overrides.cpp#override_lookup
---

## Claim

Sparse exact-address guest dispatch materially reduces Native-60 CPU overhead, but does not bring the heavy Delfino tick under budget.

## Evidence

Before perf samples attributed 5.26% to call_ppc and 4.39% to override_lookup; after samples attributed 2.71% and 1.25%, with zero lost samples. Full follow-up ticks measured roughly 20.5-21.2 ms versus the prior 22.3 ms baseline. See debug_journal/2026-08-22_native60_dispatch_optimization.md.

## What would falsify it

A controlled same-scene alternating profile shows no reduction in combined call_ppc and override_lookup samples, the direct table resolves a different owner than the original exact lookup, or a complete heavy tick reaches 16.67 ms without further changes.
