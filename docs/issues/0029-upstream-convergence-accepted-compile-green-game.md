---
id: 29
title: Upstream convergence accepted compile-green gameplay regressions
status: resolved
symptom: The convergence tool adopted 135 upstream files, built successfully, then crashed during bounded gameplay and silently removed six more fixes outside that stage.
tags: decomp,upstream,convergence,verification
created: 2026-08-30
updated: 2026-08-30
---

## Root cause

The convergence algorithm accepted a candidate group after compilation and ran only one smoke test
after all adoption. Compilation cannot detect host undefined behavior, guest-layout corruption,
missing factory cases, or completed behavior that is not exercised by the selected stage. Existing
keep classification also lacked an explicit marker for verified fixes without a platform guard.

## Resolution

The tool now proves a known-good gameplay baseline, runs the bounded decomp gameplay smoke after
every build-green candidate group, and recursively bisects runtime failures. `SUNBRIGHT-KEEP` marks
verified local behavior that must never be offered for convergence. Commit-history and removed-line
review restored thirteen unsafe replacements; 122 equivalent upstream files remain adopted. The
corrected tree reached Delfino Plaza and completed all 400 diagnostic frames under the GPU watcher.
